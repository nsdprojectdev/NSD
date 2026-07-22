// SPDX-License-Identifier: GPL-2.0-only
/*
 * NSD - Neural Storage Driver
 * Copyright (c) 2026 Ayhan Aydin
 *
 * Kernel module that learns I/O patterns and prefetches data
 * using a synaptic Markov chain.
 *
 * Dual-licensed: GPL-2.0-only OR Commercial.
 * See LICENSE file for details.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/blkdev.h>
#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/fprobe.h>
#include <linux/percpu.h>
#include <linux/jiffies.h>
#include <linux/sched.h>
#include <linux/random.h>
#include <linux/siphash.h>
#include <linux/shrinker.h>
#include <linux/sysinfo.h>
#include <linux/fs.h>
#include <linux/fadvise.h>
#include <linux/version.h>
#include <linux/ptrace.h>
#include <linux/string.h>

#ifndef POSIX_FADV_WILLNEED
#define POSIX_FADV_WILLNEED 3
#endif

#define NSD_VERSION "1.0.0"

#define NSD_REGION_SHIFT_HDD   16
#define NSD_REGION_SHIFT_SSD   12
#define NSD_REGION_SHIFT_NVME  13

#define NSD_REGION_BYTES_HDD   (1UL << NSD_REGION_SHIFT_HDD)
#define NSD_REGION_BYTES_SSD   (1UL << NSD_REGION_SHIFT_SSD)
#define NSD_REGION_BYTES_NVME  (1UL << NSD_REGION_SHIFT_NVME)

#define NSD_THRESH_HDD         700
#define NSD_THRESH_SSD         180
#define NSD_THRESH_NVME        400

#define NSD_DEPTH_HDD          2
#define NSD_DEPTH_SSD          8
#define NSD_DEPTH_NVME         2

#define NSD_PREFETCH_SPAN_HDD  (4UL * 1024)
#define NSD_PREFETCH_SPAN_SSD  (256UL * 1024)
#define NSD_PREFETCH_SPAN_NVME (4UL   * 1024)

#define NSD_STRIDE_SPAN_MULT   2

#define NSD_SPAN_MULT_SEQ       2
#define NSD_SPAN_MULT_RAND      1
#define NSD_SPAN_MULT_META      0
#define NSD_SPAN_MULT_MIXED     2

#define NSD_META_RATIO_THRESH   800
#define NSD_SMALL_IO_THRESH     8192

#define NSD_PROFILE_WINDOW      512
#define NSD_SEQ_RATIO_THRESH    600

#define NSD_PROCAWARE_BUCKET_BITS   8

#define NSD_SYN_WAYS          8

#define NSD_W_MAX         1000
#define NSD_W_INIT        400
#define NSD_W_OBSERVE     300
#define NSD_W_HIT         150
#define NSD_W_PENALTY   150
#define NSD_PEND_NONE      0
#define NSD_PEND_SYNAPTIC  1
#define NSD_PEND_STRIDE    2
#define NSD_W_DECAY_MS    300000

#define NSD_DECAY_RUNS_MIN        6
#define NSD_DECAY_AGE_RUNS        5
#define NSD_DECAY_AGGRESSIVE_PCT  90
#define NSD_SHRINK_EVICT_WEIGHT   100

#define NSD_SEQ_RATIO_THRESH    600
#define NSD_RPT_RATIO_THRESH    300
#define NSD_SEQ_BYPASS_THRESH   800

#define NSD_SEQ_BYPASS_THRESH_HDD   600
#define NSD_SEQ_BYPASS_THRESH_SSD   800
#define NSD_SEQ_BYPASS_THRESH_NVME  900

#define NSD_HYST_CONFIRM_UP     2
#define NSD_HYST_CONFIRM_UP_RND 3
#define NSD_HYST_CONFIRM_DOWN   1
#define NSD_HOT_SLOTS           64
#define NSD_PCPU_SLOTS          8

#define NSD_ML_WINDOW_SIZE      64
#define NSD_ML_LR_INIT          100
#define NSD_AUTOTHRESH_HIGH_ACC 900
#define NSD_AUTOTHRESH_LOW_ACC  150
#define NSD_AUTOTHRESH_STEP     25
#define NSD_THRESH_MIN          200
#define NSD_THRESH_MAX          500

#define NSD_AUTOTHRESH_HIGH_ACC_HDD 800
#define NSD_AUTOTHRESH_LOW_ACC_HDD  100
#define NSD_AUTOTHRESH_STEP_HDD     10
#define NSD_AUTOTHRESH_HIGH_ACC 900
#define NSD_AUTOTHRESH_LOW_ACC  150
#define NSD_AUTOTHRESH_STEP     25

#define NSD_THRESH_MIN_HDD 200
#define NSD_THRESH_MAX_HDD 500

#define NSD_MON_WINDOW    256
#define NSD_MON_DISABLE   30
#define NSD_MON_ENABLE     80

#define NSD_FILE_ACC_MIN           5
#define NSD_FILE_ACC_MIN_SAMPLES   100

#define NSD_RING_SIZE      256u
#define NSD_RING_MASK      (NSD_RING_SIZE - 1u)
#define NSD_PENDING_SLOTS  65536U
#define NSD_PENDING_MASK   (NSD_PENDING_SLOTS - 1U)
#define NSD_FCTX_BITS      8
#define NSD_FCTX_SLOTS     (1 << NSD_FCTX_BITS)
#define NSD_TELEM_MS       5000

enum nsd_dev_class {
    NSD_DEV_UNKNOWN = 0,
    NSD_DEV_HDD,
    NSD_DEV_SSD,
    NSD_DEV_NVME,
};

static const char * const nsd_dev_names[] = {
    [NSD_DEV_UNKNOWN] = "unknown",
    [NSD_DEV_HDD]     = "HDD",
    [NSD_DEV_SSD]     = "SSD",
    [NSD_DEV_NVME]    = "NVMe",
};

enum nsd_strategy {
    NSD_STRAT_NONE = 0,
    NSD_STRAT_MARKOV,
    NSD_STRAT_BIGRAM,
    NSD_STRAT_FREQ_RECENCY,
};

static const char * const nsd_strat_names[] = {
    [NSD_STRAT_NONE]         = "none",
    [NSD_STRAT_MARKOV]       = "markov",
    [NSD_STRAT_BIGRAM]       = "bigram",
    [NSD_STRAT_FREQ_RECENCY] = "freq_recency",
};

enum nsd_mode {
    NSD_MODE_NORMAL = 0,
    NSD_MODE_INCOGNITO,
    NSD_MODE_AIR_GAP,
};

#define NSD_MAX_DESTS 4

struct nsd_synapse {
    u32 key_hi;
    u32 dst_region[NSD_MAX_DESTS];
    u16 weight[NSD_MAX_DESTS];
    u8  dst_count;
    u64 last_seen_ns;
};

struct nsd_syn_bucket {
    spinlock_t lock;
    struct nsd_synapse ways[NSD_SYN_WAYS];
};

struct nsd_ring_entry {
    struct file *file;
    loff_t       offset;
    size_t       size;
};

struct nsd_pcpu_ring {
    struct nsd_ring_entry slots[NSD_RING_SIZE];
    atomic_t prod;
    atomic_t cons;
};
static DEFINE_PER_CPU(struct nsd_pcpu_ring, nsd_ring);

struct nsd_pcpu_slot {
    u32  file_id;
    u32  region;
    u32  prev_region;
    bool valid;
    u64  last_ns;
};

struct nsd_pcpu_state {
    u32  micro_seq_last_inode;
    u32  micro_seq_last_pid;
    loff_t micro_seq_last_offset;
    size_t micro_seq_last_len;
    u64  micro_seq_last_ns;
    u32  micro_seq_hits;
    struct nsd_pcpu_slot slots[NSD_PCPU_SLOTS];
};
static DEFINE_PER_CPU(struct nsd_pcpu_state, nsd_cpu_state);

struct nsd_hot_entry {
    u32  region;
    u32  count;
    u64  last_ns;
};

struct nsd_workload {
    u32  event_count;
    u32  seq_count;
    u32  repeat_count;
    s64  last_delta;
    enum nsd_strategy active;
    enum nsd_strategy pending;
    u8   pending_confirms;
};

struct nsd_adaptive {
    u16 historical_acc;
    u16 recent_acc;
    u32 recent_window[NSD_ML_WINDOW_SIZE];
    u32 recent_idx;
    u32 recent_sum;
    u16 learning_rate;
};
struct nsd_monitor {
    u32  window[NSD_MON_WINDOW];
    u32  idx, sum, count;
    bool prefetch_ok;
};

struct nsd_fctx {
    u32                file_id;
    bool               valid;
    struct inode      *inode;
    u64                last_seen_ns;
    u64                read_count;
    struct nsd_workload wl;
    struct nsd_hot_entry hot[NSD_HOT_SLOTS];
    u8                 hot_used;


    s32                stride_deltas[3];
    u8                 stride_count;
    u32                stride_last_region;


    u32                pf_count;
    u32                hit_count;
    u32                miss_count;
    u32                negcache_hits;
    u64                negcache_until;
    bool               disabled;


    u32                meta_ops;
    u32                small_io_ops;
    u64                total_bytes_read;
    u32  depth;
    u16  thresh;
    u64  prefetch_span;
    struct nsd_monitor mon;
    struct nsd_adaptive ada;
};



static struct {

    struct nsd_syn_bucket *syn;
    u32  syn_bits;
    u32  syn_buckets;
    u32  syn_mask;


    struct {
        spinlock_t lock;
        struct nsd_fctx e[NSD_FCTX_SLOTS];
    } fctx;


    struct task_struct      *worker;
    struct workqueue_struct *wq;


    struct nsd_monitor       mon;
    spinlock_t               mon_lock;


    struct nsd_adaptive      ada;
    spinlock_t               ada_lock;


    u64 pending[NSD_PENDING_SLOTS];
    u64 pending_ts[NSD_PENDING_SLOTS];
    u32 pending_way[NSD_PENDING_SLOTS];
    u32 pending_dst[NSD_PENDING_SLOTS];
    u32 pending_kind[NSD_PENDING_SLOTS];
    bool skip_kprobe[NSD_FCTX_SLOTS];
    u64 skip_ino[NSD_FCTX_SLOTS];
    u64 skip_time[NSD_FCTX_SLOTS];

    struct delayed_work      decay_work;
    struct delayed_work      telem_work;
    struct delayed_work      waste_track_work;


    struct shrinker         *shrinker_ptr;
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 7, 0)
    struct shrinker          shrinker;
#endif


    siphash_key_t            hkey;


    enum nsd_dev_class       dev_class;


    u32  region_shift;
    u32  depth;
    u16  thresh;
    u64  prefetch_span;


    atomic_t running;
    atomic_t hook_on;
    atomic_t hook_reg;
    atomic_t observe_only;
    atomic_t feat_stride;
    atomic_t feat_autothresh;
    atomic_t feat_fine_decay;
    atomic_t feat_procaware;
    atomic_t feat_waste_track;


    atomic64_t st_events;
    atomic64_t st_learned;
    atomic64_t st_predictions;
    atomic64_t st_correct;
    atomic64_t st_prefetch;
    atomic64_t st_coalesced;
    atomic64_t st_disabled;
    atomic64_t st_decay;
    atomic64_t st_waste_expired;
    atomic64_t st_pending_overwrite;
    atomic64_t st_pending_skip;

    atomic64_t st_syn_active;
    atomic64_t st_syn_evict;
    atomic64_t st_dropped;
    atomic64_t st_kprobe;
    atomic64_t st_seq_bypass;
    atomic64_t st_strat_switch;
    atomic64_t st_autothresh_adj;
    atomic64_t st_stride_predictions;
    atomic64_t st_stride_chain;
} nsd = {
    .running       = ATOMIC_INIT(0),
    .hook_on       = ATOMIC_INIT(0),
    .hook_reg      = ATOMIC_INIT(0),
    .observe_only  = ATOMIC_INIT(0),
    .feat_stride   = ATOMIC_INIT(1),
    .feat_autothresh = ATOMIC_INIT(1),
    .feat_fine_decay = ATOMIC_INIT(1),
.feat_procaware  = ATOMIC_INIT(1),
    .feat_waste_track = ATOMIC_INIT(0),
    .dev_class     = NSD_DEV_SSD,
    .region_shift  = NSD_REGION_SHIFT_SSD,
    .depth         = NSD_DEPTH_SSD,
    .thresh        = NSD_THRESH_SSD,
    .prefetch_span = NSD_PREFETCH_SPAN_SSD,
    .fctx.lock     = __SPIN_LOCK_UNLOCKED(nsd.fctx.lock),
    .mon_lock      = __SPIN_LOCK_UNLOCKED(nsd.mon_lock),
    .ada_lock      = __SPIN_LOCK_UNLOCKED(nsd.ada_lock),
    .mon.prefetch_ok = true,
    .ada.historical_acc = 700,
    .ada.recent_acc     = 700,
    .ada.learning_rate  = NSD_ML_LR_INIT,
};

static enum nsd_mode nsd_mode = NSD_MODE_NORMAL;
/* module parameters */
static bool param_waste_track = false;
module_param(param_waste_track, bool, 0644);
MODULE_PARM_DESC(param_waste_track, "Enable waste_track feature");
static bool param_observe_only = false;
module_param(param_observe_only, bool, 0644);
MODULE_PARM_DESC(param_observe_only, "Start in observe-only mode");
static bool param_penalty = true;
module_param(param_penalty, bool, 0644);
MODULE_PARM_DESC(param_penalty, "Enable penalty weaken on waste");


