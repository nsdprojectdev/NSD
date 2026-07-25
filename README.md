# NSD — Neural Storage Driver

Copyright (c) 2026 Ayhan Aydin

A Linux kernel module that learns I/O patterns and prefetches data
using a synaptic Markov chain / stride predictor, improving read throughput
and database query performance.

## Requirements

- Linux kernel **5.x / 6.x** (built with `CONFIG_KPROBES` and `CONFIG_SYSFS`)
- GCC (matching kernel build compiler)
- `sudo` for module load/unload

## Quick Start

```bash
make
sudo insmod nsd.ko
cat /sys/kernel/nsd/stats
```

## Architecture

```
vfs_read (kprobe) -> per-CPU ring -> worker thread -> Markov/Bigram/Stride/FreqRecency
    -> vfs_fadvise(WILLNEED) -> page cache warm -> application reads from cache
```

## Benchmarks

All results are from **interleaved OFF/ON/OFF/ON** tests on a SATA SSD with cold cache.

| Device | Workload | Gain | Methodology |
|--------|----------|------|-------------|
| **SSD** | SQLite Full Table Scan (4GB) | **-18.8%** | Interleaved, 2 passes |
| **SSD** | Sequential 64K (dd, buffered) | **+22.6%** | Interleaved, 3 passes |
| **SSD** | Random 4K (fio, buffered) | +1.1% | Interleaved, 3 passes (noise) |
| **SSD** | Read Latency | +0.8% | Interleaved (neutral) |
| **HDD** | Sequential 64K | 0% | Disk-bound |
| **HDD** | Random 4K | ~0% | Disk-bound |

See [BENCHMARKS.md](BENCHMARKS.md) for detailed methodology and raw results.
See [test_results/](test_results/) for comprehensive interleaved benchmark reports.

## Design

See [docs/DESIGN.md](docs/DESIGN.md) for architecture details.

## Roadmap

See [docs/ROADMAP.md](docs/ROADMAP.md).

## License

Dual-licensed: **GNU General Public License v2.0 only** (GPL-2.0-only)
**OR Commercial License**.

For GPL-2.0 terms see [LICENSES/GPL-2.0.txt](LICENSES/GPL-2.0.txt).
For commercial licensing inquiries: **nsd.project.dev@gmail.com**

See [LICENSE](LICENSE) for the full licensing terms.
