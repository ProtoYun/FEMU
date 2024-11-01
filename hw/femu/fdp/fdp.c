#include "../nvme.h"
#include "./fdp.h"

#define MIN_DISCARD_GRANULARITY  (4 * KiB)
#define NVME_DEFAULT_RU_SIZE     (64 * MiB)
#define NVME_DEFAULT_RUH_NUM     (8)
#define NVME_DEFAULT_RG_NUM      (1)
#define NVME_DEFAULT_PH_NUM      (8)

static uint16_t fdp_nvme_rw(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                           NvmeRequest *req)
{
    return nvme_rw(n, ns, cmd, req);
}

static uint16_t fdp_io_cmd(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                          NvmeRequest *req)
{
    switch (cmd->opcode) {
    case NVME_CMD_READ:
    case NVME_CMD_WRITE:
        return fdp_nvme_rw(n, ns, cmd, req);
    default:
        return NVME_INVALID_OPCODE | NVME_DNR;
    }
}

static uint16_t fdp_admin_cmd(FemuCtrl *n, NvmeCmd *cmd)
{
    switch (cmd->opcode) {
    default:
        return NVME_INVALID_OPCODE | NVME_DNR;
    }
}

static void fdp_set_ctrl_str(FemuCtrl *n)
{
    static int fsid_fdp = 0;
    const char *fdp_mn = "FEMU FDP-SSD Controller";
    const char *fdp_sn = "vFDPSSD";

    nvme_set_ctrl_name(n, fdp_mn , fdp_sn, &fsid_fdp);
}

static void fdp_set_ctrl(FemuCtrl *n)
{
    uint8_t *pci_conf = n->parent_obj.config;

    fdp_set_ctrl_str(n);

    pci_config_set_vendor_id(pci_conf, PCI_VENDOR_ID_INTEL);
    pci_config_set_device_id(pci_conf, 0x5845);
}

static inline int victim_ru_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
}

static inline pqueue_pri_t victim_ru_get_pri(void *a)
{
    return ((NvmeReclaimUnit *)a)->vpc;
}

static inline void victim_ru_set_pri(void *a, pqueue_pri_t pri)
{
    ((NvmeReclaimUnit *)a)->vpc = pri;
}

static inline size_t victim_ru_get_pos(void *a)
{
    return ((NvmeReclaimUnit *)a)->pos;
}

static inline void victim_ru_set_pos(void *a, size_t pos)
{
    ((NvmeReclaimUnit *)a)->pos = pos;
}

static void fdp_init_phs(FemuCtrl *n)
{
    n->phs = g_malloc0(sizeof(uint16_t) * n->nph);
    for (uint16_t phid = 0; phid < n->nph; phid++) {
       n->phs[phid] = phid;
    }
}

static void fdp_init_ru_status(NvmeReclaimUnit* ru)
{
    ru->secstas = g_malloc0(sizeof(int8_t) * ru->ruamw);
    for (int i = 0; i < ru->ruamw; i++) {
        ru->secstas[i] = SEC_FREE;
    }
}

static void fdp_init_rgs(FemuCtrl *n, NvmeNamespace *ns)
{
    n->rgs = g_malloc0(sizeof(NvmeReclaimGroup) * n->nrg);
    uint64_t actual_size = ns->size * 100 / n->fdp_params.fdp_gc_thres_pcent;
    uint32_t ru_num = actual_size / n->runs;
    uint32_t rg_nru = ru_num / n->nrg;
    uint32_t remain_ru = ru_num;
    NvmeReclaimUnit *ru;
    uint64_t start = 0;
    for (uint16_t rgid = 0; rgid < n->nrg; rgid++) {
        NvmeReclaimGroup* rg = &n->rgs[rgid];
        QTAILQ_INIT(&rg->free_ru_list);
        rg->victim_ru_pq = pqueue_init(rg->ru_cnt, victim_ru_cmp_pri,
            victim_ru_get_pri, victim_ru_set_pri,
            victim_ru_get_pos, victim_ru_set_pos);
        QTAILQ_INIT(&rg->used_ru_list);
        rg->free_ru_cnt = 0;
        rg->victim_ru_cnt = 0;
        rg->used_ru_cnt = 0;

        rg->ru_cnt = remain_ru >= rg_nru ? rg_nru : remain_ru;
        rg->ru_cnt = rg_nru;
        rg->gc_thres_rus = rg->ru_cnt * (100 - n->fdp_params.fdp_gc_thres_pcent) / 100;
        rg->gc_thres_rus_high = rg->ru_cnt * (100 - n->fdp_params.fdp_gc_thres_pcent_high) / 100;

        remain_ru -= rg->ru_cnt;
        rg->ru_array = g_malloc0(sizeof(NvmeReclaimUnit) * rg->ru_cnt);
        ru = rg->ru_array;
        for (uint16_t i = 0; i < rg->ru_cnt; i++, ru++) {
            ru->ruamw = n->runs >> NVME_ID_NS_LBADS(ns);
            ru->wp = ru->slba = start;
            ru->ruhid = -1;
            ru->ipc = 0;
            ru->vpc = 0;
            ru->pos = 0;
            ru->id = i;
            ru->status = RU_FREE;
            ru->dtype= RU_UNUSED;
            fdp_init_ru_status(ru);
            QTAILQ_INSERT_TAIL(&rg->free_ru_list, ru, entry);
            start += ru->ruamw;
            rg->free_ru_cnt++;
        }
    }
}

