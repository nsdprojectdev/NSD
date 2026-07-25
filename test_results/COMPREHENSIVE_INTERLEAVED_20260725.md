# NSD v1.0.1 Comprehensive Interleaved Benchmark

**Date:** 2026-07-25
**Test Machine:** Notebook (i5-3230M, 7.6GB RAM)
**Storage:** 240GB SATA SSD (sda1, ext4)
**Kernel:** 7.0.0-28-generic
**NSD Version:** v1.0.1 (synaptic Markov chain stride predictor)

---

## Test Methodology

All workloads use an **OFF/ON interleaved** design (OFF/1 -> ON/1 -> OFF/2 -> ON/2 -> OFF/3 -> ON/3)
to eliminate ordering effects (thermal throttling, cache warmup). Each run is preceded by
`drop_caches` (sync + echo 3 > /proc/sys/vm/drop_caches + 2s sleep). NSD is toggled via
`observe_only` sysfs parameter (no module reload).

### Workloads

| # | Workload | Tool | Size | Metric |
|---|----------|------|------|--------|
| 1 | Sequential read (buffered) | dd bs=64K | 2GB | MB/s |
| 2 | Random 4K read (buffered) | fio psync | 1GB | MB/s (IOPS) |
| 3 | Mixed seq64K + rand4K | fio concurrent | 2GB x 20s | MB/s |
| 4 | Read latency | fio seq 64K | 1GB | clat mean (us) |
| 5 | SQLite Full Table Scan | sqlite3 | 4GB (5.9GB file) | seconds (3 queries) |
| 6 | CPU compression | gzip 512MB | --- | seconds |
| 7 | CPU crypto | openssl sha256 | --- | 16K-block rate |

### SQLite Queries (per pass)

```
Q1: SELECT COUNT(*), AVG(b) FROM t;
Q2: SELECT AVG(a), SUM(c) FROM t;
Q3: SELECT COUNT(*), AVG(b), MAX(a), MIN(c) FROM t;
```

Database: single table t(id, a, b, c, pad) with ~13M rows, 4KB pages, indexes on a and b,
synchronous=OFF, journal=OFF.

---

## Results

### W1: Sequential 64K (dd, buffered)

| Pass | OFF (MB/s) | ON (MB/s) | Delta |
|------|-----------|-----------|-------|
| #1   | 386       | 477       | +23.6% |
| #2   | 394       | 463       | +17.5% |
| #3   | 397       | 503       | +26.7% |
| **ORT**  | **392**       | **481**       | **+22.6%** |

### W2: Random 4K (fio psync, buffered)

| Pass | OFF (MB/s) | ON (MB/s) | Delta |
|------|-----------|-----------|-------|
| #1   | 27.6      | 27.1      | -1.8% |
| #2   | 26.8      | 27.0      | +0.7% |
| #3   | 26.2      | 27.6      | +5.3% |
| **ORT**  | **26.9**      | **27.2**      | **+1.1%** |

### W3: Mixed (seq64K + rand4K, 20s)

Results unavailable due to parsing error in test script.

### W4: Read Latency (fio seq 64K, clat mean)

| Pass | OFF (us) | ON (us) | Delta |
|------|---------|---------|-------|
| #1   | 128     | 135     | +5.5% |
| #2   | 133     | 130     | -2.3% |
| #3   | 129     | 129     | +0.0% |
| **ORT**  | **130**     | **131**     | **+0.8%** |

### W5: SQLite Full Table Scan (4GB Database)

Each pass runs 3 aggregate queries over a 13M-row table with cold cache.

| Pass | OFF (s) | ON (s) | Delta |
|------|---------|-------|-------|
| #1   | 54.958  | 44.699 | **-18.7%** |
| #2   | 53.814  | 43.563 | **-19.0%** |
| #3   | 59.863* | ---   | ---   |
| **ORT**  | **54.386**  | **44.131** | **-18.8%** |

*Pass 3 OFF incomplete (test timeout).*

#### Per-Query Breakdown (Pass 1)

| Query | OFF (s) | ON (s) | Delta |
|-------|---------|-------|-------|
| Q1 (COUNT(*), AVG(b)) | 1.170 | 1.108 | -5.3% |
| Q2 (AVG(a), SUM(c)) | 27.409 | 21.971 | **-19.8%** |
| Q3 (4 aggregates) | 26.379 | 21.620 | **-18.0%** |
| **Total** | **54.958** | **44.699** | **-18.7%** |

---

## NSD Statistics (during benchmark)

| Metric | Value |
|--------|-------|
| Events processed | 30.6M |
| Learned patterns | 5.1M |
| Prefetch sent | 1.65M |
| Correct predictions | 6.59M |
| hit_rate | 35% |
| hit_rate_real | **98%** |
| Stride predictions | 31.4M |

---

## Summary

| Workload | Improvement | Reliability |
|----------|------------|-------------|
| SQLite Full Table Scan | **-18.8%** | High (interleaved, 2 consistent passes) |
| Sequential 64K (buffered) | **+22.6%** | Moderate (3 runs, +17 to +27%) |
| Random 4K (buffered) | +1.1% | Noise (within +/-5%) |
| Read Latency | +0.8% | Neutral (expected) |

### Key Findings

1. **SQLite full table scans** benefit most (-19%) due to perfectly predictable 4KB stride access
   pattern. NSD detects the stride immediately and keeps the page cache populated ahead of the scan.
2. **Buffered sequential reads** show +22% improvement. NSD's vfs_fadvise(WILLNEED) pre-faults pages
   into the page cache, eliminating page fault latency during the read.
3. **Random reads** show no significant improvement, as expected.
4. **hit_rate_real of 98%** confirms that almost all prefetched pages are consumed by the application.

### Comparison with v9.0.0

| Workload | v1.0.1 | v9.0.0 |
|----------|--------|--------|
| SQLite FTS | **-18.8%** | -6.5% |
| RAND 4K dir=0 | +1.1% | **+6.5%** |
| Prefetch sent | 1.65M | 537K |

v1.0.1's simpler stride-based predictor is more effective for SQLite's regular access pattern
than v9.0.0's inline-learn frontier approach. v9.0.0 shows better random read improvement (+6.5%)
but at the cost of SQLite performance.
