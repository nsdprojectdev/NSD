# NSD v1 Benchmark Results

Date: 2026-07-14
Kernel: 6.17.0-35-generic
Module: nsd.ko (v10.0)

## Test Environment

| Component | Detail |
|-----------|--------|
| SSD | sda1, 8GB test file (/home/nsd/nsd_test_ssd.dat) |
| HDD | sdb5 (ntfs-3g), 4GB test file (/mnt/hdd/nsd_hdd_test.dat) |
| CPU | x86_64 |
| fio | v3.36 |
| RAM | System RAM |

---

## SSD Results

### Sequential 64K, direct=0 (buffered I/O)

| Run | NSD OFF | NSD ON | Gain |
|-----|---------|--------|------|
| #1  | 390 MB/s | 470 MB/s | +20.5% |
| #2-3 | 388-392 | 468-472 | +20-21% |

NSD Stats (ON):
- `rec_acc` = 85.9%
- `hit_rate` = 75-85%
- `prefetch_amp` = 90%+

---

## HDD Results

### Sequential 64K, direct=0 (buffered I/O)

| Run | NSD OFF | NSD ON | Gain |
|-----|---------|--------|------|
| #1  | 149.8 MB/s | 189.6 MB/s | +26.6%* |
| #2  | 198.5 MB/s | 191.5 MB/s | −3.5% |
| #3  | 201.7 MB/s | 192.4 MB/s | −4.6% |

*Run #1 OFF was cold-cache outlier. Actual baseline: ~200 MB/s.
**Verdict: ~−4% (neutral, earlier −70.7% regression fixed)**

### Sequential 64K, direct=1 (direct I/O)

| Run | NSD OFF | NSD ON | Gain |
|-----|---------|--------|------|
| #1  | 103 MB/s | 103 MB/s | ~0% |

### Random 4K, direct=0/1

| Run | NSD OFF | NSD ON | Gain |
|-----|---------|--------|------|
| direct=0 | 106 IOPS | 104 IOPS | ~0% |
| direct=1 | 106 IOPS | 104 IOPS | ~0% |

### Repeated Random 4K, direct=1 (randrepeat=1)

| Run | NSD OFF | NSD ON | Gain |
|-----|---------|--------|------|
| #1  | 106 IOPS | 104 IOPS | ~0% |

### Mixed 80/20 (80% seq + 20% rand), direct=1

| Run | NSD OFF | NSD ON | Gain |
|-----|---------|--------|------|
| #1  | 742 MB/s | 826 MB/s | +11.3%* |

*Disk cache likely active; results may not reflect pure disk I/O.

---

## HDD Regression Fix History

| Version | Config | HDD Seq (direct=0) | Regression |
|---------|--------|--------------------|------------|
| v6.0 (block-level) | min_conf=550 | ~100 MB/s | ~0% |
| v10.0 original | span=512KB, thresh=200, shift=14 | ~30 MB/s | −70.7% |
| v10.0 fixed | span=4KB, thresh=700, shift=16, stride fix | 192 MB/s | −4% |

## Key Fixes Applied

1. `REGION_SHIFT_HDD 14→16` — 16KB→64KB regions (4× less noise)
2. `THRESH_HDD 200→700` — only strong predictions pass
3. `PREFETCH_SPAN_HDD 512KB→4KB` — minimal bandwidth theft per wrong prediction
4. `DEPTH_HDD 4→2` — minimal chain depth
5. Stride predictor bypass bug fix — `strat != NSD_STRAT_NONE` check added
6. Auto-tune hardcoded values → defines

## Summary

| Device | Best Result | Notes |
|--------|-------------|-------|
| **SSD** | **+20.5%** (seq 64K, buffered) | Stable, repeatable |
| **HDD** | **−4% to 0%** | Neutral, no regression |

## 2026-08-20 - Window-size matrix (v2.4)

Interleaved ON/OFF on the same machine, drop_caches before every run,
3 runs per configuration. 6.2 GB SQLite DB (Q2 full scan), 8 GB
sequential file (64k requests). Disk I/O byte-identical between ON/OFF
in all 96 runs.

  kb    | seq64 OFF  ON    d%   | sqlite OFF  ON    d%   | p99 seq64 (us)
  128   | 22.02  17.11 -22%   | 24.51  20.48 -16%   | 812 -> 805
  256   | 17.08  17.22 +1%    | 20.05  16.60 -17%   | 824 -> 1129
  512   | 16.81  16.96 +1%    | 15.31  12.18 -20%   | 1092 -> 1903
  1024  | 17.25  17.03 -1%    | 12.22  12.09 -1%    | 1903 -> 1883

random4k: fully neutral at all window sizes. random_repeat: -6% (128k),
-4% (256k), p95 296 -> 243 us. At >= 1M kernel window the module
silences itself entirely (verified by counters).
