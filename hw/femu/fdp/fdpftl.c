#include "fdpftl.h"
#include "fdp.h"

//#define FEMU_DEBUG_FTL

#define FEMU_FDP_THRES_PCENT 0.75
#define FEMU_FDP_THRES_PCENT_HIGH 0.95

static uint64_t ii_host_writes = 0;
static uint64_t pi_host_writes = 0;
static uint64_t ii_gc_writes = 0;
static uint64_t pi_gc_writes = 0;
static uint64_t gc_of_gc_ru_writes = 0;

static void *ftl_thread(void *arg);
static void *log_thread(void *arg);
QemuThread logthread;
static NvmeReclaimGroup* g_rg = NULL;

static inline bool rg_should_gc(NvmeReclaimGroup* rg)
{
    return (rg->free_ru_cnt <= rg->gc_thres_rus);
}

static inline bool rg_should_gc_high(NvmeReclaimGroup* rg)
{
    return (rg->free_ru_cnt <= rg->gc_thres_rus_high);
}

static inline bool should_gc(FemuCtrl *n)
{
    NvmeReclaimGroup* rg;
    for (int i = 0; i < n->nrg; i++) {
        rg = &n->rgs[i];
        if (rg_should_gc(rg)) {
            return true;
        }
    }
    return false;
}

static inline bool should_gc_high(FemuCtrl *n)
{
    NvmeReclaimGroup* rg;
    for (int i = 0; i < n->nrg; i++) {
        rg = &n->rgs[i];
        if (rg_should_gc_high(rg)) {
            return true;
        }
    }
    return false;
}

static inline struct ppa get_maptbl_ent(struct ssd *ssd, uint64_t lpn)
{
    return ssd->maptbl[lpn];
}

static inline void set_maptbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    ssd->maptbl[lpn] = *ppa;
}

static inline uint64_t get_rmap_ent(struct ssd *ssd, struct ppa *ppa)
{
    return ssd->rmap[ppa->g.blk];
}

static inline void set_rmap_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    ssd->rmap[ppa->g.blk] = lpn;
}

static NvmeReclaimGroup* get_rg(FemuCtrl *n, struct ppa *ppa)
{
   return &n->rgs[ppa->g.rg];
}

static NvmeReclaimUnit * get_ru(FemuCtrl *n, struct ppa *ppa)
{
   NvmeReclaimGroup* rg = get_rg(n, ppa);
   return &rg->ru_array[ppa->g.ru];
}

static struct NvmeReclaimUnit *get_next_free_ru(FemuCtrl *fctl, int rgi)
{
    NvmeReclaimGroup* rg = &fctl->rgs[rgi];
    NvmeReclaimUnit *ru;

    ru = QTAILQ_FIRST(&rg->free_ru_list);
    if (!ru) {
        ftl_err("No free ru left in rg [%d]  !!!!\n", rgi);
        return NULL;
    }

    QTAILQ_REMOVE(&rg->free_ru_list, ru, entry);
    ru->status = RU_BUSY;
    rg->free_ru_cnt--;
    return ru;
}

static void ssd_advance_write_pointer(FemuCtrl *n, struct ppa* ppa)
{
    NvmeReclaimGroup* rg = &n->rgs[ppa->g.rg];
    NvmeReclaimUnit* ru = &rg->ru_array[ppa->g.ru];
    NvmeRuHandle* ruh = &n->ruhs[ru->ruhid];
    ru->wp++;
    if (ru->wp == ru->slba + ru->ruamw) {
        ruh->rus[ppa->g.rg] = NULL;
        if (ru->vpc == ru->ruamw) {
            /* all pgs are still valid, move to full line list */
            QTAILQ_INSERT_TAIL(&rg->used_ru_list, ru, entry);
            rg->used_ru_cnt++;
            ru->status = RU_USED;
        } else {
            /* there must be some invalid pages in this line */
            pqueue_insert(rg->victim_ru_pq, ru);
            ru->status = RU_VICTIM;
            rg->victim_ru_cnt++;
        }
    }
}