static struct kobject *nsd_kobj;

static int  nsd_hook_register(void);
static void nsd_hook_unregister(void);
static void nsd_drain_rings(void);
static unsigned long nsd_shrink_count(struct shrinker *, struct shrink_control *);
static unsigned long nsd_shrink_scan(struct shrinker *, struct shrink_control *);

static inline __maybe_unused u64 nsd_ns(void) { return ktime_get_ns(); }

static inline __maybe_unused u32 nsd_off_to_region(loff_t off)
{
    return (u32)((u64)off >> READ_ONCE(nsd.region_shift));
}

static __maybe_unused u32 nsd_file_id(struct file *file)
{
    struct inode *inode = file->f_inode;
    if (!inode) return 0;
    u64 key = inode->i_ino ^ (u64)(unsigned long)inode->i_sb;
    return hash_64(key, NSD_FCTX_BITS);
}

static inline __maybe_unused u64 nsd_syn_key(u32 file_id, u32 region)
{
    u64 d[2] = { (u64)file_id, (u64)region };
    return siphash(d, sizeof(d), &nsd.hkey);
}

static inline __maybe_unused u64 nsd_syn_key_bigram(u32 file_id, u32 prev_r, u32 cur_r)
{
    u64 d[2] = { ((u64)file_id << 32) | prev_r, (u64)cur_r };
    return siphash(d, sizeof(d), &nsd.hkey);
}

static inline __maybe_unused u64 nsd_procaware_hash(u64 base_hash)
{
    u32 tgid = current->tgid;
    u8 bucket = (u8)(tgid & ((1u << NSD_PROCAWARE_BUCKET_BITS) - 1));
    return base_hash ^ ((u64)bucket << 56);
}

static __maybe_unused int nsd_syn_init(void)
{
    u32 i, w;
    nsd.syn = kvmalloc_array(nsd.syn_buckets,
                              sizeof(struct nsd_syn_bucket),
                              GFP_KERNEL | __GFP_ZERO);
    if (!nsd.syn) return -ENOMEM;
    for (i = 0; i < nsd.syn_buckets; i++) {
        spin_lock_init(&nsd.syn[i].lock);
        for (w = 0; w < NSD_SYN_WAYS; w++) {
            int d;
            for (d = 0; d < NSD_MAX_DESTS; d++)
                nsd.syn[i].ways[w].weight[d] = 0;
            nsd.syn[i].ways[w].dst_count = 0;
        }
    }
    return 0;
}

static __maybe_unused void nsd_syn_free(void)
{
    kvfree(nsd.syn);
    nsd.syn = NULL;
}

static __maybe_unused void nsd_syn_strengthen(u64 key, u32 dst)
{
    u32 bi = (u32)(key & nsd.syn_mask), hi = (u32)(key >> 32);
    struct nsd_syn_bucket *b = &nsd.syn[bi];
    int i, weak = 0;
    u16 weak_w = NSD_W_MAX + 1;
    u64 now = nsd_ns();

    spin_lock(&b->lock);
    for (i = 0; i < NSD_SYN_WAYS; i++) {
        if (b->ways[i].key_hi == hi) {
            int d;
            for (d = 0; d < b->ways[i].dst_count; d++) {
                if (b->ways[i].dst_region[d] == dst) {

                    if (b->ways[i].weight[d] == 0)
                        atomic64_inc(&nsd.st_syn_active);
                    b->ways[i].weight[d] = min_t(u16, b->ways[i].weight[d] + NSD_W_OBSERVE, NSD_W_MAX);
                    b->ways[i].last_seen_ns = now;
                    spin_unlock(&b->lock);
                    return;
                }
            }

            if (b->ways[i].dst_count < NSD_MAX_DESTS) {
                b->ways[i].dst_region[b->ways[i].dst_count] = dst;
                b->ways[i].weight[b->ways[i].dst_count] = NSD_W_INIT;
                b->ways[i].dst_count++;
                b->ways[i].last_seen_ns = now;
                spin_unlock(&b->lock);
                return;
            }
        }
        if (b->ways[i].weight[0] < weak_w) {
            weak_w = b->ways[i].weight[0];
            weak   = i;
        }
    }

    if (b->ways[weak].weight[0] == 0)
        atomic64_inc(&nsd.st_syn_active);
    b->ways[weak].key_hi      = hi;
    b->ways[weak].dst_region[0] = dst;
    b->ways[weak].weight[0]   = NSD_W_INIT;
    b->ways[weak].dst_count   = 1;
    b->ways[weak].last_seen_ns = now;
    spin_unlock(&b->lock);
    atomic64_inc(&nsd.st_learned);
}

static __maybe_unused bool nsd_syn_predict(u64 key, u32 *dst, u16 *w, int *way_idx)
{
    u32 bi = (u32)(key & nsd.syn_mask), hi = (u32)(key >> 32);
    struct nsd_syn_bucket *b = &nsd.syn[bi];
    int i, best_way = -1, best_dest = -1;
    u16 bw = 0;

    spin_lock(&b->lock);
    for (i = 0; i < NSD_SYN_WAYS; i++) {
        if (b->ways[i].key_hi == hi && b->ways[i].dst_count > 0) {
            int d;
            for (d = 0; d < b->ways[i].dst_count; d++) {
                if (b->ways[i].weight[d] > bw) {
                    bw       = b->ways[i].weight[d];
                    best_way = i;
                    best_dest = d;
                }
            }
        }
    }
    if (best_way >= 0) {
        *dst = b->ways[best_way].dst_region[best_dest];
        *w   = bw;
        if (way_idx) *way_idx = best_way;

        if (atomic_read(&nsd.feat_fine_decay) && b->ways[best_way].last_seen_ns) {
            u64 now_ns = nsd_ns();
            u64 elapsed_ns = now_ns - b->ways[best_way].last_seen_ns;
            u64 half_life_ns = 30ULL * 60ULL * NSEC_PER_SEC;
            if (elapsed_ns > half_life_ns) {
                u64 half_lives = elapsed_ns / half_life_ns;
                if (half_lives >= 16)
                    bw = 0;
                else
                    bw = bw >> half_lives;
            }
        }
        *w = bw;
        spin_unlock(&b->lock);
        return true;
    }
    spin_unlock(&b->lock);
    return false;
}

static __maybe_unused void nsd_syn_reward(u64 key, u32 dst)
{
    u32 bi = (u32)(key & nsd.syn_mask), hi = (u32)(key >> 32);
    struct nsd_syn_bucket *b = &nsd.syn[bi];
    int i;
    spin_lock(&b->lock);
    for (i = 0; i < NSD_SYN_WAYS; i++) {
        if (b->ways[i].key_hi == hi) {
            int d;
            for (d = 0; d < b->ways[i].dst_count; d++) {
                if (b->ways[i].dst_region[d] == dst && b->ways[i].weight[d] > 0) {
                    b->ways[i].weight[d] = min_t(u16, b->ways[i].weight[d] + NSD_W_HIT, NSD_W_MAX);
                    break;
                }
            }
        }
    }
    spin_unlock(&b->lock);
}

static __maybe_unused void nsd_syn_weaken(u32 bw, u32 dst)
{
    u32 bi = (bw >> 8) & 0xFFFFFF;
    u32 wi = bw & 0xFF;
    struct nsd_syn_bucket *b = &nsd.syn[bi];
    if (wi >= NSD_SYN_WAYS) return;
    spin_lock(&b->lock);
    for (int d = 0; d < b->ways[wi].dst_count; d++) {
        if (b->ways[wi].dst_region[d] == dst && b->ways[wi].weight[d] > 0) {
            if (b->ways[wi].weight[d] <= (u16)NSD_W_PENALTY)
                b->ways[wi].weight[d] = 0;
            else
                b->ways[wi].weight[d] = b->ways[wi].weight[d] - NSD_W_PENALTY;
            break;
        }
    }
    spin_unlock(&b->lock);
}


static __maybe_unused void nsd_syn_decay(struct work_struct *work)
{
    static u32 decay_runs = 0;
    u32 i, w, d;
    u64 now = nsd_ns();
    u64 thr_idle;
    u64 thr_aggressive;
    bool aggressive;

    decay_runs++;
    thr_idle       = (u64)NSD_DECAY_AGE_RUNS * (u64)NSD_W_DECAY_MS * 1000000ULL;
    thr_aggressive = (u64)NSD_W_DECAY_MS * 1000000ULL;


    if (decay_runs <= NSD_DECAY_RUNS_MIN)
        goto done;


    {
        u64 capacity = (u64)nsd.syn_buckets * (u64)NSD_SYN_WAYS;
        u64 active   = (u64)atomic64_read(&nsd.st_syn_active);
        aggressive   = (capacity > 0) &&
                       (active * 100u / capacity) >= NSD_DECAY_AGGRESSIVE_PCT;
    }

    for (i = 0; i < nsd.syn_buckets; i++) {
        struct nsd_syn_bucket *b = &nsd.syn[i];
        spin_lock(&b->lock);
        for (w = 0; w < NSD_SYN_WAYS; w++) {
            if (b->ways[w].dst_count == 0) continue;
            u64 thr = aggressive ? thr_aggressive : thr_idle;
            if (now - b->ways[w].last_seen_ns > thr) {
                for (d = 0; d < b->ways[w].dst_count; d++) {
                    if (b->ways[w].weight[d] == 0) continue;
                    b->ways[w].weight[d] >>= 1;
                    if (b->ways[w].weight[d] == 0) {
                        if (atomic64_read(&nsd.st_syn_active) > 0)
                            atomic64_dec(&nsd.st_syn_active);
                    }
                }
            }
        }
        spin_unlock(&b->lock);
        if ((i & 0xFF) == 0xFF) cond_resched();
    }
done:
    atomic64_inc(&nsd.st_decay);
    if (atomic_read(&nsd.running))
        queue_delayed_work(nsd.wq, &nsd.decay_work,
                           msecs_to_jiffies(NSD_W_DECAY_MS));
}

