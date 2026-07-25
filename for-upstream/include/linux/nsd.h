/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_NSD_H
#define _LINUX_NSD_H

struct file;
struct nsd_ops {
	void (*notify_read)(struct file *f, loff_t pos, size_t len);
	int (*lookup)(struct file *f);
};

#ifdef CONFIG_NSD
void nsd_notify_read(struct file *f, loff_t pos, size_t len);
void nsd_notify_fault(struct file *f, loff_t pos);
int nsd_lookup(struct file *f);
int nsd_register_ops(struct nsd_ops *ops);
void nsd_unregister_ops(void);
#else
static inline void nsd_notify_read(struct file *f, loff_t pos, size_t len) {}
static inline void nsd_notify_fault(struct file *f, loff_t pos) {}
static inline int nsd_lookup(struct file *f) { return 0; }
static inline int nsd_register_ops(struct nsd_ops *ops) { return 0; }
static inline void nsd_unregister_ops(void) {}
#endif
#endif /* _LINUX_NSD_H */