static void gc_advance_write_pointer(FemuCtrl *n, struct ppa* ppa)
{
    NvmeReclaimGroup* rg = &n->rgs[ppa->g.rg];
    NvmeReclaimUnit* ru = &rg->ru_array[ppa->g.ru];
    NvmeRuHandle* ruh = &n->ruhs[ru->ruhid];
    ru->wp++;
    if (ru->wp == ru->slba + ru->ruamw) {
        if (ruh->ruht == NVME_RUHT_PERSISTENTLY_ISOLATED) {
            ruh->pi_gc_rus[ppa->g.rg] = NULL;
        } else {
            ruh->rus[ppa->g.rg] = NULL;
        }
        if (ru->vpc == ru->ruamw) {
            /* all pgs are still valid, move to full line list */
            QTAILQ_INSERT_TAIL(&rg->used_ru_list, ru, entry);
            rg->used_ru_cnt++;
            ru->status = RU_USED;
        } else {
            /* there must be some invalid pages in this line */
            pqueue_insert(rg->victim_ru_pq, ru);
            ru->status = RU_VICTIM;
            rg->victim_ru_cnt++;
        }
    }
}

static struct ppa gc_get_new_page(FemuCtrl *n, int ruhid, int rg, NvmeRuDataType dtype)
{
    struct ppa ppa;
    NvmeReclaimUnit* ru;
    NvmeRuHandle * ruh = &n->ruhs[ruhid];
    if (ruh->ruht == NVME_RUHT_PERSISTENTLY_ISOLATED) {
        ppa.ppa = INVALID_PPA;

        if (!ruh->pi_gc_rus[rg]) {
            ruh->pi_gc_rus[rg] = get_next_free_ru(n, rg);
        }

        ru = ruh->pi_gc_rus[rg];
        if (ru == NULL) {
            return ppa;
        }
    } else {
        ppa.ppa = INVALID_PPA;
        if (!ruh->rus[rg]) {
            ruh->rus[rg] = get_next_free_ru(n, rg);
        }

        ru = ruh->rus[rg];
        if (ru == NULL) {
            return ppa;
        }
    }
    ru->dtype = dtype;
    ru->ruhid = ruhid;
    ppa.g.blk = ru->wp;
    ppa.g.ru  = ru->id;
    ppa.g.rg  = rg;

    return ppa;
}

static struct ppa get_new_page(FemuCtrl *n, int ruhid, int rg, NvmeRuDataType dtype)
{
    struct ppa ppa;
    NvmeReclaimUnit* ru;

    ppa.ppa = INVALID_PPA;
    NvmeRuHandle * ruh = &n->ruhs[ruhid];
    if (!ruh->rus[rg]) {
        ruh->rus[rg] = get_next_free_ru(n, rg);
    }

    ru = ruh->rus[rg];
    if (ru == NULL) {
        return ppa;
    }

    ru->dtype = dtype;
    ru->ruhid = ruhid;
    ppa.g.blk = ru->wp;
    ppa.g.ru  = ru->id;
    ppa.g.rg  = rg;

    return ppa;
}

static inline bool valid_ppa(FemuCtrl *n, struct ppa *ppa)
{
    NvmeNamespace *ns = &n->namespaces[0];
    int rgi = ppa->g.rg;
    NvmeReclaimGroup* rg = get_rg(n, ppa);
    int nru = rg->ru_cnt;
    int ru = ppa->g.ru;
    int blk = ppa->g.blk;

    if (rgi >= 0 && rgi < n->nrg && ru >= 0 && ru < nru &&
        blk >= 0 && blk < le64_to_cpu(ns->id_ns.nsze))
        return true;

    return false;
}

static inline bool mapped_ppa(struct ppa *ppa)
{
    return !(ppa->ppa == UNMAPPED_PPA);
}

