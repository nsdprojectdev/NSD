# NSD — Neural Storage Driver

Copyright (c) 2026 Ayhan Aydin

A Linux kernel module that learns I/O patterns and prefetches data
using a synaptic Markov chain, improving sequential read throughput
on SSDs by up to **+20.5%**.

This code includes development iterations of the NSD algorithm
(labeled v0.0.1 within certain code references).

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
vfs_read (kprobe) → per-CPU ring → worker thread → Markov/Bigram/Stride/FreqRecency
    → vfs_fadvise(WILLNEED) → page cache warm → application reads from cache
```

## Benchmarks

| Device | Workload | Gain |
|--------|----------|------|
| **SSD** | Sequential 64K | **+20.5%** |
| **HDD** | Sequential 64K | −4% to 0% (neutral) |
| **HDD** | Random 4K | ~0% |

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