static __maybe_unused void nsd_hot_update(struct nsd_fctx *ctx, u32 region, u64 now)
{
    int i, oldest_slot = 0;
    u64 oldest_ns = ~0ULL;


    for (i = 0; i < ctx->hot_used; i++) {
        if (ctx->hot[i].region == region) {
            ctx->hot[i].count++;
            ctx->hot[i].last_ns = now;
            return;
        }
    }


    if (ctx->hot_used < NSD_HOT_SLOTS) {
        ctx->hot[ctx->hot_used].region  = region;
        ctx->hot[ctx->hot_used].count   = 1;
        ctx->hot[ctx->hot_used].last_ns = now;
        ctx->hot_used++;
    } else {
        for (i = 0; i < NSD_HOT_SLOTS; i++) {
            if (ctx->hot[i].last_ns < oldest_ns) {
                oldest_ns   = ctx->hot[i].last_ns;
                oldest_slot = i;
            }
        }
        ctx->hot[oldest_slot].region  = region;
        ctx->hot[oldest_slot].count   = 1;
        ctx->hot[oldest_slot].last_ns = now;
    }
}

static __maybe_unused bool nsd_hot_predict(struct nsd_fctx *ctx, u32 cur_region,
                             u32 *dst, u16 *score, u64 now)
{
    int i, best = -1;
    u32 best_score = 0;

    for (i = 0; i < ctx->hot_used; i++) {
        u64 age_ms;
        u32 s;

        if (ctx->hot[i].region == cur_region) continue;
        if (ctx->hot[i].count < 2) continue;

        age_ms = (now - ctx->hot[i].last_ns) / 1000000ULL;
        if (age_ms > 60000) continue;


        s = ctx->hot[i].count * (age_ms < 5000  ? 1000 :
                                   age_ms < 15000 ? 700  :
                                   age_ms < 30000 ? 400  : 200);
        s /= 1000;

        if (s > best_score) {
            best_score = s;
            best = i;
        }
    }

    if (best >= 0) {
        *dst   = ctx->hot[best].region;
        *score = (u16)min_t(u32, best_score * 100, NSD_W_MAX);
        return true;
    }
    return false;
}

static __maybe_unused void nsd_stride_update(struct nsd_fctx *ctx, u32 region, s32 delta)
{
    if (delta == 0) {

        ctx->stride_count = 0;
        ctx->stride_last_region = region;
        return;
    }


    ctx->stride_deltas[2] = ctx->stride_deltas[1];
    ctx->stride_deltas[1] = ctx->stride_deltas[0];
    ctx->stride_deltas[0] = delta;

    if (ctx->stride_count < 3)
        ctx->stride_count++;

    ctx->stride_last_region = region;
}

static __maybe_unused bool nsd_stride_predict(struct nsd_fctx *ctx, u32 cur_region,
                                u32 *dst, u16 *score, s32 *stride_out)
{
    s32 d0, d1, d2;


    if (ctx->stride_count < 3) return false;

    d0 = ctx->stride_deltas[0];
    d1 = ctx->stride_deltas[1];
    d2 = ctx->stride_deltas[2];


    if (d0 == 0 || d0 != d1 || d1 != d2) return false;

    *dst   = (u32)((s64)cur_region + d0);
    *score = 900;
    if (stride_out)
        *stride_out = d0;
    return true;
}

static inline int nsd_strat_rank(enum nsd_strategy s)
{
    switch (s) {
    case NSD_STRAT_NONE:         return 0;
    case NSD_STRAT_MARKOV:       return 1;
    case NSD_STRAT_BIGRAM:       return 2;
    case NSD_STRAT_FREQ_RECENCY: return 3;
    }
    return 0;
}

static __maybe_unused void nsd_workload_update(struct nsd_fctx *ctx, u32 region,
                                 u32 prev_region, u64 now)
{
    struct nsd_workload *w = &ctx->wl;
    s64 delta = (s64)region - (s64)prev_region;
    int i;

    w->event_count++;
    nsd_hot_update(ctx, region, now);


    if (ctx->total_bytes_read > 0 && ctx->total_bytes_read < NSD_SMALL_IO_THRESH) {
        ctx->small_io_ops++;
    }


    if (delta == w->last_delta && delta != 0)
        w->seq_count++;
    w->last_delta = delta;


    for (i = 0; i < ctx->hot_used; i++) {
        if (ctx->hot[i].region == region && ctx->hot[i].count >= 2) {
            w->repeat_count++;
            break;
        }
    }


    if (w->event_count >= NSD_PROFILE_WINDOW ||
        ((w->event_count & 31U) == 0 && w->event_count >= 32)) {
        u32 seq_r = (w->seq_count * 1000) / w->event_count;
        u32 rpt_r = (w->repeat_count * 1000) / w->event_count;
        enum nsd_strategy proposed;
        bool full = (w->event_count >= NSD_PROFILE_WINDOW);


        if (seq_r < 120 && rpt_r < 100) {
            proposed = NSD_STRAT_NONE;
        } else if (full && rpt_r > NSD_RPT_RATIO_THRESH) {
            proposed = NSD_STRAT_FREQ_RECENCY;
        } else if (full && seq_r > 200 && rpt_r > 150) {
            proposed = NSD_STRAT_BIGRAM;
        } else if (rpt_r > (full ? 75U : 200U)) {
            proposed = NSD_STRAT_FREQ_RECENCY;
        } else {
            proposed = NSD_STRAT_MARKOV;
        }


        if (proposed == w->active) {
            w->pending_confirms = 0;
            w->pending = proposed;
        } else if (proposed == w->pending) {
            w->pending_confirms++;
            u8 thresh;
            if (nsd_strat_rank(proposed) > nsd_strat_rank(w->active)) {
                if (seq_r < 120 && rpt_r < 100)
                    thresh = NSD_HYST_CONFIRM_UP_RND;
                else
                    thresh = NSD_HYST_CONFIRM_UP;
            } else {
                thresh = NSD_HYST_CONFIRM_DOWN;
            }
            if (w->pending_confirms >= thresh) {
                w->active           = w->pending;
                w->pending_confirms = 0;
                atomic64_inc(&nsd.st_strat_switch);
            }
        } else {
            w->pending          = proposed;
            w->pending_confirms = 1;
        }


        if (proposed == NSD_STRAT_NONE && seq_r > 600)
            atomic64_inc(&nsd.st_seq_bypass);

        WRITE_ONCE(nsd.skip_kprobe[ctx->file_id], false);

        w->seq_count    = 0;
        w->repeat_count = 0;
        w->event_count  = 0;
    }
}

static __maybe_unused void nsd_adaptive_update(struct nsd_fctx *ctx, bool correct)
{
    struct nsd_adaptive *a = &ctx->ada;
    u32 idx;

    spin_lock(&nsd.ada_lock);


    idx = a->recent_idx % NSD_ML_WINDOW_SIZE;
    if (a->recent_idx >= NSD_ML_WINDOW_SIZE)
        a->recent_sum -= a->recent_window[idx];
    a->recent_window[idx] = correct ? 1000 : 0;
    a->recent_sum        += correct ? 1000 : 0;
    a->recent_idx++;
    if (a->recent_idx >= NSD_ML_WINDOW_SIZE)
        a->recent_acc = a->recent_sum / NSD_ML_WINDOW_SIZE;


    a->historical_acc =
        (u16)((a->historical_acc * 990 + (correct ? 1000 : 0) * 10) / 1000);


    if (a->recent_acc > 800 && a->learning_rate > 1)
        a->learning_rate--;
    else if (a->recent_acc < 400 && a->learning_rate < 100)
        a->learning_rate++;


    if (atomic_read(&nsd.feat_autothresh)) {
        u16 cur_thresh = ctx->thresh;
        u16 new_thresh = cur_thresh;
        u16 high_acc, low_acc, step, min_thresh, max_thresh;

        if (nsd.dev_class == NSD_DEV_HDD) {
            high_acc = NSD_AUTOTHRESH_HIGH_ACC_HDD;
            low_acc  = NSD_AUTOTHRESH_LOW_ACC_HDD;
            step     = NSD_AUTOTHRESH_STEP_HDD;
            min_thresh = NSD_THRESH_HDD;
            max_thresh = NSD_THRESH_HDD + 50;
        } else {
            high_acc = NSD_AUTOTHRESH_HIGH_ACC;
            low_acc  = NSD_AUTOTHRESH_LOW_ACC;
            step     = NSD_AUTOTHRESH_STEP;
            min_thresh = NSD_THRESH_MIN;
            max_thresh = NSD_THRESH_MAX;
        }

        if (a->recent_acc > high_acc) {
            if (cur_thresh > min_thresh + step)
                new_thresh = cur_thresh - step;
        } else if (a->recent_acc < low_acc) {
            if (cur_thresh < max_thresh - step)
                new_thresh = cur_thresh + step;
        }

        if (new_thresh != cur_thresh) {
            ctx->thresh = new_thresh;
            atomic64_inc(&nsd.st_autothresh_adj);
        }


        if (a->recent_acc < 100 && cur_thresh > 300) {
            u16 reset_thresh = max(min_thresh, (u16)(cur_thresh / 2));
            ctx->thresh = reset_thresh;
            atomic64_inc(&nsd.st_autothresh_adj);
            pr_debug("NSD: death spiral recovery thresh %u -> %u\n",
                     cur_thresh, reset_thresh);
        }
    }

    spin_unlock(&nsd.ada_lock);
}

static struct nsd_fctx *nsd_fctx_get(u32 file_id, u64 now, struct inode *inode)
{
    int i, lru = 0;
    u64 oldest = ~0ULL;


    for (i = 0; i < NSD_FCTX_SLOTS; i++) {
        if (nsd.fctx.e[i].valid && nsd.fctx.e[i].file_id == file_id) {
            nsd.fctx.e[i].read_count++;
            nsd.fctx.e[i].inode = inode;
            if (now - nsd.fctx.e[i].last_seen_ns > 100ULL * NSEC_PER_MSEC) {
                WRITE_ONCE(nsd.skip_kprobe[file_id], false);
            }
            nsd.fctx.e[i].last_seen_ns = now;
            return &nsd.fctx.e[i];
        }
        if (!nsd.fctx.e[i].valid) {
            lru = i;
            oldest = 0;
        } else if (nsd.fctx.e[i].last_seen_ns < oldest) {
            oldest = nsd.fctx.e[i].last_seen_ns;
            lru    = i;
        }
    }