static uint64_t ssd_advance_status(FemuCtrl *n, struct ppa *ppa, struct
        nand_cmd *ncmd)
{
    int c = ncmd->cmd;
    uint64_t cmd_stime = (ncmd->stime == 0) ? \
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : ncmd->stime;
    uint64_t nand_stime;
    struct ssd *ssd = n->ssd;
    struct ssdparams *spp = &ssd->spp;
    NvmeReclaimUnit* ru = get_ru(n, ppa);
    uint64_t lat = 0;

    switch (c) {
    case NAND_READ:
        /* read: perform NAND cmd first */
        nand_stime = (ru->next_ru_avail_time < cmd_stime) ? cmd_stime : \
                     ru->next_ru_avail_time;
        ru->next_ru_avail_time = nand_stime + spp->pg_rd_lat;
        lat = ru->next_ru_avail_time - cmd_stime;
        break;

    case NAND_WRITE:
        /* write: transfer data through channel first */
        nand_stime = (ru->next_ru_avail_time < cmd_stime) ? cmd_stime : \
                     ru->next_ru_avail_time;
        if (ncmd->type == USER_IO) {
            ru->next_ru_avail_time = nand_stime + spp->pg_wr_lat;
        } else {
            ru->next_ru_avail_time = nand_stime + spp->pg_wr_lat;
        }
        lat = ru->next_ru_avail_time - cmd_stime;
        break;

    case NAND_ERASE:
        /* erase: only need to advance NAND status */
        nand_stime = (ru->next_ru_avail_time < cmd_stime) ? cmd_stime : \
                     ru->next_ru_avail_time;
        ru->next_ru_avail_time = nand_stime + spp->blk_er_lat;

        lat = ru->next_ru_avail_time - cmd_stime;
        break;

    default:
        ftl_err("Unsupported NAND command: 0x%x\n", c);
    }

    return lat;
}

/* update SSD status about one page from PG_VALID -> PG_VALID */
static void mark_page_invalid(FemuCtrl *n, struct ppa *ppa)
{
    /* update corresponding page status */
    NvmeReclaimGroup * rg = get_rg(n, ppa);
    NvmeReclaimUnit * ru = get_ru(n, ppa);
    int pg_id = ppa->g.blk - ru->slba;
    ru->secstas[pg_id] = SEC_INVALID;

    /* update corresponding ru status */
    ru->ipc++;
    ru->vpc--;
    if (ru->status == RU_VICTIM) {
        /* Note that line->vpc will be updated by this call */
        pqueue_change_priority(rg->victim_ru_pq, ru->vpc, ru);
     } else if (ru->status == RU_USED) {
        /* move line: "used" -> "victim" */
        QTAILQ_REMOVE(&rg->used_ru_list, ru, entry);
        rg->used_ru_cnt--;
        ru->status = RU_VICTIM;
        pqueue_insert(rg->victim_ru_pq, ru);
        rg->victim_ru_cnt++;
    }
}

static void mark_page_valid(FemuCtrl *n, struct ppa *ppa)
{
    NvmeReclaimUnit * ru = get_ru(n, ppa);
    int pg_id = ppa->g.blk - ru->slba;
    ru->secstas[pg_id] = SEC_VALID;
    ru->vpc++;
}

static void gc_read_page(FemuCtrl *n, struct ppa *ppa)
{
    /* advance ssd status, we don't care about how long it takes */
    struct ssd* ssd = n->ssd;
    if (ssd->spp.enable_gc_delay) {
        struct nand_cmd gcr;
        gcr.type = GC_IO;
        gcr.cmd = NAND_READ;
        gcr.stime = 0;
        ssd_advance_status(n, ppa, &gcr);
    }
}

