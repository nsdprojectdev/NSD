import sys
with open(sys.argv[1], "r") as f:
    c = f.read()

# 1. Replace kprobes.h include with fprobe.h
c = c.replace('#include <linux/kprobes.h>', '#include <linux/fprobe.h>')

# 2. Replace the handler function
old_handler = """static __maybe_unused int nsd_kprobe_pre(struct kprobe *p, struct pt_regs *regs)
{
    struct file  *file;
    struct inode *inode;
    loff_t       *pos_ptr, off;
    (void)p;

#if defined(CONFIG_X86_64)
    file    = (struct file *)regs->di;
    pos_ptr = (loff_t *)regs->cx;

    if (regs->dx < 4096) return 0;
#elif defined(CONFIG_ARM64)
    file    = (struct file *)regs->regs[0];
    pos_ptr = (loff_t *)regs->regs[3];

    if (regs->regs[2] < 4096) return 0;
#else
    return 0;
#endif

    if (!file || !pos_ptr) return 0;

    inode = file->f_inode;
    if (!inode || !S_ISREG(inode->i_mode)) return 0;
    if (!(file->f_mode & FMODE_READ)) return 0;

    off = *pos_ptr;
    if (off < 0) return 0;

    size_t io_size = 0;
#if defined(CONFIG_X86_64)
    io_size = (size_t)regs->dx;
#elif defined(CONFIG_ARM64)
    io_size = (size_t)regs->regs[2];
#endif


    if (atomic_read(&nsd.feat_procaware))
        nsd_track_micro_seq(inode, off, io_size);

    if (true) {
        atomic64_inc(&nsd.st_kprobe);
        u64 key = (u64)inode->i_ino ^ (u64)(unsigned long)inode->i_sb;
        u32 file_id = hash_64(key, NSD_FCTX_BITS);
        bool skip = READ_ONCE(nsd.skip_kprobe[file_id]) &&
                    READ_ONCE(nsd.skip_ino[file_id]) == (u64)inode->i_ino;
        if (skip && nsd_ns() - READ_ONCE(nsd.skip_time[file_id]) > 5ULL * NSEC_PER_SEC) {
            WRITE_ONCE(nsd.skip_kprobe[file_id], false);
            skip = false;
        }
        if (!skip && !nsd_ring_put(file, off, io_size))
            atomic64_inc(&nsd.st_dropped);
    }

    return 0;
}"""

new_handler = """static __maybe_unused int nsd_fprobe_entry(struct fprobe *fp, unsigned long entry_ip,
                             unsigned long ret_ip, struct ftrace_regs *fregs,
                             void *entry_data)
{
    struct file  *file;
    struct inode *inode;
    loff_t       *pos_ptr, off;
    (void)fp; (void)entry_ip; (void)ret_ip; (void)entry_data;

    file    = (struct file *)ftrace_regs_get_argument(fregs, 0);
    pos_ptr = (loff_t *)ftrace_regs_get_argument(fregs, 3);
    size_t io_size = (size_t)ftrace_regs_get_argument(fregs, 2);

    if (io_size < 4096) return 0;
    if (!file || !pos_ptr) return 0;

    inode = file->f_inode;
    if (!inode || !S_ISREG(inode->i_mode)) return 0;
    if (!(file->f_mode & FMODE_READ)) return 0;

    off = *pos_ptr;
    if (off < 0) return 0;

    if (atomic_read(&nsd.feat_procaware))
        nsd_track_micro_seq(inode, off, io_size);

    if (true) {
        atomic64_inc(&nsd.st_kprobe);
        u64 key = (u64)inode->i_ino ^ (u64)(unsigned long)inode->i_sb;
        u32 file_id = hash_64(key, NSD_FCTX_BITS);
        bool skip = READ_ONCE(nsd.skip_kprobe[file_id]) &&
                    READ_ONCE(nsd.skip_ino[file_id]) == (u64)inode->i_ino;
        if (skip && nsd_ns() - READ_ONCE(nsd.skip_time[file_id]) > 5ULL * NSEC_PER_SEC) {
            WRITE_ONCE(nsd.skip_kprobe[file_id], false);
            skip = false;
        }
        if (!skip && !nsd_ring_put(file, off, io_size))
            atomic64_inc(&nsd.st_dropped);
    }

    return 0;
}"""

assert old_handler in c, "old handler not found"
c = c.replace(old_handler, new_handler, 1)
print("OK: handler replaced")

# 3. Replace kprobe struct with fprobe struct
old_struct = """static struct kprobe nsd_kp = {
    .symbol_name = "vfs_read",
    .pre_handler = nsd_kprobe_pre,
};"""

new_struct = """static struct fprobe nsd_fp = {
    .entry_handler = nsd_fprobe_entry,
};"""

assert old_struct in c, "old struct not found"
c = c.replace(old_struct, new_struct, 1)
print("OK: struct replaced")

# 4. Replace registration
old_reg = """    ret = register_kprobe(&nsd_kp);
    if (ret) return ret;
    atomic_set(&nsd.hook_reg, 1);
    pr_info("NSD: vfs_read kprobe active\\n");"""

new_reg = """    {
        const char *syms[] = { "vfs_read" };
        ret = register_fprobe_syms(&nsd_fp, syms, 1);
    }
    if (ret) return ret;
    atomic_set(&nsd.hook_reg, 1);
    pr_info("NSD: vfs_read fprobe active\\n");"""

assert old_reg in c, "old registration not found"
c = c.replace(old_reg, new_reg, 1)
print("OK: registration replaced")

# 5. Replace unregistration
old_unreg = """    unregister_kprobe(&nsd_kp);"""
new_unreg = """    unregister_fprobe(&nsd_fp);"""

assert old_unreg in c, "old unregistration not found"
c = c.replace(old_unreg, new_unreg, 1)
print("OK: unregistration replaced")

with open(sys.argv[1], "w") as f:
    f.write(c)
print("File written successfully")