    memset(&nsd.fctx.e[lru], 0, sizeof(nsd.fctx.e[lru]));
    nsd.fctx.e[lru].file_id      = file_id;
    nsd.fctx.e[lru].inode        = inode;
    nsd.fctx.e[lru].valid        = true;
    nsd.fctx.e[lru].read_count   = 1;
    nsd.fctx.e[lru].last_seen_ns = now;
    nsd.fctx.e[lru].wl.active    = NSD_STRAT_MARKOV;
    WRITE_ONCE(nsd.skip_kprobe[file_id], false);
    nsd.fctx.e[lru].depth          = READ_ONCE(nsd.depth);
    nsd.fctx.e[lru].thresh         = READ_ONCE(nsd.thresh);
    nsd.fctx.e[lru].prefetch_span  = READ_ONCE(nsd.prefetch_span);
    nsd.fctx.e[lru].ada.historical_acc = 700;
    nsd.fctx.e[lru].ada.recent_acc     = 700;
    nsd.fctx.e[lru].ada.learning_rate  = NSD_ML_LR_INIT;
    nsd.fctx.e[lru].mon.prefetch_ok    = true;
    nsd.fctx.e[lru].meta_ops     = 0;
    nsd.fctx.e[lru].small_io_ops = 0;
    nsd.fctx.e[lru].total_bytes_read = 0;

    nsd.fctx.e[lru].stride_count = 0;
    return &nsd.fctx.e[lru];
}

static inline __maybe_unused u64 nsd_pend_hash(u32 file_id, u32 region)
{
    u64 k[2] = { (u64)file_id, (u64)region };
    return siphash(k, sizeof(k), &nsd.hkey);
}

static inline __maybe_unused void nsd_pend_add(u64 h, u32 way, u32 dst, u32 kind)
{
    u32 s = h & NSD_PENDING_MASK;
    if (READ_ONCE(nsd.pending[s]) && READ_ONCE(nsd.pending[s]) != h)
        atomic64_inc(&nsd.st_pending_overwrite);
    WRITE_ONCE(nsd.pending[s], h);    WRITE_ONCE(nsd.pending_ts[s], nsd_ns());    WRITE_ONCE(nsd.pending_way[s], way);    WRITE_ONCE(nsd.pending_dst[s], dst);    WRITE_ONCE(nsd.pending_kind[s], kind);
}

static inline __maybe_unused bool nsd_pend_check(u64 h)
{
    u32 s = (u32)(h & NSD_PENDING_MASK);
    if (READ_ONCE(nsd.pending[s]) == h) {
        WRITE_ONCE(nsd.pending[s], 0ULL);        WRITE_ONCE(nsd.pending_ts[s], 0ULL);        WRITE_ONCE(nsd.pending_way[s], 0);        WRITE_ONCE(nsd.pending_dst[s], 0);        WRITE_ONCE(nsd.pending_kind[s], 0);
        return true;
    }
    return false;
}

static inline __maybe_unused bool nsd_pend_test(u64 h)
{
    if (READ_ONCE(nsd.pending[h & NSD_PENDING_MASK]) == h) {
        atomic64_inc(&nsd.st_pending_skip);
        return true;
    }
    return false;
}

static __maybe_unused void nsd_mon_update(struct nsd_fctx *ctx, bool correct)
{
    u32 idx, acc;
    /* no lock - per-fctx */
    idx = nsd.mon.idx % NSD_MON_WINDOW;
    if (nsd.mon.count >= NSD_MON_WINDOW)
        nsd.mon.sum -= nsd.mon.window[idx];
    nsd.mon.window[idx] = correct ? 1000 : 0;
    nsd.mon.sum        += correct ? 1000 : 0;
    nsd.mon.idx++;
    if (nsd.mon.count < NSD_MON_WINDOW) nsd.mon.count++;
    acc = nsd.mon.count > 0 ? nsd.mon.sum / nsd.mon.count : 500;
    if (acc < NSD_MON_DISABLE && nsd.mon.count >= NSD_MON_WINDOW)
        ctx->mon.prefetch_ok = false;
    else if (acc > NSD_MON_ENABLE)
        nsd.mon.prefetch_ok = true;
    /* no lock - per-fctx */
}

static __maybe_unused bool nsd_ring_put(struct file *file, loff_t off, size_t size)
{
    struct nsd_pcpu_ring *r = this_cpu_ptr(&nsd_ring);
    u32 p    = (u32)atomic_read(&r->prod);
    u32 next = (p + 1u) & NSD_RING_MASK;

    if (next == (u32)atomic_read(&r->cons))
        return false;

    get_file(file);
    r->slots[p].file   = file;
    r->slots[p].offset = off;
    r->slots[p].size   = size;
    smp_wmb();
    atomic_set(&r->prod, (int)next);

    if (nsd.worker)
        wake_up_process(nsd.worker);
    return true;
}

static inline __maybe_unused void nsd_track_micro_seq(struct inode *inode, loff_t off, size_t len)
{
    struct nsd_pcpu_state *st = this_cpu_ptr(&nsd_cpu_state);
    u32 pid = current->pid;
    u32 ino = inode->i_ino;

    if (st->micro_seq_last_inode == ino &&
        st->micro_seq_last_pid == pid &&
        off == st->micro_seq_last_offset + st->micro_seq_last_len) {
        st->micro_seq_hits++;
        if (st->micro_seq_hits > 3)
            st->micro_seq_hits = 4;
    } else {
        st->micro_seq_hits = 0;
    }

    st->micro_seq_last_inode = ino;
    st->micro_seq_last_pid   = pid;
    st->micro_seq_last_offset = off;
    st->micro_seq_last_len    = len;
    st->micro_seq_last_ns     = nsd_ns();
}

static __maybe_unused int nsd_fprobe_entry(struct fprobe *fp, unsigned long entry_ip,
                             unsigned long ret_ip, struct ftrace_regs *fregs,
                             void *entry_data)
{
    struct file  *file;
    struct inode *inode;
    loff_t       *pos_ptr, off;
    (void)fp; (void)entry_ip; (void)ret_ip; (void)entry_data;

    file    = (struct file *)ftrace_regs_get_argument(fregs, 0);
    pos_ptr = (loff_t *)ftrace_regs_get_argument(fregs, 3);
    size_t io_size = (size_t)ftrace_regs_get_argument(fregs, 2);

    if (io_size < 4096) return 0;
    if (!file || !pos_ptr) return 0;

    inode = file->f_inode;
    if (!inode || !S_ISREG(inode->i_mode)) return 0;
    if (!(file->f_mode & FMODE_READ)) return 0;

    off = *pos_ptr;
    if (off < 0) return 0;

    if (atomic_read(&nsd.feat_procaware))
        nsd_track_micro_seq(inode, off, io_size);

    if (true) {
        atomic64_inc(&nsd.st_kprobe);
        u64 key = (u64)inode->i_ino ^ (u64)(unsigned long)inode->i_sb;
        u32 file_id = hash_64(key, NSD_FCTX_BITS);
        bool skip = READ_ONCE(nsd.skip_kprobe[file_id]) &&
                    READ_ONCE(nsd.skip_ino[file_id]) == (u64)inode->i_ino;
        if (skip && nsd_ns() - READ_ONCE(nsd.skip_time[file_id]) > 5ULL * NSEC_PER_SEC) {
            WRITE_ONCE(nsd.skip_kprobe[file_id], false);
            skip = false;
        }
        if (!skip && !nsd_ring_put(file, off, io_size))
            atomic64_inc(&nsd.st_dropped);
    }

    return 0;
}

static struct fprobe nsd_fp = {
    .entry_handler = nsd_fprobe_entry,
};

static __maybe_unused int nsd_hook_register(void)
{
    int ret;
    if (!atomic_read(&nsd.hook_on) || atomic_read(&nsd.hook_reg)) return 0;
    {
        const char *syms[] = { "vfs_read" };
        ret = register_fprobe_syms(&nsd_fp, syms, 1);
    }
    if (ret) return ret;
    atomic_set(&nsd.hook_reg, 1);
    pr_info("NSD: vfs_read fprobe active\n");
    return 0;
}

static __maybe_unused void nsd_hook_unregister(void)
{
    if (!atomic_read(&nsd.hook_reg)) return;
    unregister_fprobe(&nsd_fp);
    atomic_set(&nsd.hook_reg, 0);
}

static __maybe_unused u32 nsd_calc_adaptive_span_mult(enum nsd_strategy strat, struct nsd_fctx *ctx)
{
    u32 mult = NSD_SPAN_MULT_SEQ;

    if (!ctx)
        return mult;


    u32 total_ops = ctx->read_count + ctx->meta_ops;
    if (total_ops > 0) {
        u32 meta_ratio = (ctx->meta_ops * 1000) / total_ops;
        if (meta_ratio >= NSD_META_RATIO_THRESH)
            return NSD_SPAN_MULT_META;
    }


    if (ctx->read_count > 0) {
        u32 small_ratio = (ctx->small_io_ops * 1000) / ctx->read_count;
        if (small_ratio >= 500)
            return NSD_SPAN_MULT_META;
    }

    switch (strat) {
    case NSD_STRAT_NONE:
        mult = NSD_SPAN_MULT_META;
        break;
    case NSD_STRAT_MARKOV:
    case NSD_STRAT_BIGRAM:
        mult = NSD_SPAN_MULT_SEQ;
        break;
    case NSD_STRAT_FREQ_RECENCY:
        mult = NSD_SPAN_MULT_MIXED;
        break;
    default:
        mult = NSD_SPAN_MULT_MIXED;
        break;
    }

    return mult;
}

enum nsd_workload_type {
    NSD_WL_UNKNOWN = 0,
    NSD_WL_SEQUENTIAL,
    NSD_WL_RANDOM,
    NSD_WL_PATTERN,
    NSD_WL_MIXED,
};

static enum nsd_workload_type nsd_update_workload_state(struct nsd_fctx *ctx, u32 region, u64 now)
{
    struct nsd_workload *w = &ctx->wl;
    u32 seq_r, rpt_r;
    (void)now;

    if (w->event_count < 32)
        return NSD_WL_UNKNOWN;

    seq_r = (w->seq_count * 1000) / max(w->event_count, (u32)1);
    rpt_r = (w->repeat_count * 1000) / max(w->event_count, (u32)1);

    if (seq_r > 800)
        return NSD_WL_SEQUENTIAL;
    if (rpt_r > 600)
        return NSD_WL_PATTERN;
    if (seq_r < 100 && rpt_r < 100)
        return NSD_WL_RANDOM;
    return NSD_WL_MIXED;
}