/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t gc_write_page(FemuCtrl *n, struct ssd *ssd, int ruhid, int rgi, struct ppa *old_ppa)
{
    struct ppa new_ppa;
    NvmeRuHandle* ruh = &n->ruhs[ruhid];
    uint64_t lpn = get_rmap_ent(ssd, old_ppa);

    new_ppa = gc_get_new_page(n, ruhid, rgi, RU_GC);
    if (ruh->ruht == NVME_RUHT_PERSISTENTLY_ISOLATED) {
        pi_gc_writes++;
    } else {
        ii_gc_writes++;
    }

    NvmeReclaimUnit* src_ru = get_ru(n, old_ppa);
    if (src_ru->dtype == RU_GC) {
        gc_of_gc_ru_writes++;
    }
    //g_gc_writes++;

    /* update maptbl */
    set_maptbl_ent(ssd, lpn, &new_ppa);
    /* update rmap */
    set_rmap_ent(ssd, lpn, &new_ppa);

    mark_page_valid(n, &new_ppa);

    /* need to advance the write pointer here */
    gc_advance_write_pointer(n, &new_ppa);

    if (ssd->spp.enable_gc_delay) {
        struct nand_cmd gcw;
        gcw.type = GC_IO;
        gcw.cmd = NAND_WRITE;
        gcw.stime = 0;
        ssd_advance_status(n, &new_ppa, &gcw);
    }

    return 0;
}

static int select_victim_rg(FemuCtrl *n)
{
    int victim_rg = -1;
    uint32_t ru_free_min = -1;
    for (int i = 0; i < n->nrg; i++) {
        NvmeReclaimGroup * rg = rg = &n->rgs[i];
        if (rg->free_ru_cnt < ru_free_min) {
            victim_rg = i;
            ru_free_min = rg->free_ru_cnt;
        }
    }

    return victim_rg;
}

static struct NvmeReclaimUnit *select_victim_ru(FemuCtrl *n, NvmeReclaimGroup* victim_rg, bool force)
{
    NvmeReclaimUnit * victim_ru = NULL;
    victim_ru = pqueue_peek(victim_rg->victim_ru_pq);
    if (!victim_ru) {
        return NULL;
    }

    if (!force && victim_ru->ipc < victim_ru->ruamw / 2) {
        return NULL;
    }

    pqueue_pop(victim_rg->victim_ru_pq);
    victim_ru->pos = 0;
    victim_rg->victim_ru_cnt--;

    return victim_ru;
}

static void mark_ru_free(NvmeReclaimGroup * rg, NvmeReclaimUnit *ru)
{
    ru->ipc = 0;
    ru->vpc = 0;
    ru->pos = 0;
    ru->ruhid = -1;
    ru->wp = ru->slba;
    for (int i = 0; i < ru->ruamw; i++) {
        ru->secstas[i] = SEC_FREE;
    }

    /* move this line to free line list */
    ru->status = RU_FREE;
    ru->dtype = RU_UNUSED;
    QTAILQ_INSERT_TAIL(&rg->free_ru_list, ru, entry);

    rg->free_ru_cnt++;
}

static int get_ctrl_ruh_id(FemuCtrl *n)
{
    return n->nruh - 1;
}

static int select_gc_ruh_id(FemuCtrl *n, NvmeReclaimUnit *ru)
{
    NvmeRuHandle * ruh = &n->ruhs[ru->ruhid];
    if (ruh->ruht == NVME_RUHT_PERSISTENTLY_ISOLATED) {
        return ru->ruhid;
    }

    return get_ctrl_ruh_id(n);
}

#ifdef FEMU_DEBUG_FTL
static void print(FemuCtrl *n) {
    uint32_t allipc = 0;
    uint32_t allvpc = 0;
    uint32_t all_ivsec = 0;
    uint32_t all_vsec = 0;
    uint32_t all_free = 0;
    uint32_t allsec = 0;
    for (uint16_t rgid = 0; rgid < n->nrg; rgid++) {
        NvmeReclaimGroup* rg = &n->rgs[rgid];
        NvmeReclaimUnit *ru = rg->ru_array;
        for (uint16_t i = 0; i < rg->ru_cnt; i++, ru++) {
            allipc += ru->ipc;
            allvpc += ru->vpc;
            allsec += ru->ruamw;
            uint32_t vsec = 0;
            uint32_t ivsec = 0;
            for (uint32_t j = 0; j < ru->ruamw; j++) {
                int8_t status = ru->secstas[j];
                if (status == SEC_VALID) {
                    vsec++;
                } else if (status == SEC_INVALID) {
                    ivsec++;
                }
            }
            all_ivsec += ivsec;
            all_vsec += vsec;
            all_free += ru->ruamw - ru->ipc - ru->vpc;
        }
    }

    uint32_t all_map_ivsec = 0;
    uint32_t all_map_vsec = 0;
    uint32_t all_rmap_ivsec = 0;
    uint32_t all_rmap_vsec = 0;
    struct ssd *ssd = n->ssd;
    for (int i = 0; i < allsec; i++) {
        if (ssd->maptbl[i].ppa == UNMAPPED_PPA) {
            all_map_ivsec++;
        } else {
            all_map_vsec++;
        }
    }

    for (int i = 0; i < allsec; i++) {
        if (ssd->rmap[i] == INVALID_LPN) {
            all_rmap_ivsec++;
        } else {
            all_rmap_vsec++;
        }
    }
}
#endif

