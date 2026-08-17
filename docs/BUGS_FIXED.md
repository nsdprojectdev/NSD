# BUGS_FIXED

Record of bugs found and fixed during NSD development.

---

## 2026-08-17 — Buffer overflow in sysfs debug interface (sprintf → scnprintf)

**File:** `nsd.c` (9 call sites)

**Symptom:** After extended use (4.5 days uptime, NSD loaded), `drop_caches`
triggered kernel oops + RCU stall + full system hang:

```
Oops: general protection fault, probably for non-canonical address 0x6f635f64616576d0
RIP: lru_gen_clear_refs+0x8f/0x100  (MGLRU)
RIP: memcg_list_lru_alloc+0xd5/0x220 (socket inode alloc)
```

ASCII decoding of the oops registers (`read_count`, `=20874`, `ot=1049`)
showed the corrupted memory contained parts of the `fctx_debug` output.

**Root cause:** `fctx_debug_show()` produced a ~110-character line per fctx
slot (`sprintf(buf + len, ...)` — unbounded write). With 256 slots that is
~29KB written into a kernel sysfs read buffer of only PAGE_SIZE (4096 bytes),
overflowing the buffer and corrupting kernel memory. The other 8 sysfs show
handlers had the same pattern (unbounded `sprintf` into a fixed-size buffer);
they had not triggered yet only because their output is smaller.

**How it was found:** The ASCII strings in the dumped oops registers
(`read_co` = the `read_count` field name) matched the `fctx_debug` format,
and a `kobj_attr_show returned bad count` warning appeared 56s before the
first oops.

**Fix:** All `sprintf(buf, ...)` calls in the file were replaced with bounded
`scnprintf(buf, PAGE_SIZE, ...)`; `fctx_debug_show()` additionally breaks out
of its slot loop at `len >= PAGE_SIZE`. Result: output is capped at exactly
4095 bytes — overflow is impossible.

**Verification:** After the fix, 100× `cat /sys/kernel/nsd/fctx_debug` plus
50× stress reads of all other sysfs files: 0 oops, 0 BUG, 0 warnings.
A 15-round drop_caches → fctx_debug → SQLite pass loop also ran clean.