static __maybe_unused void nsd_do_prefetch(struct file *file, u32 file_id,
                             u32 region, u32 prev_region,
                             bool has_prev, struct nsd_fctx *ctx)
{
    u32 r = region;
    u32 depth, d;
    u32 depth_boost = 0;
    u16 thresh;
    u64 span;
    enum nsd_strategy strat;

    /* SSD stratejik optimizasyon #1: seq bypass (refined)
     * Stabil stride (d0==d1==d2, 3+ tekrar) ve ileri yönlü (d0>0)
     * ve makul aralikta (d0<=256, ~1MB) ise kernel readahead devreye girer.
     * Sadece MARKOV/SEQ stratejilerinde; FREQ_RECENCY'de prefetch anlamlı. */
    if (ctx && ctx->stride_count >= 3 && ctx->wl.active != NSD_STRAT_FREQ_RECENCY) {
        s32 d0 = ctx->stride_deltas[0];
        s32 d1 = ctx->stride_deltas[1];
        s32 d2 = ctx->stride_deltas[2];
        if (d0 > 0 && d0 <= 256 && d0 == d1 && d1 == d2) {
            atomic64_inc(&nsd.st_seq_bypass);
            return;
        }
    }

    if (atomic_read(&nsd.feat_procaware)) {
        struct nsd_pcpu_state *st = this_cpu_ptr(&nsd_cpu_state);
        if (st->micro_seq_hits >= 3)
            depth_boost = 4;
    }

    /* Phase 5: stride_predict bir kez çağr, sonucu sakla */
    bool stride_found_early = false;
    u32  stride_next_r = 0;
    u16  stride_w      = 0;
    s32  stride_delta_early = 0;
    if (atomic_read(&nsd.feat_stride) && ctx) {
        if (nsd_stride_predict(ctx, r, &stride_next_r, &stride_w, &stride_delta_early)) {
            atomic64_inc(&nsd.st_stride_predictions);
            stride_found_early = true;
        }
    }


    if (atomic_read(&nsd.observe_only)) return;
    if (ctx && ctx->disabled && nsd_ns() < ctx->negcache_until)
        return;


    depth  = ctx ? ctx->depth : READ_ONCE(nsd.depth);
    if (depth_boost > depth) depth = depth_boost;
    /* queue depth limit kaldirildi — SSD/HDD qd=32 > 24 olduğu için
     * depth=1'e zorlanıyordu ve prefetch felç oluyordu */
    /* if (file->f_inode && file->f_inode->i_sb && file->f_inode->i_sb->s_bdev) {
        struct request_queue *q = bdev_get_queue(file->f_inode->i_sb->s_bdev);
        if (q) {
            unsigned int qd = blk_queue_depth(q);
            if (qd > 24)
                depth = 1;
            else if (qd > 16)
                depth = min(depth, (u32)2);
        }
    } */

    thresh = READ_ONCE(nsd.thresh);
    span   = ctx ? ctx->prefetch_span : READ_ONCE(nsd.prefetch_span);
    strat  = ctx ? ctx->wl.active : NSD_STRAT_MARKOV;
    if (strat == NSD_STRAT_NONE)
        return;


    int  consecutive_dup = 0;
    u32  coalesce_start = 0;
    u32  coalesce_len   = 0;
    bool coalesce_is_stride = false;


    bool use_stride_chain = false;
    s32  stride_delta    = 0;
    u32  stride_base     = 0;
    u16  stride_score0   = 0;
    u32  max_region      = (~0U) >> 1;

#define FLUSH_COALESCE() do {                                            \
    if (coalesce_len > 0) {                                              \
        loff_t start = (loff_t)coalesce_start << READ_ONCE(nsd.region_shift); \
        u64 len;                                                          \
        u32 span_mult = nsd_calc_adaptive_span_mult(strat, ctx);         \
        if (coalesce_len == 1 && coalesce_is_stride) {                   \
                       \
            u32 stride_span = READ_ONCE(nsd.prefetch_span) *             \
                              span_mult;                                 \
            loff_t file_size = i_size_read(file_inode(file));            \
            loff_t remaining = file_size - start;                        \
            if (remaining < 0) remaining = 0;                           \
            if ((u64)stride_span > (u64)remaining)                       \
                stride_span = (u32)remaining;                            \
            len = (u64)stride_span;                                      \
        } else {                                                          \
            len = (u64)coalesce_len * (u64)READ_ONCE(nsd.prefetch_span) * \
                  span_mult;                                             \
        }                                                                 \
        if (strat == NSD_STRAT_MARKOV || strat == NSD_STRAT_BIGRAM)       \
            vfs_fadvise(file, start, len, POSIX_FADV_SEQUENTIAL);        \
        else                                                              \
            vfs_fadvise(file, start, len, POSIX_FADV_WILLNEED | POSIX_FADV_NOREUSE); \
        if (coalesce_len > 1)                                            \
            atomic64_inc(&nsd.st_coalesced);                             \
        atomic64_inc(&nsd.st_prefetch);                                  \
        if (ctx) ctx->pf_count++;                                        \
        coalesce_start     = 0;                                          \
        coalesce_len       = 0;                                          \
        coalesce_is_stride = false;                                      \
    }                                                                    \
} while (0)

    for (d = 0; d < depth; d++) {
        u32  next_r = 0;
        u16  w      = 0;
        bool found  = false;
        u64  ph;
        int  pred_way = -1;
        u64  pred_key = 0;


        if (use_stride_chain) {
            s64 cand = (s64)stride_base + (s64)d * (s64)stride_delta;
            if (cand < 0 || cand > (s64)max_region) {
                use_stride_chain = false;
                break;
            }
            next_r = (u32)cand;

            w = (u16)((u32)stride_score0 - (u32)stride_score0 * 2u * (u32)d / 100u);
            if (w < thresh) {
                use_stride_chain = false;
                break;
            }
            found = true;
            goto stride_chain_emit;
        }


        /* Phase 5: İlk çağrı sonucunu kullan, tekrar çağırma */
        if (stride_found_early && strat != NSD_STRAT_NONE) {
            next_r      = stride_next_r;
            w           = stride_w;
            stride_delta = stride_delta_early;
            found       = true;
            atomic64_inc(&nsd.st_stride_predictions);

            if (stride_delta != 0) {

                coalesce_is_stride = true;
                use_stride_chain   = true;
                stride_base        = next_r;
                stride_score0      = w;
                atomic64_inc(&nsd.st_stride_chain);
            }
        } else if (strat != NSD_STRAT_NONE) {
        switch (strat) {
        case NSD_STRAT_BIGRAM:
            	    pred_key = nsd_syn_key_bigram(file_id, prev_region, r);
            found = nsd_syn_predict(pred_key, &next_r, &w, &pred_way);

            if (found)
                prev_region = r;
            break;
        case NSD_STRAT_FREQ_RECENCY:
            found = nsd_hot_predict(ctx, r, &next_r, &w, nsd_ns());
            break;
        case NSD_STRAT_MARKOV:
        default:
            pred_key = nsd_syn_key(file_id, r);
            found = nsd_syn_predict(pred_key, &next_r, &w, &pred_way);
            break;
        }
        }

        if (!found) break;
        if (w < thresh) break;
        if (next_r == r) break;

stride_chain_emit:
        atomic64_inc(&nsd.st_predictions);
        r = next_r;


        ph = nsd_pend_hash(file_id, next_r);
        if (nsd_pend_test(ph)) {

            FLUSH_COALESCE();
            consecutive_dup++;
            if (consecutive_dup >= 2)
                break;
            continue;
        }
        consecutive_dup = 0;


        {
        u32 pend_kind = NSD_PEND_NONE;
        u32 pend_way  = 0;
        if (use_stride_chain) {
            pend_kind = NSD_PEND_STRIDE;
            pend_way  = file_id;
        } else if (pred_way >= 0 && pred_key != 0) {
            pend_kind = NSD_PEND_SYNAPTIC;
            pend_way  = ((u32)(pred_key & nsd.syn_mask) << 8) | ((u32)pred_way + 1);
        }
        nsd_pend_add(ph, pend_way, next_r, pend_kind);
        }


        if (coalesce_len == 0) {
            coalesce_start     = next_r;
            coalesce_len       = 1;
            coalesce_is_stride = use_stride_chain;
        } else if (next_r == coalesce_start + coalesce_len) {
            coalesce_len++;
        } else {
            FLUSH_COALESCE();
            coalesce_start     = next_r;
            coalesce_len       = 1;
            coalesce_is_stride = use_stride_chain;
        }
    }


    FLUSH_COALESCE();
#undef FLUSH_COALESCE
}

static __maybe_unused void nsd_process(struct file *file, loff_t off, size_t size)
{
    u32  file_id = nsd_file_id(file);
    u32  region  = nsd_off_to_region(off);
    int  cpu     = smp_processor_id();
    struct nsd_pcpu_state *st = per_cpu_ptr(&nsd_cpu_state, cpu);
    struct nsd_pcpu_slot  *cs = NULL;
    u64  oldest_ns = U64_MAX;
    int  oldest_i  = 0;
    int  i;
    struct nsd_fctx *ctx;
    u64  now     = nsd_ns();
    bool has_prev;
    u32  prev_r = 0;

    atomic64_inc(&nsd.st_events);


    spin_lock(&nsd.fctx.lock);
    ctx = nsd_fctx_get(file_id, now, file->f_inode);
    spin_unlock(&nsd.fctx.lock);


    if (ctx) {
        ctx->total_bytes_read += size;
        if (size < NSD_SMALL_IO_THRESH)
            ctx->small_io_ops++;
    }


    for (i = 0; i < NSD_PCPU_SLOTS; i++) {
        if (st->slots[i].valid && st->slots[i].file_id == file_id) {
            cs = &st->slots[i];
            break;
        }
        if (st->slots[i].last_ns < oldest_ns) {
            oldest_ns = st->slots[i].last_ns;
            oldest_i  = i;
        }
    }

    if (!cs) {
        cs = &st->slots[oldest_i];
        cs->valid = false;
    }

    has_prev = (cs->valid && cs->file_id == file_id && cs->region != region);
    if (has_prev) prev_r = cs->region;


    cs->prev_region = has_prev ? cs->region : region;
    cs->file_id     = file_id;
    cs->region      = region;
    cs->valid       = true;
    cs->last_ns     = now;


    if (has_prev) {
        s32 stride_delta = (s32)region - (s32)prev_r;
        spin_lock(&nsd.fctx.lock);
        nsd_workload_update(ctx, region, prev_r, now);
        nsd_stride_update(ctx, region, stride_delta);
        spin_unlock(&nsd.fctx.lock);
    }


    if (!atomic_read(&nsd.observe_only)) {
        u64 ph = nsd_pend_hash(file_id, region);
        if (nsd_pend_check(ph)) {
            atomic64_inc(&nsd.st_correct);
            nsd_mon_update(ctx, true);
            nsd_adaptive_update(ctx, true);

            ctx->hit_count++;
            if (has_prev)
                nsd_syn_reward(nsd_syn_key(file_id, prev_r), region);
        } else {

            if (ctx) {
                ctx->miss_count++;
                if ((ctx->miss_count & 7U) == 0 && ctx->pf_count > 0) {
                    u32 acc = ctx->hit_count * 100 / ctx->pf_count;
                    if (acc < NSD_FILE_ACC_MIN && !ctx->disabled) {
                        atomic64_inc(&nsd.st_disabled);
                        ctx->disabled = true;
                    }
                }
                if (ctx) {
                    u32 acc2 = ctx->hit_count * 100 / max(ctx->pf_count, (u32)1);
                    if (acc2 < 10 && ctx->pf_count > 5) {
                        ctx->negcache_hits++;
                        if (ctx->negcache_hits >= 3) {
                            ctx->negcache_until = nsd_ns() + 10ULL * NSEC_PER_SEC;
                            ctx->disabled = true;
                        }
                    }
                    if (nsd_ns() > ctx->negcache_until) {
                        ctx->negcache_until = 0;
                        ctx->negcache_hits = 0;
                        ctx->disabled = false;
                    }
                }

                nsd_mon_update(ctx, false);
                nsd_adaptive_update(ctx, false);
            }
        }
    }

    if (!has_prev) return;


    nsd_syn_strengthen(nsd_syn_key(file_id, prev_r), region);


    if (cs->prev_region != prev_r)
        nsd_syn_strengthen(nsd_syn_key_bigram(file_id, cs->prev_region, prev_r), region);


    if (ctx && ctx->wl.active == NSD_STRAT_NONE)
        return;

    nsd_do_prefetch(file, file_id, region, prev_r, has_prev, ctx);
}

