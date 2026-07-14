# NSD Roadmap

## v1.0 — Current (2026-07-14)

- VFS-level kprobe on `vfs_read`
- Markov / Bigram / FreqRecency / Stride predictors
- Device-aware auto-tuning (NVMe/SSD/HDD)
- SSD: +20.5% sequential 64K throughput
- HDD: −4% to 0% (regression fixed)

## v1.1 — Planned

- `page_cache_ra_unbounded` fallback for native ext4/btrfs
- Per-device safe mode for HDD
- Random-access pattern detection for HDD structured workloads
- Strided workload optimization for SSD

## v1.2 — Future

- Write prefetch prediction (write-ahead caching)
- Multi-device coordination
- Userspace profiling interface
- Distributed training dataset collection

## v2.0 — Vision

- Block-layer integration for direct I/O workloads
- Hardware offload (NVMe TPU coprocessor)
- Cross-filesystem pattern learning
