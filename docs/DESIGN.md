# NSD Design

## Architecture

NSD uses a VFS-level kprobe on `vfs_read` to intercept file read operations,
learns access patterns via a synaptic Markov chain, and prefetches predicted
regions using `vfs_fadvise(POSIX_FADV_WILLNEED)`.

```
Userspace App
     │
     ▼  read()
┌────────────┐
│  vfs_read  │ ←── kprobe hook (sampled 1:2)
└─────┬──────┘
      │
      ▼  page cache hit?
   ┌──┴──┐
   │ YES │ → return data
   └──┬──┘
      │ NO
      ▼
   disk I/O (submit_bio)
```

## Pipeline

1. **kprobe** (`vfs_read`): Captures (file, offset, size) — 50% sampling
2. **per-CPU ring buffer**: Lock-free producer, single consumer
3. **Worker thread**: Processes events → updates Markov synapses → predicts next region
4. **Prefetch**: `vfs_fadvise(WILLNEED)` for predicted region (coalesced for contiguous ranges)

## Predictors

- **Markov** (1st order): P(next|current_region)
- **Bigram** (2nd order): P(next|prev_region, current_region)
- **FreqRecency**: Hot regions by access frequency + recency score
- **Stride**: Fixed-step pattern detection (4-delta history)

## Device Classes

| Class | Region | Threshold | Depth | Span |
|-------|--------|-----------|-------|------|
| NVMe  | 8KB    | 400       | 2     | 4KB  |
| SSD   | 4KB    | 100       | 6     | 128KB |
| HDD   | 64KB   | 700       | 2     | 4KB  |

## Key Mechanisms

- **Workload classifier**: Picks best strategy every NSD_PROFILE_WINDOW events
- **Sequential bypass**: Defers to kernel readahead when seq_ratio > threshold
- **Hysteresis**: Requires N consecutive windows to switch strategy
- **Auto-threshold**: Adjusts confidence threshold based on recent accuracy
- **Death spiral recovery**: Halves threshold if rec_acc < 100 && thresh > 300
- **Coalescing**: Merges contiguous prefetch regions into single fadvise call
