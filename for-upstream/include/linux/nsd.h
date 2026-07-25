/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_NSD_H
#define _LINUX_NSD_H

#include <linux/fs.h>

#ifdef CONFIG_NSD
void nsd_notify_read(struct file *f, loff_t pos, size_t len);
void nsd_notify_fault(struct file *f, loff_t pos);
#else
static inline void nsd_notify_read(struct file *f, loff_t pos, size_t len) {}
static inline void nsd_notify_fault(struct file *f, loff_t pos) {}
#endif

#endif /* _LINUX_NSD_H */