static __maybe_unused int nsd_worker(void *data)
{
    int cpu;
    (void)data;

    while (!kthread_should_stop()) {
        bool did_work = false;

        if (!atomic_read(&nsd.running)) {
            schedule_timeout_interruptible(HZ / 10);
            continue;
        }

        for_each_online_cpu(cpu) {
            struct nsd_pcpu_ring *ring = per_cpu_ptr(&nsd_ring, cpu);
            int budget = NSD_RING_SIZE;

            while (budget-- > 0) {
                struct file *file;
                loff_t off;
                size_t size;
                u32 prod, cons;

                prod = (u32)atomic_read(&ring->prod);
                smp_rmb();
                cons = (u32)atomic_read(&ring->cons);
                if (cons == prod) break;

                file = ring->slots[cons].file;
                off  = ring->slots[cons].offset;
                size = ring->slots[cons].size;
                cons = (cons + 1u) & NSD_RING_MASK;
                atomic_set(&ring->cons, (int)cons);

                if (file) {
                    nsd_process(file, off, size);
                    fput(file);
                    did_work = true;
                }
            }
        }


        if (!did_work)
            schedule_timeout_interruptible(max(1, HZ / 500));
    }
    return 0;
}

static __maybe_unused void nsd_drain_rings(void)
{
    int cpu;
    for_each_possible_cpu(cpu) {
        struct nsd_pcpu_ring *ring = per_cpu_ptr(&nsd_ring, cpu);
        u32 prod = (u32)atomic_read(&ring->prod);
        u32 cons = (u32)atomic_read(&ring->cons);
        while (cons != prod) {
            struct file *file = ring->slots[cons].file;
            if (file) fput(file);
            cons = (cons + 1u) & NSD_RING_MASK;
        }
        atomic_set(&ring->cons, (int)prod);
    }
}

static unsigned long nsd_shrink_count(struct shrinker *s, struct shrink_control *sc)
{
    (void)s; (void)sc;
    return (unsigned long)atomic64_read(&nsd.st_syn_active);
}

static unsigned long nsd_shrink_scan(struct shrinker *s, struct shrink_control *sc)
{
    unsigned long freed = 0, target = sc->nr_to_scan;
    u32 i, w;
    (void)s;

    for (i = 0; i < nsd.syn_buckets && freed < target; i++) {
        struct nsd_syn_bucket *b = &nsd.syn[i];
        int evict_way = -1;
        u16 min_weight = U16_MAX;
        spin_lock(&b->lock);
        for (w = 0; w < NSD_SYN_WAYS; w++) {
            int d;
            for (d = 0; d < b->ways[w].dst_count; d++) {
                if (b->ways[w].weight[d] > 0 && b->ways[w].weight[d] < min_weight) {
                    min_weight = b->ways[w].weight[d];
                    evict_way = w;
                }
            }
        }
        if (evict_way >= 0) {
            int d;
            for (d = 0; d < b->ways[evict_way].dst_count; d++)
                b->ways[evict_way].weight[d] = 0;
            freed++;
            if (atomic64_read(&nsd.st_syn_active) > 0)
                atomic64_dec(&nsd.st_syn_active);
            atomic64_inc(&nsd.st_syn_evict);
        }
        spin_unlock(&b->lock);
        if ((i & 0xFF) == 0xFF) cond_resched();
    }
    return freed;
}

static __maybe_unused void nsd_telem_fn(struct work_struct *work)
{
    u64 c   = atomic64_read(&nsd.st_correct);
    u64 pf  = atomic64_read(&nsd.st_prefetch);
    u64 hit = pf ? (c * 100 / pf) : 0;
    u16 hist_acc, rec_acc, lr;
    (void)work;

    hist_acc = 0; rec_acc = 0; lr = 0;
    {
        int _fi;
        u32 _cnt = 0;
        spin_lock(&nsd.fctx.lock);
        for (_fi = 0; _fi < NSD_FCTX_SLOTS; _fi++) {
            if (nsd.fctx.e[_fi].valid) {
                hist_acc += nsd.fctx.e[_fi].ada.historical_acc;
                rec_acc += nsd.fctx.e[_fi].ada.recent_acc;
                lr += nsd.fctx.e[_fi].ada.learning_rate;
                _cnt++;
            }
        }
        spin_unlock(&nsd.fctx.lock);
        if (_cnt) { hist_acc /= _cnt; rec_acc /= _cnt; lr /= _cnt; }
    }

    pr_info("[NSD] ev=%llu pf=%llu hit=%llu%% syn=%llu drop=%llu "
            "bypass=%llu strat_sw=%llu thresh=%u "
            "hist_acc=%u/1000 rec_acc=%u/1000 lr=%u/1000\n",
            (unsigned long long)atomic64_read(&nsd.st_events),
            (unsigned long long)pf,
            (unsigned long long)hit,
            (unsigned long long)atomic64_read(&nsd.st_syn_active),
            (unsigned long long)atomic64_read(&nsd.st_dropped),
            (unsigned long long)atomic64_read(&nsd.st_seq_bypass),
            (unsigned long long)atomic64_read(&nsd.st_strat_switch),
            READ_ONCE(nsd.thresh),
            (unsigned)hist_acc, (unsigned)rec_acc, (unsigned)lr);

    if (atomic_read(&nsd.running))
        queue_delayed_work(nsd.wq, &nsd.telem_work,
                           msecs_to_jiffies(NSD_TELEM_MS));
}

