# NSD v1 — Changelog

## 2026-07-19 — waste_track Bug Fix, Metric Analysis & Kprobe Verification

### Kprobe O_DIRECT Verification

Kprobe on `vfs_read` fires for both O_DIRECT and buffered reads. The earlier
"O_DIRECT bypasses the kprobe" note was wrong; the kprobe fires in both cases.
Verified with `dd iflag=direct` (1000x4K → kprobe +1008).

### observe_only Verification

`observe_only=0` → prefetch active, `observe_only=1` → no prefetch at all.
In a 5000 I/O test: observe_only=1 yielded prefetch_sent=0, correct=0.
No behavior other than prefetch is affected.

### prefetch_amp Metric Explanation

prefetch_amp = correct / prefetch_sent × 100. Because numerator and
denominator count different things it can exceed 100%: a single
vfs_fadvise can fetch several pages, and each page read counts as a
separate correct. Not a bug — a definition difference.

### Critical Bug Fix: waste_track timestamp corruption

**File:** `nsd.c`

`nsd_pend_add()` was writing a siphash value into the pending array.
`nsd_waste_track_fn` treated it as a timestamp and compared `(now - hash) > 5s`.
In unsigned arithmetic the hash wrapped when hash >> now, so the entire
pending table was counted as expired. A parallel `pending_ts[]` array was
added storing the real `nsd_ns()` value. The feature is off by default (0),
so the bug was not active. Live test: sequential read + jump → +19
waste_expired, not the whole table.

---

## 2026-07-19 — qd>24 Fix, Parameter Optimization & Strategy Revision

### Critical Bug Fix: queue-depth limiter

**File:** `nsd.c` (lines 1236-1245)

When qd > 24, `depth` was force-set to 1. Since both SSD (sda) and HDD (sdb)
run at qd=32, **prefetch was completely crippled** — `depth=1` prefetched a
single page.

**Fix:** Block removed. `depth` is used directly.

```
- if (qd > 24) depth = 1;
+ /* queue depth limit removed */
```

### Fix: Uninitialized `depth` in `feat_procaware`

**File:** `nsd.c` (lines 1216-1220)

The `feat_procaware` block used `depth = max(depth, 4)` but `depth` had not
been initialized yet. A `depth_boost` variable was added and applied after
the assignment.

### New: skip_kprobe removed

**File:** `nsd.c` (lines 828-835)

When the strategy was `NSD_STRAT_NONE`, the kprobe was skipped entirely
(for 5s). This prevented deterministic patterns like RRP from being learned
by the Markov chain. Now events are processed even under `strat=NONE`;
only prefetch is suppressed.

```
- WRITE_ONCE(nsd.skip_kprobe[ctx->file_id], true);
- ...
+ WRITE_ONCE(nsd.skip_kprobe[ctx->file_id], false);
```

### New: Markov strategy active for SEQ

**File:** `nsd.c` (lines 783-797)

Previously `seq_r > 600` → `NSD_STRAT_NONE` (no prefetch). Now only pure
random (`seq_r < 120 && rpt_r < 100`) gets NONE. Everything else (including
SEQ) prefetches with `NSD_STRAT_MARKOV`.

```
- if (seq_r > NSD_SEQ_RATIO_THRESH) proposed = NSD_STRAT_NONE;
- else if (full) proposed = NSD_STRAT_NONE;
- else proposed = NSD_STRAT_NONE;
+ if (seq_r < 120 && rpt_r < 100) proposed = NSD_STRAT_NONE;
+ ...
+ else proposed = NSD_STRAT_MARKOV;
```

### Changed Parameters

| Parameter | Old | New |
|-----------|-----|-----|
| `NSD_THRESH_SSD` | 100 | 200 |
| `NSD_DEPTH_SSD` | 6 | 3 |
| `NSD_PREFETCH_SPAN_SSD` | 128K | 64K |
| `NSD_THRESH_MIN` | 150 | 200 |
| `kprobe sampling` | 1/4 (`& 3ULL`) | 1/2 (`& 1ULL`) |

### Test Results (3x OFF/ON, SSD)

| Workload | OFF_avg | ON_avg | Δ |
|----------|---------|--------|---|
| SEQ (1M) | 479,713 | **488,511** | **+1.8%** |
| RND (4K) | 16,821 | **18,590** | **+10.5%** |
| RRP1 (4K) | 20,883 | 20,356 | −2.5% |
| RRP2 (4K) | 20,744 | 20,035 | −3.4% |

**Note:** Test noise is high (±5-10%). SEQ and RND are positive; RRP
differences are within noise. pf=33K, hit=96%, ev=1.37M, rec_acc=65%.

### Previously Detected Issues

1. **Before qd>24 fix:** prefetch_sent=135, hit=82% (depth force-set to 1)
2. **After qd>24 fix:** prefetch_sent=2365, hit=72% (depth=6, thresh=100)
3. **After thresh=200/depth=3:** pf=38, hit=81%, rec_acc=67% (too conservative)
4. **MARKOV active for SEQ + skip_kprobe removed:** pf=33K, hit=96%

### Remaining Issues

- HDD test not done
- RRP1 still within noise; a longer test may be needed
- Direct=1 incompatible with O_DIRECT (kprobe depends on `vfs_read`,
  O_DIRECT bypasses it)
- Automatic parameter discovery (autothresh) not yet evaluated

---

## 2026-07-18 — Baseline & Regression Analysis

### Kprobe Sampling

Changed 1/4 → 1/2. Kprobe events rose from ~4271 to ~8123 (2x).