static int do_gc(FemuCtrl *n, struct ssd *ssd, bool force)
{
    struct NvmeReclaimUnit *ru = NULL;
    struct ppa ppa;
#ifdef FEMU_DEBUG_FTL
    print(n);
#endif
    int rgid = select_victim_rg(n);
    NvmeReclaimGroup * rg = &n->rgs[rgid];
    ru = select_victim_ru(n, rg, force);
    if (!ru) {
        return -1;
    }

    int ruhid = select_gc_ruh_id(n, ru);
    if (ru->vpc > 0) {
        for (uint64_t i = 0; i < ru->ruamw; i++) {
            if (ru->secstas[i] != SEC_VALID) {
                continue;
            }
            ppa.g.rg = rgid;
            ppa.g.ru = ru->id;
            ppa.g.blk = ru->slba + i;
            gc_read_page(n, &ppa);
            /* delay the maptbl update until "write" happens */
            gc_write_page(n, ssd, ruhid, rgid, &ppa);

        }
    }

    mark_ru_free(rg, ru);
    return 0;
}

static inline uint16_t nvme_pid2ph(FemuCtrl *n, uint16_t pid)
{
    uint16_t rgif = n->rgif;

    if (!rgif) {
        return pid;
    }

    return pid & ((1 << (15 - rgif)) - 1);
}

static inline uint16_t nvme_pid2rg(FemuCtrl *n, uint16_t pid)
{
    uint16_t rgif = n->rgif;

    if (!rgif) {
        return 0;
    }

    return pid >> (16 - rgif);
}

static inline bool nvme_parse_pid(FemuCtrl *n, uint16_t pid,
                                  uint16_t *ph, uint16_t *rg)
{
    *rg = nvme_pid2rg(n, pid);
    *ph = nvme_pid2ph(n, pid);

    return *ph < n->nph && *rg < n->nrg;
}

