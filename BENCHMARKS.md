# Benchmarks

## SQLite Full Table Scan (4GB)

**Test setup:**
- Database: 13M rows, 5.9 GB (4KB pages, no journal, synchronous OFF)
- Queries: `SELECT AVG(a), SUM(c) FROM t` and `SELECT COUNT(*), AVG(b), MAX(a), MIN(c) FROM t`
- Cold cache: `sync; echo 3 > /proc/sys/vm/drop_caches` before each query (18 times per run)
- SSD: SATA AHCI, ~400 MB/s sequential, ~6K random IOPS
- RAM: 7.6 GB, CPU: i5-3230M 2.6 GHz

### Interleaved Test (Gold Standard)

OFF/1 → ON/1 → OFF/2 → ON/2 → OFF/3 → ON/3, module fully unloaded/reloaded between each pair.

| Pass | OFF (s) | ON (s) | Gain |
|------|---------|--------|------|
| 1    | 53.04   | 43.28  | **−18.4%** |
| 2    | 52.55   | 43.21  | **−17.8%** |
| 3    | 52.17   | 42.87  | **−17.8%** |
| **Avg** | **52.58** | **43.12** | **−18.0%** |

### Blocked Test

OFF/1-3 → ON/1-3 (module loaded once for all ON passes).

| Pass | OFF (s) | ON (s) | Gain |
|------|---------|--------|------|
| 1    | 53.56   | 43.45  | **−18.9%** |
| 2    | 53.00   | 43.60  | **−17.7%** |
| 3    | 53.61   | 43.00  | **−19.8%** |
| **Avg** | **53.39** | **43.35** | **−18.8%** |

### NSD Stats (per pass, fresh module load)

| Metric | Value |
|--------|-------|
| Learned patterns | 1.57M |
| Predictions | 8.24M |
| Prefetch sent | 417K |
| Correct predictions | 2.92M |
| hit_rate_real | 99% |
| Stride predictions | 5.74M |
| Stride chain length | 2.87M (avg 2 pages/chain) |

### Why it works

Full table scans generate a perfectly sequential page access pattern
(stride = 4KB). NSD detects this stride in the first few pages and
prefetches ahead using `vfs_fadvise(WILLNEED)`. The 99% hit_rate_real
confirms almost all prefetched pages are consumed by the application.

## Sequential 64K (fio)

**Test setup:**
- fio: `--rw=read --bs=64K --size=256M --direct=0`
- SSD: SATA AHCI
- 15 runs each OFF/ON

| Metric | OFF | ON | Gain |
|--------|-----|----|------|
| Avg BW | 384 MB/s | 502 MB/s | **+30.7%** |

## HDD Sequential 64K

SATA HDD (ST1000LM024), 64K sequential read: ~53 MB/s saturated.
NSD shows no gain (disk-bound).