static void fdp_init_ruhs(FemuCtrl *n)
{
    n->ruhs = g_malloc0(sizeof(NvmeRuHandle) * n->nruh);
    uint16_t max_host_ruh = n->fdp_params.fdp_num_ruh;
    uint16_t ctrl_ruhi = n->nruh-1;
    for (uint16_t ruhid = 0; ruhid < max_host_ruh; ruhid++) {
        if (ruhid < n->npi) {
            n->ruhs[ruhid] = (NvmeRuHandle) {
                .ruht = NVME_RUHT_PERSISTENTLY_ISOLATED,
                .ruha = NVME_RUHA_HOST,
            };

            n->ruhs[ruhid].rus = g_new0(NvmeReclaimUnit*, n->nrg);
            n->ruhs[ruhid].pi_gc_rus = g_new0(NvmeReclaimUnit*, n->nrg);
        } else {
            n->ruhs[ruhid] = (NvmeRuHandle) {
                .ruht = NVME_RUHT_INITIALLY_ISOLATED,
                .ruha = NVME_RUHA_HOST,
            };

            n->ruhs[ruhid].rus = g_new0(NvmeReclaimUnit*, n->nrg);
            n->ruhs[ruhid].pi_gc_rus = NULL;
        }
    }

    n->ruhs[ctrl_ruhi] = (NvmeRuHandle) {
        .ruht = NVME_RUHT_INITIALLY_ISOLATED,
        .ruha = NVME_RUHA_CTRL,
    };

    n->ruhs[ctrl_ruhi].rus = g_new0(NvmeReclaimUnit*, n->nrg);
}

static int fdp_init_para(FemuCtrl *n, NvmeNamespace *ns)
{
    n->fdp_enabled = true;
    n->hbmw = 0;
    n->mbmw = 0;
    n->mb_erase = 0;
    n->nruh = n->fdp_params.fdp_num_ruh + 1;
    n->nph = n->fdp_params.fdp_num_ph;
    n->npi = n->fdp_params.fdp_num_pi;
    if (n->fdp_params.fdp_num_ph > n->fdp_params.fdp_num_ruh) {
        n->nph = n->fdp_params.fdp_num_ruh;
    }
    if (n->fdp_params.fdp_num_pi > n->fdp_params.fdp_num_ph) {
        n->npi = n->fdp_params.fdp_num_ph;
    }

    n->nrg = n->fdp_params.fdp_num_rg;
    n->runs = n->fdp_params.fdp_ru_size * 1024 * 1024;
    return 0;
}

static void fdp_init(FemuCtrl *n, Error **errp)
{
    NvmeNamespace *ns = &n->namespaces[0];
    struct ssd *ssd = n->ssd = g_malloc0(sizeof(struct ssd));

    fdp_set_ctrl(n);
    ssd->dataplane_started_ptr = &n->dataplane_started;
    ssd->ssdname = (char *)n->devname;

    femu_debug("Starting FEMU in FDP-SSD mode ...\n");

    fdp_init_para(n, ns);

    fdp_init_phs(n);

    fdp_init_ruhs(n);

    fdp_init_rgs(n, ns);

    fdp_init_ftl(n);
}

static void fdp_exit(FemuCtrl *n)
{
    /*
     * Release any extra resource (zones) allocated for FDP mode
     */
}

int nvme_register_fdpssd(FemuCtrl *n)
{
    n->ext_ops = (FemuExtCtrlOps) {
        .state            = NULL,
        .init             = fdp_init,
        .exit             = fdp_exit,
        .rw_check_req     = NULL,
        .admin_cmd        = fdp_admin_cmd,
        .io_cmd           = fdp_io_cmd,
        .get_log          = NULL,
    };

    return 0;
}