static __maybe_unused void nsd_waste_track_fn(struct work_struct *work)
{
    u64 now = nsd_ns();
    u32 i;
    u64 expired = 0;
    u64 expire_ns = 5ULL * NSEC_PER_SEC;

    if (!atomic_read(&nsd.feat_waste_track))
        goto resched;

    for (i = 0; i < NSD_PENDING_SLOTS; i++) {
        u64 h = READ_ONCE(nsd.pending[i]);
        if (h && (now - READ_ONCE(nsd.pending_ts[i])) > expire_ns) {
            expired++;
            {
                    u32 kind = READ_ONCE(nsd.pending_kind[i]);
                    u32 bw   = READ_ONCE(nsd.pending_way[i]);
                    u32 dst  = READ_ONCE(nsd.pending_dst[i]);
                    if (kind == NSD_PEND_SYNAPTIC && bw) {
                        if (param_penalty) nsd_syn_weaken(bw, dst);
                    } else if (kind == NSD_PEND_STRIDE) {
                        u32 fid = bw;
                        struct nsd_fctx *ctx = NULL;
                        int k;
                        for (k = 0; k < NSD_FCTX_SLOTS; k++) {
                            if (nsd.fctx.e[k].valid && nsd.fctx.e[k].file_id == fid) {
                                ctx = &nsd.fctx.e[k];
                                break;
                            }
                        }
                        if (ctx) ctx->stride_count = 0;
                    }
            }
            WRITE_ONCE(nsd.pending[i], 0ULL);            WRITE_ONCE(nsd.pending_ts[i], 0ULL);
            WRITE_ONCE(nsd.pending_way[i], 0);            WRITE_ONCE(nsd.pending_dst[i], 0);            WRITE_ONCE(nsd.pending_kind[i], 0);
        }
    }
    if (expired)
        atomic64_add(expired, &nsd.st_waste_expired);

resched:
    if (atomic_read(&nsd.running))
        queue_delayed_work(nsd.wq, &nsd.waste_track_work,
                           msecs_to_jiffies(NSD_TELEM_MS));
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
# define NSD_BDEV_OPEN(dev) \
    ((void *)bdev_file_open_by_dev((dev), BLK_OPEN_READ, NULL, NULL))
# define NSD_BDEV_ERR(ref)   IS_ERR((struct file *)(ref))
# define NSD_BDEV_BDEV(ref)  file_bdev((struct file *)(ref))
# define NSD_BDEV_CLOSE(ref) bdev_fput((struct file *)(ref))
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
# define NSD_BDEV_OPEN(dev) \
    ((void *)bdev_open_by_dev((dev), BLK_OPEN_READ, NULL, NULL))
# define NSD_BDEV_ERR(ref)   IS_ERR((struct bdev_handle *)(ref))
# define NSD_BDEV_BDEV(ref)  ((struct bdev_handle *)(ref))->bdev
# define NSD_BDEV_CLOSE(ref) bdev_release((struct bdev_handle *)(ref))
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
# define NSD_BDEV_OPEN(dev) \
    ((void *)blkdev_get_by_dev((dev), BLK_OPEN_READ, NULL, NULL))
# define NSD_BDEV_ERR(ref)   IS_ERR((struct block_device *)(ref))
# define NSD_BDEV_BDEV(ref)  ((struct block_device *)(ref))
# define NSD_BDEV_CLOSE(ref) blkdev_put((struct block_device *)(ref), BLK_OPEN_READ)
#else
# define NSD_BDEV_OPEN(dev) \
    ((void *)blkdev_get_by_dev((dev), FMODE_READ, NULL))
# define NSD_BDEV_ERR(ref)   IS_ERR((struct block_device *)(ref))
# define NSD_BDEV_BDEV(ref)  ((struct block_device *)(ref))
# define NSD_BDEV_CLOSE(ref) blkdev_put((struct block_device *)(ref), FMODE_READ)
#endif

static __maybe_unused void nsd_auto_tune(void)
{

    static const dev_t cands[] = {
        MKDEV(259, 0), MKDEV(8, 0), MKDEV(8, 16), MKDEV(8, 32), MKDEV(253, 0),
    };
    bool has_ssd = false, has_nvme = false, has_hdd = false;
    u64  pages = totalram_pages();
    u32  ram   = (u32)((pages << PAGE_SHIFT) >> 20);
    int  i;

    for (i = 0; i < (int)ARRAY_SIZE(cands); i++) {
        void *ref = (void *)NSD_BDEV_OPEN(cands[i]);
        if (NSD_BDEV_ERR(ref)) continue;

        struct block_device *bdev = NSD_BDEV_BDEV(ref);


        bool is_rotational = !bdev_nonrot(bdev);
        bool is_nvme = (MAJOR(cands[i]) == 259);


        bool looks_like_hdd = false;
        if (bdev->bd_disk && bdev->bd_disk->disk_name[0]) {
            const char *model = bdev->bd_disk->disk_name;

            if (strstr(model, "HDD") || strstr(model, "HARDDISK") ||
                strstr(model, "HD ") || strstr(model, "HDD") ||
                strstr(model, "WD") || strstr(model, "ST") ||
                strstr(model, "ST") || strstr(model, "HGST") ||
                strstr(model, "HITACHI") || strstr(model, "TOSHIBA") ||
                strstr(model, "SEAGATE") || strstr(model, "SAMSUNG")) {
                looks_like_hdd = true;
            }
        }

        if (is_nvme) {
            has_nvme = true;
        } else if (is_rotational || looks_like_hdd) {
            has_hdd = true;
        } else {
            has_ssd = true;
        }

        NSD_BDEV_CLOSE(ref);
    }


    if (has_nvme) {
        nsd.dev_class     = NSD_DEV_NVME;
        nsd.region_shift  = NSD_REGION_SHIFT_NVME;
        nsd.depth         = NSD_DEPTH_NVME;
        nsd.thresh        = NSD_THRESH_NVME;
        nsd.prefetch_span = NSD_PREFETCH_SPAN_NVME;
    } else if (has_ssd) {
        nsd.dev_class     = NSD_DEV_SSD;
        nsd.region_shift  = NSD_REGION_SHIFT_SSD;
        nsd.depth         = NSD_DEPTH_SSD;
        nsd.thresh        = NSD_THRESH_SSD;
        nsd.prefetch_span = NSD_PREFETCH_SPAN_SSD;
    } else if (has_hdd) {
        nsd.dev_class     = NSD_DEV_HDD;
        nsd.region_shift  = NSD_REGION_SHIFT_HDD;
        nsd.depth         = NSD_DEPTH_HDD;
        nsd.thresh        = NSD_THRESH_HDD;
        nsd.prefetch_span = NSD_PREFETCH_SPAN_HDD;
    } else {
        nsd.dev_class     = NSD_DEV_HDD;
        nsd.region_shift  = NSD_REGION_SHIFT_HDD;
        nsd.depth         = NSD_DEPTH_HDD;
        nsd.thresh        = NSD_THRESH_HDD;
        nsd.prefetch_span = NSD_PREFETCH_SPAN_HDD;
    }

    pr_info("NSD auto-tune: RAM=%uMB disk=%s depth=%u thresh=%u region=%lluKB span=%lluKB\n",
            ram,
            nsd_dev_names[nsd.dev_class],
            nsd.depth,
            nsd.thresh,
            (unsigned long long)((1UL << nsd.region_shift) >> 10),
            (unsigned long long)(nsd.prefetch_span >> 10));
}

static __maybe_unused ssize_t stats_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
    ssize_t len;
    u64  p  = atomic64_read(&nsd.st_predictions);
    u64  c  = atomic64_read(&nsd.st_correct);
    u64  pf = atomic64_read(&nsd.st_prefetch);

    u64  skip = atomic64_read(&nsd.st_pending_skip);
    u64  pnew = (skip < p) ? (p - skip) : 0;
    u64  hit = (p  > 0) ? (c * 100 / p)  : 0;
    u64  hit_real = (pnew > 0) ? (c * 100 / pnew) : 0;
    u64  amp = (pf > 0) ? (c * 100 / pf) : 0;
    bool mok;
    u16  hist_acc, rec_acc, lr;
    (void)k; (void)a;

    mok = true;
    hist_acc = 0; rec_acc = 0; lr = 0;
    {
        int _fi;
        u32 _cnt = 0;
        spin_lock(&nsd.fctx.lock);
        for (_fi = 0; _fi < NSD_FCTX_SLOTS; _fi++) {
            if (nsd.fctx.e[_fi].valid) {
                hist_acc += nsd.fctx.e[_fi].ada.historical_acc;
                rec_acc += nsd.fctx.e[_fi].ada.recent_acc;
                lr += nsd.fctx.e[_fi].ada.learning_rate;
                if (!nsd.fctx.e[_fi].mon.prefetch_ok)
                    mok = false;
                _cnt++;
            }
        }
        spin_unlock(&nsd.fctx.lock);
        if (_cnt) { hist_acc /= _cnt; rec_acc /= _cnt; lr /= _cnt; }
    }

    len = sprintf(buf,
        "version:%s\n"
        "running:%d\nhook:%d\nobserve_only:%d\n"
        "features:stride=%d procaware=%d\n"
        "dev_class:%s\n"
        "depth:%u\nthresh:%u\nregion_kb:%llu\nspan_kb:%llu\n"
        "events:%llu\nkprobe:%llu\nlearned:%llu\npredictions:%llu\n"
        "prefetch_sent:%llu\ncorrect:%llu\nhit_rate:%llu%%\nhit_rate_real:%llu%%\nprefetch_amp:%llu%%\n"
        "synapses:%llu\nevicted:%llu\ndropped:%llu\n"
        "seq_bypass:%llu\nstrat_switch:%llu\nautothresh_adj:%llu\n"
        "stride_predictions:%llu\nstride_chain:%llu\n"
        "hist_acc:%u\nrec_acc:%u\nlearning_rate:%u\n"
        "decay_runs:%llu\ndisabled:%llu\ncoalesced:%llu\nmonitor_ok:%d\n"
        "pending_overwrite:%llu\n""pending_skip:%llu\n""waste_expired:%llu\n",
        NSD_VERSION,
        atomic_read(&nsd.running),
        atomic_read(&nsd.hook_reg),
        atomic_read(&nsd.observe_only),
        atomic_read(&nsd.feat_stride),
        atomic_read(&nsd.feat_procaware),
        nsd_dev_names[nsd.dev_class],
        READ_ONCE(nsd.depth),
        READ_ONCE(nsd.thresh),
        (unsigned long long)((1UL << READ_ONCE(nsd.region_shift)) >> 10),
        (unsigned long long)(READ_ONCE(nsd.prefetch_span) >> 10),
        (unsigned long long)atomic64_read(&nsd.st_events),
        (unsigned long long)atomic64_read(&nsd.st_kprobe),
        (unsigned long long)atomic64_read(&nsd.st_learned),
        (unsigned long long)p,
        (unsigned long long)pf,
        (unsigned long long)c,
        (unsigned long long)hit,
        (unsigned long long)hit_real,
        (unsigned long long)amp,
        (unsigned long long)atomic64_read(&nsd.st_syn_active),
        (unsigned long long)atomic64_read(&nsd.st_syn_evict),
        (unsigned long long)atomic64_read(&nsd.st_dropped),
        (unsigned long long)atomic64_read(&nsd.st_seq_bypass),
        (unsigned long long)atomic64_read(&nsd.st_strat_switch),
        (unsigned long long)atomic64_read(&nsd.st_autothresh_adj),
        (unsigned long long)atomic64_read(&nsd.st_stride_predictions),
        (unsigned long long)atomic64_read(&nsd.st_stride_chain),
        (unsigned)hist_acc, (unsigned)rec_acc, (unsigned)lr,
        (unsigned long long)atomic64_read(&nsd.st_decay),
        (unsigned long long)atomic64_read(&nsd.st_disabled),
        (unsigned long long)atomic64_read(&nsd.st_coalesced),
        (unsigned)mok,
        (unsigned long long)atomic64_read(&nsd.st_pending_overwrite),
        (unsigned long long)atomic64_read(&nsd.st_pending_skip),
        (unsigned long long)atomic64_read(&nsd.st_waste_expired));

    return len;
}

static __maybe_unused ssize_t fctx_debug_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
    ssize_t len = 0;
    int i;
    (void)k; (void)a;

    spin_lock(&nsd.fctx.lock);
    for (i = 0; i < NSD_FCTX_SLOTS; i++) {
        struct nsd_fctx *fc = &nsd.fctx.e[i];
        if (!fc->valid) continue;
        len += sprintf(buf + len,
            "fctx[%d]: file_id=%u read_count=%llu pred=%u cor=%u depth=%u thresh=%u span=%llu dis=%d hist=%u rec=%u lr=%u\n",
            i, fc->file_id,
            (unsigned long long)fc->read_count,
            fc->pf_count, fc->hit_count,
            fc->depth, fc->thresh,
            (unsigned long long)fc->prefetch_span,
            fc->disabled,
            fc->ada.historical_acc, fc->ada.recent_acc, fc->ada.learning_rate);
    }
    spin_unlock(&nsd.fctx.lock);
    return len;
}

static __maybe_unused ssize_t hook_enable_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
    (void)k; (void)a;
    return sprintf(buf, "%d\n", atomic_read(&nsd.hook_on));
}
static __maybe_unused ssize_t hook_enable_store(struct kobject *k, struct kobj_attribute *a,
                                  const char *buf, size_t n)
{
    unsigned v;
    (void)k; (void)a;
    if (kstrtouint(buf, 10, &v)) return -EINVAL;
    atomic_set(&nsd.hook_on, v ? 1 : 0);
    if (v) nsd_hook_register();
    else   nsd_hook_unregister();
    return n;
}

static __maybe_unused ssize_t observe_only_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
    (void)k; (void)a;
    return sprintf(buf, "%d\n", atomic_read(&nsd.observe_only));
}
static __maybe_unused ssize_t observe_only_store(struct kobject *k, struct kobj_attribute *a,
                                   const char *buf, size_t n)
{
    unsigned v;
    (void)k; (void)a;
    if (kstrtouint(buf, 10, &v)) return -EINVAL;
    atomic_set(&nsd.observe_only, v ? 1 : 0);
    return n;
}

static __maybe_unused ssize_t mode_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
    enum nsd_mode m = READ_ONCE(nsd_mode);
    const char *s;
    (void)k; (void)a;
    switch (m) {
    case NSD_MODE_NORMAL:    s = "normal";    break;
    case NSD_MODE_INCOGNITO: s = "incognito"; break;
    case NSD_MODE_AIR_GAP:   s = "air_gap";   break;
    default:                 s = "normal";    break;
    }
    return sprintf(buf, "%s\n", s);
}

static __maybe_unused ssize_t mode_store(struct kobject *k, struct kobj_attribute *a,
                          const char *buf, size_t n)
{
    enum nsd_mode new_mode;
    size_t orig_n = n;
    (void)k; (void)a;


    while (n && (buf[n-1] == '\n' || buf[n-1] == '\r' ||
                 buf[n-1] == ' '  || buf[n-1] == '\t'))
        n--;

    if (n == 6 && !strncmp(buf, "normal", 6)) {
        new_mode = NSD_MODE_NORMAL;
    } else if (n == 9 && !strncmp(buf, "incognito", 9)) {
        new_mode = NSD_MODE_INCOGNITO;
    } else if (n == 7 && !strncmp(buf, "air_gap", 7)) {
        new_mode = NSD_MODE_AIR_GAP;
    } else {
        return -EINVAL;
    }

    switch (new_mode) {
    case NSD_MODE_NORMAL:
        atomic_set(&nsd.hook_on, 1);
        atomic_set(&nsd.observe_only, 0);
        nsd_hook_register();
        break;
    case NSD_MODE_INCOGNITO:
        atomic_set(&nsd.hook_on, 1);
        atomic_set(&nsd.observe_only, 1);
        nsd_hook_register();
        break;
    case NSD_MODE_AIR_GAP:
        atomic_set(&nsd.observe_only, 1);
        atomic_set(&nsd.hook_on, 0);
        nsd_hook_unregister();
        break;
    }

    WRITE_ONCE(nsd_mode, new_mode);
    return orig_n;
}

static __maybe_unused ssize_t depth_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
    (void)k; (void)a;
    return sprintf(buf, "%u\n", READ_ONCE(nsd.depth));
}
static __maybe_unused ssize_t depth_store(struct kobject *k, struct kobj_attribute *a,
                            const char *buf, size_t n)
{
    unsigned v;
    (void)k; (void)a;
    if (kstrtouint(buf, 10, &v) || v > 4096) return -EINVAL;
    WRITE_ONCE(nsd.depth, v);
    return n;
}