static uint64_t fdp_write(FemuCtrl *n, struct ssd *ssd, NvmeRequest *req)
{
    int len = req->nlb;
    uint64_t start_lpn = req->slba;
    uint64_t end_lpn = (start_lpn + len - 1);
    NvmeRwCmd *rw = (NvmeRwCmd *)&req->cmd;
    uint32_t dw12 = le32_to_cpu(req->cmd.cdw12);
    uint8_t dtype = (dw12 >> 20) & 0xf;
    uint16_t pid = le16_to_cpu(rw->dspec);
    uint16_t ph, rg;

    if (dtype != NVME_DIRECTIVE_DATA_PLACEMENT ||
        !nvme_parse_pid(n, pid, &ph, &rg)) {
        ph = 0;
        rg = 0;
    }

    uint16_t ruhid = n->phs[ph];
    NvmeRuHandle* ruh = &n->ruhs[ruhid];
    struct ppa ppa;
    uint64_t curlat = 0, maxlat = 0;
    int r;

    while (should_gc_high(n)) {
        /* perform GC here until !should_gc(ssd) */
        r = do_gc(n, ssd, true);
        if (r == -1) {
            break;
        }
    }

    for (uint64_t lpn = start_lpn; lpn <= end_lpn; lpn++) {
        ppa = get_maptbl_ent(ssd, lpn);
        if (mapped_ppa(&ppa)) {
            /* update old page information first */
            mark_page_invalid(n, &ppa);
            set_rmap_ent(ssd, INVALID_LPN, &ppa);
        }

        /* new write */
        ppa = get_new_page(n, ruhid, rg, RU_DATA);
        if (ruh->ruht == NVME_RUHT_PERSISTENTLY_ISOLATED) {
            pi_host_writes++;
        } else {
            ii_host_writes++;
        }

        /* update maptbl */
        set_maptbl_ent(ssd, lpn, &ppa);
        /* update rmap */
        set_rmap_ent(ssd, lpn, &ppa);

        struct ppa tppa = get_maptbl_ent(ssd, lpn);
        uint64_t tlpn = get_rmap_ent(ssd, &ppa);
        if (tppa.ppa != ppa.ppa || tlpn != lpn) {
            ftl_err("check error lpn[%lu] ru[%d] rg[%u] blk[%u] tru[%d] trg[%u] tblk[%u] tlpn[%lu]\n", lpn,
                ppa.g.ru, ppa.g.rg, ppa.g.blk,
                tppa.g.ru, tppa.g.rg, tppa.g.blk, tlpn);
        }

        mark_page_valid(n, &ppa);

        /* need to advance the write pointer here */
        ssd_advance_write_pointer(n, &ppa);

        struct nand_cmd swr;
        swr.type = USER_IO;
        swr.cmd = NAND_WRITE;
        swr.stime = req->stime;
        /* get latency statistics */
        curlat = ssd_advance_status(n, &ppa, &swr);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }

    return maxlat;
}

static uint64_t fdp_read(FemuCtrl *n, struct ssd *ssd, NvmeRequest *req)
{
    int nsecs = req->nlb;
    uint64_t start_lpn = req->slba;
    uint64_t end_lpn = (start_lpn + nsecs - 1);
    uint64_t sublat, maxlat = 0;

    /* normal IO read path */
    for (uint64_t lpn = start_lpn; lpn <= end_lpn; lpn++) {
        struct ppa ppa = get_maptbl_ent(ssd, lpn);
        if (!mapped_ppa(&ppa) || !valid_ppa(n, &ppa)) {
            continue;
        }

        struct nand_cmd srd;
        srd.type = USER_IO;
        srd.cmd = NAND_READ;
        srd.stime = req->stime;
        sublat = ssd_advance_status(n, &ppa, &srd);
        maxlat = (sublat > maxlat) ? sublat : maxlat;
    }

    return maxlat;
}

static uint64_t fdp_dsm(FemuCtrl *n, struct ssd *ssd, NvmeRequest *req)
{
    NvmeCmd* cmd = &(req->cmd);
    uint32_t dw10 = le32_to_cpu(cmd->cdw10);
    uint32_t dw11 = le32_to_cpu(cmd->cdw11);
    uint64_t prp1 = le64_to_cpu(cmd->dptr.prp1);
    uint64_t prp2 = le64_to_cpu(cmd->dptr.prp2);
    uint64_t lat = 0;
    if (dw11 & NVME_DSMGMT_AD) {
        uint16_t nr = (dw10 & 0xff) + 1;
        uint64_t slba;
        uint32_t nlb;
        NvmeDsmRange *range = g_malloc0(sizeof(NvmeDsmRange) * nr);
        if (dma_write_prp(n, (uint8_t *)range, sizeof(range), prp1, prp2)) {
            g_free(range);
            return lat;
        }

        req->status = NVME_SUCCESS;
        for (int i = 0; i < nr; i++) {
            slba = le64_to_cpu(range[i].slba);
            nlb = le32_to_cpu(range[i].nlb);
            for (uint64_t lpn = slba; lpn < slba + nlb; lpn++) {
                struct ppa ppa = get_maptbl_ent(ssd, lpn);
                if (mapped_ppa(&ppa)) {
                    mark_page_invalid(n, &ppa);
                    set_rmap_ent(ssd, INVALID_LPN, &ppa);
                    ppa.ppa = UNMAPPED_PPA;
                    set_maptbl_ent(ssd, lpn, &ppa);
                }
            }
        }
    g_free(range);	
    }
    return lat;
}

