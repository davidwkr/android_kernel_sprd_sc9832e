/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Legacy (pre-4.12) ION ioctl ABI compatibility shim.
 *
 * See ion_legacy.c for the rationale.
 */
#ifndef _ION_LEGACY_H
#define _ION_LEGACY_H

#include <linux/fs.h>

int ion_legacy_open(struct inode *inode, struct file *file);
int ion_legacy_release(struct inode *inode, struct file *file);
long ion_ioctl_dispatch(struct file *filp, unsigned int cmd, unsigned long arg);
#ifdef CONFIG_COMPAT
long ion_compat_ioctl_dispatch(struct file *filp, unsigned int cmd,
			       unsigned long arg);
#endif

#endif /* _ION_LEGACY_H */
