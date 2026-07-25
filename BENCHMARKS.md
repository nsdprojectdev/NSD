

# Benchmarks

## SQLite Full Table Scan (4GB)

**Test setup:**
- Database: 13M rows, 5.9 GB (4KB pages, no journal, synchronous OFF)
- Queries: `SELECT AVG(a), SUM(c) FROM t` and `SELECT COUNT(*), AVG(b), MAX(a), MIN(c) FROM t`
- Cold cache: `sync; echo 3 > /proc/sys/vm/drop_caches` before each query (18 times per run)
- SSD: SATA AHCI, ~400 MB/s sequential, ~6K random IOPS
- RAM: 7.6 GB, CPU: i5-3230M 2.6 GHz

### Interleaved Test (Gold Standard) — 2026-07-25 (Reproduced)

OFF/1 -> ON/1 -> OFF/2 -> ON/2 -> OFF/3 -> ON/3. NSD toggled via `observe_only` sysfs parameter (no module reload).

| Pass | OFF (s) | ON (s) | Gain |
|------|---------|--------|------|
| 1    | 54.96   | 44.70  | **-18.7%** |
| 2    | 53.81   | 43.56  | **-19.0%** |
| **Avg** | **54.39** | **44.13** | **-18.8%** |

### Interleaved Test (Gold Standard) — Original (2026-07-19)

OFF/1 -> ON/1 -> OFF/2 -> ON/2 -> OFF/3 -> ON/3, module fully unloaded/reloaded between each pair.

| Pass | OFF (s) | ON (s) | Gain |
|------|---------|--------|------|
| 1    | 53.04   | 43.28  | **-18.4%** |
| 2    | 52.55   | 43.21  | **-17.8%** |
| 3    | 52.17   | 42.87  | **-17.8%** |
| **Avg** | **52.58** | **43.12** | **-18.0%** |

### Blocked Test

OFF/1-3 -> ON/1-3 (module loaded once for all ON passes).

| Pass | OFF (s) | ON (s) | Gain |
|------|---------|--------|------|
| 1    | 53.56   | 43.45  | **-18.9%** |
| 2    | 53.00   | 43.60  | **-17.7%** |
| 3    | 53.61   | 43.00  | **-19.8%** |
| **Avg** | **53.39** | **43.35** | **-18.8%** |

### NSD Stats (2026-07-25 interleaved run)

| Metric | Value |
|--------|-------|
| Events processed | 30.6M |
| Learned patterns | 5.1M |
| Prefetch sent | 1.65M |
| Correct predictions | 6.59M |
| hit_rate | 35% |
| hit_rate_real | **98%** |
| Stride predictions | 31.4M |

### Why it works

Full table scans generate a perfectly sequential page access pattern
(stride = 4KB). NSD detects this stride in the first few pages and
prefetches ahead using `vfs_fadvise(WILLNEED)`. The 99% hit_rate_real
confirms almost all prefetched pages are consumed by the application.

## Sequential 64K (fio) — Original

**Test setup:**
- fio: `--rw=read --bs=64K --size=256M --direct=0`
- SSD: SATA AHCI
- 15 runs each OFF/ON

| Metric | OFF | ON | Gain |
|--------|-----|----|------|
| Avg BW | 384 MB/s | 502 MB/s | **+30.7%** |

Note: This test was performed on a tmpfs (RAM disk), not on a real SSD.
The result may include page-fault pre-warming effects rather than true I/O improvement.

## Sequential 64K (dd, buffered) — 2026-07-25

**Test setup:**
- dd: bs=64K, count=2GB on 8GB test file
- SSD: SATA SSD, cold cache before each run
- 3 passes OFF/ON interleaved

| Pass | OFF (MB/s) | ON (MB/s) | Gain |
|------|-----------|-----------|-------|
| #1   | 386       | 477       | **+23.6%** |
| #2   | 394       | 463       | **+17.5%** |
| #3   | 397       | 503       | **+26.7%** |
| **Avg** | **392** | **481** | **+22.6%** |

## Random 4K (fio psync, buffered) — 2026-07-25

**Test setup:**
- fio: psync, --rw=randread --bs=4K --size=1G --direct=0
- SSD: SATA SSD, cold cache before each run
- 3 passes OFF/ON interleaved

| Pass | OFF (MB/s) | ON (MB/s) | Gain |
|------|-----------|-----------|-------|
| #1   | 27.6      | 27.1      | -1.8% |
| #2   | 26.8      | 27.0      | +0.7% |
| #3   | 26.2      | 27.6      | +5.3% |
| **Avg** | **26.9** | **27.2** | **+1.1%** |

Random 4K buffered reads show no significant benefit from NSD, as expected
(stride predictor cannot predict random access patterns).

## Read Latency (fio seq 64K) — 2026-07-25

| Pass | OFF (us) | ON (us) | Gain |
|------|---------|---------|-------|
| **Avg** | **130** | **131** | **+0.8%** |

Latency is essentially unchanged, confirming NSD adds no measurable overhead.

## HDD Sequential 64K

SATA HDD (ST1000LM024), 64K sequential read: ~53 MB/s saturated.
NSD shows no gain (disk-bound).