static __maybe_unused ssize_t thresh_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
    (void)k; (void)a;
    return sprintf(buf, "%u\n", READ_ONCE(nsd.thresh));
}
static __maybe_unused ssize_t thresh_store(struct kobject *k, struct kobj_attribute *a,
                             const char *buf, size_t n)
{
    unsigned v;
    (void)k; (void)a;
    if (kstrtouint(buf, 10, &v) || v > NSD_W_MAX) return -EINVAL;
    WRITE_ONCE(nsd.thresh, (u16)v);
    return n;
}

static __maybe_unused ssize_t dev_class_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
    (void)k; (void)a;
    return sprintf(buf, "%s\n", nsd_dev_names[READ_ONCE(nsd.dev_class)]);
}

static __maybe_unused ssize_t dev_class_store(struct kobject *k, struct kobj_attribute *a,
                                const char *buf, size_t n)
{
    enum nsd_dev_class new_class;
    size_t orig_n = n;
    (void)k; (void)a;

    while (n && (buf[n-1] == '\n' || buf[n-1] == '\r' ||
                 buf[n-1] == ' '  || buf[n-1] == '\t'))
        n--;

    if (n == 3 && !strncmp(buf, "ssd", 3))
        new_class = NSD_DEV_SSD;
    else if (n == 3 && !strncmp(buf, "hdd", 3))
        new_class = NSD_DEV_HDD;
    else if (n == 4 && !strncmp(buf, "nvme", 4))
        new_class = NSD_DEV_NVME;
    else if (n == 4 && !strncmp(buf, "auto", 4)) {
        nsd_auto_tune();
        return orig_n;
    } else {
        return -EINVAL;
    }

    WRITE_ONCE(nsd.dev_class, new_class);


    switch (new_class) {
    case NSD_DEV_HDD:
        WRITE_ONCE(nsd.region_shift, NSD_REGION_SHIFT_HDD);
        WRITE_ONCE(nsd.depth, NSD_DEPTH_HDD);
        WRITE_ONCE(nsd.thresh, NSD_THRESH_HDD);
        WRITE_ONCE(nsd.prefetch_span, NSD_PREFETCH_SPAN_HDD);
        break;
    case NSD_DEV_SSD:
        WRITE_ONCE(nsd.region_shift, NSD_REGION_SHIFT_SSD);
        WRITE_ONCE(nsd.depth, NSD_DEPTH_SSD);
        WRITE_ONCE(nsd.thresh, NSD_THRESH_SSD);
        WRITE_ONCE(nsd.prefetch_span, NSD_PREFETCH_SPAN_SSD);
        break;
    case NSD_DEV_NVME:
        WRITE_ONCE(nsd.region_shift, NSD_REGION_SHIFT_NVME);
        WRITE_ONCE(nsd.depth, NSD_DEPTH_NVME);
        WRITE_ONCE(nsd.thresh, NSD_THRESH_NVME);
        WRITE_ONCE(nsd.prefetch_span, NSD_PREFETCH_SPAN_NVME);
        break;
    default:
        break;
    }

    return orig_n;
}

static __maybe_unused ssize_t features_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
    (void)k; (void)a;
    return sprintf(buf, "stride=%d autothresh=%d fine_decay=%d procaware=%d waste_track=%d\n",
                   atomic_read(&nsd.feat_stride),
                   atomic_read(&nsd.feat_autothresh),
                   atomic_read(&nsd.feat_fine_decay),
                   atomic_read(&nsd.feat_procaware),
                   atomic_read(&nsd.feat_waste_track));
}

static __maybe_unused ssize_t features_store(struct kobject *k, struct kobj_attribute *a,
                               const char *buf, size_t n)
{
    unsigned v;
    (void)k; (void)a;

    if (sscanf(buf, "stride=%u", &v) == 1) {
        atomic_set(&nsd.feat_stride, v ? 1 : 0);
        return n;
    }
    if (sscanf(buf, "autothresh=%u", &v) == 1) {
        atomic_set(&nsd.feat_autothresh, v ? 1 : 0);
        return n;
    }
    if (sscanf(buf, "fine_decay=%u", &v) == 1) {
        atomic_set(&nsd.feat_fine_decay, v ? 1 : 0);
        return n;
    }
    if (sscanf(buf, "procaware=%u", &v) == 1) {
        atomic_set(&nsd.feat_procaware, v ? 1 : 0);
        return n;
    }
    if (sscanf(buf, "waste_track=%u", &v) == 1) {
        atomic_set(&nsd.feat_waste_track, v ? 1 : 0);
        return n;
    }
    if (sscanf(buf, "all=%u", &v) == 1) {
        atomic_set(&nsd.feat_stride, v ? 1 : 0);
        atomic_set(&nsd.feat_autothresh, v ? 1 : 0);
        atomic_set(&nsd.feat_fine_decay, v ? 1 : 0);
        atomic_set(&nsd.feat_procaware, v ? 1 : 0);
        atomic_set(&nsd.feat_waste_track, v ? 1 : 0);
        return n;
    }
    return -EINVAL;
}

static struct kobj_attribute a_stats      = __ATTR_RO(stats);
static struct kobj_attribute a_hook       = __ATTR_RW(hook_enable);
static struct kobj_attribute a_obs        = __ATTR_RW(observe_only);
static struct kobj_attribute a_mode       = __ATTR(mode,   0644, mode_show,   mode_store);
static struct kobj_attribute a_depth      = __ATTR(depth,  0644, depth_show,  depth_store);
static struct kobj_attribute a_thresh     = __ATTR(thresh, 0644, thresh_show, thresh_store);
static struct kobj_attribute a_feat       = __ATTR(features, 0644, features_show, features_store);
static struct kobj_attribute a_fctx_debug = __ATTR_RO(fctx_debug);
static struct kobj_attribute a_dev_class  = __ATTR(dev_class, 0644, dev_class_show, dev_class_store);

static struct attribute *nsd_attrs[] = {
    &a_stats.attr, &a_hook.attr, &a_obs.attr, &a_mode.attr,
    &a_depth.attr, &a_thresh.attr, &a_feat.attr, &a_fctx_debug.attr,
    &a_dev_class.attr,
    NULL,
};
static struct attribute_group nsd_ag = { .attrs = nsd_attrs };

static int __init nsd_init(void)
{
    int ret;

    pr_info("NSD v%s - Neural Storage Driver\n", NSD_VERSION);
    pr_info("NSD: VFS-kprobe + Adaptive Learning + Workload Detection\n");

    get_random_bytes(&nsd.hkey, sizeof(siphash_key_t));

    {
        struct sysinfo si;
        si_meminfo(&si);
        if ((si.totalram << PAGE_SHIFT) < (2UL << 30))
            nsd.syn_bits = 16;
        else if ((si.totalram << PAGE_SHIFT) < (4UL << 30))
            nsd.syn_bits = 17;
        else
            nsd.syn_bits = 18;
        nsd.syn_buckets = 1U << nsd.syn_bits;
        nsd.syn_mask    = nsd.syn_buckets - 1U;
    }

    ret = nsd_syn_init();
    if (ret) {
        pr_err("NSD: synapse table init failed: %d\n", ret);
        return ret;
    }

    nsd.wq = alloc_workqueue("nsd_wq", WQ_UNBOUND | WQ_MEM_RECLAIM, 0);
    if (!nsd.wq) {
        nsd_syn_free();
        return -ENOMEM;
    }

    atomic_set(&nsd.running, 1);
    if (param_waste_track)
        atomic_set(&nsd.feat_waste_track, 1);
    if (param_observe_only)
        atomic_set(&nsd.observe_only, 1);



    atomic64_set(&nsd.st_kprobe, 0);
    memset(nsd.skip_kprobe, 0, sizeof(nsd.skip_kprobe));
    memset(nsd.skip_ino, 0, sizeof(nsd.skip_ino));
    memset(nsd.skip_time, 0, sizeof(nsd.skip_time));

    nsd.worker = kthread_create(nsd_worker, NULL, "nsd_brain");
    if (IS_ERR(nsd.worker)) {
        ret = PTR_ERR(nsd.worker);
        nsd.worker = NULL;
        atomic_set(&nsd.running, 0);
        destroy_workqueue(nsd.wq);
        nsd_syn_free();
        return ret;
    }
    wake_up_process(nsd.worker);


#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 7, 0)
    nsd.shrinker_ptr = shrinker_alloc(0, "nsd");
    if (nsd.shrinker_ptr) {
        nsd.shrinker_ptr->count_objects = nsd_shrink_count;
        nsd.shrinker_ptr->scan_objects  = nsd_shrink_scan;
        shrinker_register(nsd.shrinker_ptr);
    }
#else
    nsd.shrinker_ptr               = &nsd.shrinker;
    nsd.shrinker_ptr->count_objects = nsd_shrink_count;
    nsd.shrinker_ptr->scan_objects  = nsd_shrink_scan;
    nsd.shrinker_ptr->seeks         = DEFAULT_SEEKS;
# if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
    register_shrinker(nsd.shrinker_ptr, "nsd");
# else
    register_shrinker(nsd.shrinker_ptr);
# endif
#endif

    INIT_DELAYED_WORK(&nsd.decay_work, nsd_syn_decay);
    INIT_DELAYED_WORK(&nsd.telem_work, nsd_telem_fn);
    INIT_DELAYED_WORK(&nsd.waste_track_work, nsd_waste_track_fn);
    queue_delayed_work(nsd.wq, &nsd.decay_work,
                       msecs_to_jiffies(NSD_W_DECAY_MS));
    queue_delayed_work(nsd.wq, &nsd.telem_work,
                       msecs_to_jiffies(NSD_TELEM_MS));
    queue_delayed_work(nsd.wq, &nsd.waste_track_work,
                       msecs_to_jiffies(NSD_TELEM_MS));

    nsd_kobj = kobject_create_and_add("nsd", kernel_kobj);
    if (nsd_kobj && sysfs_create_group(nsd_kobj, &nsd_ag))
        pr_warn("NSD: sysfs group creation failed\n");


    nsd_auto_tune();


    atomic_set(&nsd.hook_on, 1);
    nsd_hook_register();

    pr_info("NSD v%s ready - sysfs: /sys/kernel/nsd/\n", NSD_VERSION);
    pr_info("NSD: observe_only=%d (0=prefetch enabled)\n",
            atomic_read(&nsd.observe_only));
    return 0;
}

static void __exit nsd_exit(void)
{
    atomic_set(&nsd.running, 0);
    nsd_hook_unregister();

    if (nsd_kobj) {
        sysfs_remove_group(nsd_kobj, &nsd_ag);
        kobject_put(nsd_kobj);
    }

    cancel_delayed_work_sync(&nsd.decay_work);
    cancel_delayed_work_sync(&nsd.telem_work);
    cancel_delayed_work_sync(&nsd.waste_track_work);

    if (nsd.worker) kthread_stop(nsd.worker);
    if (nsd.wq) {
        flush_workqueue(nsd.wq);
        destroy_workqueue(nsd.wq);
    }

    nsd_drain_rings();

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 7, 0)
    shrinker_free(nsd.shrinker_ptr);
#else
    unregister_shrinker(nsd.shrinker_ptr);
#endif

    nsd_syn_free();
    pr_info("NSD v%s unloaded\n", NSD_VERSION);
}

module_init(nsd_init);
module_exit(nsd_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ayhan Aydin <nsd.project.dev@gmail.com>");
MODULE_DESCRIPTION("NSD v1.0.0 - Neural Storage Driver (development iterations v0.0.1)");
MODULE_VERSION(NSD_VERSION);
