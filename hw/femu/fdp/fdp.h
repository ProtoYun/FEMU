#ifndef __FEMU_FDP_H
#define __FEMU_FDP_H

#include "../nvme.h"

#define NVME_FDP_MAX_EVENTS 63
#define NVME_FDP_MAXPIDS 128

enum NvmeRuhType {
    NVME_RUHT_INITIALLY_ISOLATED = 1,
    NVME_RUHT_PERSISTENTLY_ISOLATED = 2,
};

enum NvmeRuhAttributes {
    NVME_RUHA_UNUSED = 0,
    NVME_RUHA_HOST = 1,
    NVME_RUHA_CTRL = 2,
};

enum NvmeDirectiveTypes {
    NVME_DIRECTIVE_IDENTIFY       = 0x0,
    NVME_DIRECTIVE_DATA_PLACEMENT = 0x2,
};

enum {
    SEC_FREE = 0,
    SEC_INVALID = 1,
    SEC_VALID = 2
};

enum {
    RU_FREE = 0,
    RU_BUSY = 1,
    RU_USED = 2,
    RU_VICTIM = 3
 };

typedef struct NvmeReclaimGroup {
    NvmeReclaimUnit* ru_array;
    QTAILQ_HEAD(used_ru_list, NvmeReclaimUnit) used_ru_list;
    QTAILQ_HEAD(free_ru_list, NvmeReclaimUnit) free_ru_list;
    pqueue_t *victim_ru_pq;
    int ru_cnt;
    int free_ru_cnt;
    int victim_ru_cnt;
    int used_ru_cnt;
    int gc_thres_rus;
    int gc_thres_rus_high;
} NvmeReclaimGroup;

enum NvmeFdpEventFlags {
    FDPEF_PIV = 1 << 0,
    FDPEF_NSIDV = 1 << 1,
    FDPEF_LV = 1 << 2,
};

typedef struct QEMU_PACKED NvmeFdpEvent {
    uint8_t  type;
    uint8_t  flags;
    uint16_t pid;
    uint64_t timestamp;
    uint32_t nsid;
    uint64_t type_specific[2];
    uint16_t rgid;
    uint8_t  ruhid;
    uint8_t  rsvd35[5];
    uint64_t vendor[3];
} NvmeFdpEvent;

typedef struct NvmeFdpEventBuffer {
    NvmeFdpEvent     events[NVME_FDP_MAX_EVENTS];
    unsigned int     nelems;
    unsigned int     start;
    unsigned int     next;
} NvmeFdpEventBuffer;

struct ssdparams {
    int secsz;        /* sector size in bytes */
    int secs_per_pg;  /* # of sectors per page */
    int pgs_per_blk;  /* # of NAND pages per block */
    int blks_per_pl;  /* # of blocks per plane */
    int pls_per_lun;  /* # of planes per LUN (Die) */
    int luns_per_ch;  /* # of LUNs per channel */
    int nchs;         /* # of channels in the SSD */

    int pg_rd_lat;    /* NAND page read latency in nanoseconds */
    int pg_wr_lat;    /* NAND page program latency in nanoseconds */
    int blk_er_lat;   /* NAND block erase latency in nanoseconds */
    int ch_xfer_lat;  /* channel transfer latency for one page in nanoseconds
                       * this defines the channel bandwith
                       */

    double gc_thres_pcent;
    int gc_thres_lines;
    double gc_thres_pcent_high;
    int gc_thres_lines_high;
    bool enable_gc_delay;

    /* below are all calculated values */
    int secs_per_blk; /* # of sectors per block */
    int secs_per_pl;  /* # of sectors per plane */
    int secs_per_lun; /* # of sectors per LUN */
    int secs_per_ch;  /* # of sectors per channel */
    int tt_secs;      /* # of sectors in the SSD */

    int pgs_per_pl;   /* # of pages per plane */
    int pgs_per_lun;  /* # of pages per LUN (Die) */
    int pgs_per_ch;   /* # of pages per channel */
    int tt_pgs;       /* total # of pages in the SSD */

    int blks_per_lun; /* # of blocks per LUN */
    int blks_per_ch;  /* # of blocks per channel */
    int tt_blks;      /* total # of blocks in the SSD */

    int secs_per_line;
    int pgs_per_line;
    int blks_per_line;
    int tt_lines;

    int pls_per_ch;   /* # of planes per channel */
    int tt_pls;       /* total # of planes in the SSD */

    int tt_luns;      /* total # of LUNs in the SSD */
};

struct nand_cmd {
    int type;
    int cmd;
    int64_t stime; /* Coperd: request arrival time */
};

struct ssd {
    char *ssdname;
    struct ssdparams spp;
    struct ppa *maptbl; /* page level mapping table */
    uint64_t *rmap;     /* reverse mapptbl, assume it's stored in OOB */
    /* lockless ring for communication with NVMe IO thread */
    struct rte_ring **to_ftl;
    struct rte_ring **to_poller;
    bool *dataplane_started_ptr;
    QemuThread ftl_thread;
};

void fdp_init_ftl(FemuCtrl *n);

void fdp_ns_shutdown(NvmeNamespace *ns);
void fdp_ns_cleanup(NvmeNamespace *ns);

//#define FDP_DEBUG
#ifdef FDP_DEBUG
#define fdp_debug(fmt, ...) \
    do { printf("[FDP]: " fmt, ## __VA_ARGS__); } while (0)
#else
#define fdp_debug(fmt, ...) \
    do { } while (0)
#endif

#endif
