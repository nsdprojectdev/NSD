# NSD - Neural Storage Driver: Linux Kernel Patch Series

## Overview

NSD is a learning prefetcher for the Linux kernel page cache. It monitors
I/O patterns via a hook in `filemap_read()` and prefetches pages ahead of
the application.

Unlike the kernel existing readahead (fixed window), NSD builds a synaptic
Markov chain model of access patterns at 4KB region granularity. It detects
sequential strides, repeating patterns, and learned transitions.

## Performance

| Workload | Improvement |
|----------|-------------|
| SQLite Full Table Scan (4GB) | -18.8% query time |
| Sequential 64K read (buffered) | +22.6% throughput |
| Random 4K read (buffered) | +1.1% (noise) |

98% real hit rate on prefetched pages.

## Patch Series

1. **mm/filemap: Add NSD prefetch hook**
   - One function call in `filemap_read()`
   - Protected by CONFIG_NSD ifdef
   - Zero overhead when disabled

2. **nsd: Core prediction engine**
   - Synaptic Markov chain table
   - per-CPU ring buffer
   - sysfs interface (/sys/kernel/nsd/)
   - Prefetch worker

3. **nsd: Stride predictor**
   - Sequential stride detection
   - Chain prediction

4. **Documentation: Add NSD docs**
   - Kconfig help
   - Documentation/filesystems/nsd.rst
   - Maintainers entry

## Status

- RFC v1: Ready for review
- Tested: x86_64, kernel 7.0.0
- Todo: ARM64, power management, cgroup awareness