static void *ftl_thread(void *arg)
{
    FemuCtrl *n = (FemuCtrl *)arg;
    struct ssd *ssd = n->ssd;
    NvmeRequest *req = NULL;
    uint64_t lat = 0;
    int rc;
    int i;

    while (!*(ssd->dataplane_started_ptr)) {
        usleep(100000);
    }

    /* FIXME: not safe, to handle ->to_ftl and ->to_poller gracefully */
    ssd->to_ftl = n->to_ftl;
    ssd->to_poller = n->to_poller;

    while (1) {
        for (i = 1; i <= n->nr_pollers; i++) {
            if (!ssd->to_ftl[i] || !femu_ring_count(ssd->to_ftl[i]))
                continue;

            rc = femu_ring_dequeue(ssd->to_ftl[i], (void *)&req, 1);
            if (rc != 1) {
                ftl_err("FEMU: FTL to_ftl dequeue failed\n");
            }

            ftl_assert(req);
            switch (req->cmd.opcode) {
            case NVME_CMD_WRITE:
                lat = fdp_write(n, ssd, req);
                break;
            case NVME_CMD_READ:
                lat = fdp_read(n, ssd, req);
                break;
            case NVME_CMD_DSM:
                lat = fdp_dsm(n, ssd, req);
                break;
            default:
                ftl_err("FTL received unkown request type, ERROR\n");
                ;
            }

            req->reqlat = lat;
            req->expire_time += lat;

            rc = femu_ring_enqueue(ssd->to_poller[i], (void *)&req, 1);
            if (rc != 1) {
                ftl_err("FTL to_poller enqueue failed\n");
            }

            /* clean one line if needed (in the background) */
            if (should_gc(n)) {
                do_gc(n, ssd, false);
            }
        }
    }

    return NULL;
}

static void *log_thread(void* arg)
{
    while(true) {
        sleep(60);
        ftl_debug("ii_host_writes= %lu, ii_gc_writes= %lu, pi_host_writes= %lu, \
            pi_gc_writes= %lu, gc_of_gc_ru_writes=%lu, free_ru_cnt= %d \n",
            ii_host_writes, ii_gc_writes, pi_host_writes, pi_gc_writes,
            gc_of_gc_ru_writes, g_rg->free_ru_cnt);
    }
    return NULL;
}
static void fdp_init_maptbl(struct ssd *ssd, uint32_t nblk)
{
    ssd->maptbl = g_malloc0(sizeof(struct ppa) * nblk);
    for (int i = 0; i < nblk; i++) {
        ssd->maptbl[i].ppa = UNMAPPED_PPA;
    }
}

static void fdp_init_rmap(struct ssd *ssd, uint32_t nblk)
{
    ssd->rmap = g_malloc0(sizeof(uint64_t) * nblk);
    for (int i = 0; i < nblk; i++) {
        ssd->rmap[i] = INVALID_LPN;
    }
}

void fdp_init_ftl(FemuCtrl *n)
{
    struct ssd *ssd = n->ssd;
    NvmeNamespace *ns = &n->namespaces[0];
    g_rg = &n->rgs[0];

    uint32_t nsec = ns->ns_blks * 100 / n->fdp_params.fdp_gc_thres_pcent;
    /* initialize maptbl */
    fdp_init_maptbl(ssd, nsec);

    /* initialize rmap */
    fdp_init_rmap(ssd, nsec);

    qemu_thread_create(&ssd->ftl_thread, "FEMU-FTL-Thread", ftl_thread, n,
                       QEMU_THREAD_JOINABLE);
    qemu_thread_create(&logthread, "Log-Thread", log_thread, NULL,
                       QEMU_THREAD_JOINABLE);
}