### RND Hysteresis

`CONFIRM_UP_RND=3` added for random workloads (`seq_r<120 && rpt_r<100`).
`CONFIRM_UP=2`, `CONFIRM_DOWN=1`.

### Monitor Window/Easing

1024/300/700 → 128/50/100 (backup values). To increase adaptation speed.

### Parameters Restored to Backup Values

- `THRESH_SSD` = 100
- `DEPTH_SSD` = 6
- `PREFETCH_SPAN_SSD` = 128K
- `SEQ_BYPASS_THRESH_SSD` = 800
- `AUTOTHRESH_STEP` = 10
- `SMALL_IO_THRESH` = 8192
- `PROFILE_WINDOW` = 512
- `skip_kprobe timeout` 10s → 5s

### Fctx Buckets

`NSD_FCTX_BITS=7` (128 slots). Collision 4% vs 43% (previous 6 bits = 64 slots).

### HDD Test

All workloads between ±0.1% and ±1.8%, entirely within noise.
HDD ~78 IOPS (=12.7ms seek) is mechanically saturated. NCQ (iodepth=8)
showed no difference from depth=1.

### Direct=1 Test

NSD's kprobe is on `vfs_read`, not on `__filemap_get_folio`.
O_DIRECT reads do not fire the kprobe. SEQ direct=1 −2.5% was a measurement
error.

## 2026-07-19 — Parameter Tuning (Depth, Threshold & Sampling)

### Changed Parameters (Code)

| Parameter | Old | New | Description |
|-----------|-----|-----|-------------|
|  | 3 | 8 | Optimal for qd=32 |
|  | 200 | 180 | More prefetch |
|  | 64K | 256K | Wider speculation |
|  | 7 (128) | 8 (256) | Fewer collisions |
|  | 4 | 8 | Fewer evictions |
|  | 10 | 25 | Faster adaptation |
|  | 128 | 256 | More stable monitor |
|  | 50 | 30 | Disable later |
|  | 100 | 80 | Enable sooner |
|  | 128 | 256 | Fewer drops |
|  | 1/2 | 1/1 (all) | All events processed |
|  | 950 | 900 | Easier threshold decrease |
|  | 100 | 150 | Earlier threshold increase |

### Calibration (depth=16 aggressive test)

depth=16 was tried but rec_acc dropped to 3% — too much speculative
prefetch. Balanced at depth=8.

### Test Results (ssd_test.sh, direct=1, depth=8/thresh=180)

| Workload | OFF | ON | Delta |
|----------|-----|----|-------|
| Concurrent (64K+4K) | 80.8MB/s | 80.1MB/s | 0.9% |
| Pure random 4K | 17.3MB/s | 17.8MB/s | **+2.8%** |
| Pure seq 64K | 270.3MB/s | 258.7MB/s | 4.3% (direct=1 bypass) |
| RRP pass1 4K | 24.0MB/s | 24.8MB/s | **+3.3%** |
| RRP pass2 4K | 24.8MB/s | 27.2MB/s | **+9.6%** |

### Remaining Issues

- HDD test not done
- direct=1 benchmark bypass; direct=0 gives more meaningful results
- RRP pass2 +9.6% gain is promising; longer test needed
- Autothresh still very active (step=25)

---

## 2026-07-19 — Parameter Tuning (Depth, Threshold and Sampling)

### Changed Parameters (Code)

| Parameter | Old | New | Description |
|-----------|-----|-----|-------------|
| NSD_DEPTH_SSD | 3 | 8 | Optimal for qd=32 |
| NSD_THRESH_SSD | 200 | 180 | More prefetch |
| NSD_PREFETCH_SPAN_SSD | 64K | 256K | Wider speculation |
| NSD_FCTX_BITS | 7 (128) | 8 (256) | Fewer collisions |
| NSD_SYN_WAYS | 4 | 8 | Fewer evictions |
| NSD_AUTOTHRESH_STEP | 10 | 25 | Faster adaptation |
| NSD_MON_WINDOW | 128 | 256 | More stable monitor |
| NSD_MON_DISABLE | 50 | 30 | Disable later |
| NSD_MON_ENABLE | 100 | 80 | Enable sooner |
| NSD_RING_SIZE | 128 | 256 | Fewer drops |
| Kprobe sampling | 1/2 | 1/1 (all) | All events processed |
| NSD_AUTOTHRESH_HIGH_ACC | 950 | 900 | Easier threshold decrease |
| NSD_AUTOTHRESH_LOW_ACC | 100 | 150 | Earlier threshold increase |

### Calibration (depth=16 aggressive test)

depth=16 was tried but rec_acc dropped to 3%. Balanced at depth=8.

### Test Results (ssd_test.sh, direct=1, depth=8 thresh=180)

| Workload | OFF | ON | Delta |
|----------|-----|----|-------|
| Concurrent (64K+4K) | 80.8MB/s | 80.1MB/s | -0.9 |
| Pure random 4K | 17.3MB/s | 17.8MB/s | +2.8 |
| Pure seq 64K | 270.3MB/s | 258.7MB/s | -4.3 (direct=1 bypass) |
| RRP pass1 4K | 24.0MB/s | 24.8MB/s | +3.3 |
| RRP pass2 4K | 24.8MB/s | 27.2MB/s | +9.6 |

### Remaining Issues

- HDD test not done
- direct=1 benchmark bypass; direct=0 gives more meaningful results
- RRP pass2 +9.6 gain is promising; longer test needed
- Autothresh still very active (step=25)
