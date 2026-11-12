#ifndef SHIM_KERNEL_H
#define SHIM_KERNEL_H
#include <assert.h>
#define min(a, b) ((a) < (b) ? (a) : (b))
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define BUILD_BUG_ON(c) _Static_assert(!(c), "BUILD_BUG_ON")
#define EXPORT_SYMBOL(s)
#define __init
#define MODULE_LICENSE(s)
#define MODULE_DESCRIPTION(s)
#define get_unaligned(p) ({ __typeof__(*(p)) v_; memcpy((void *)&v_, (p), sizeof(v_)); v_; })
#define put_unaligned(v, p) ({ __typeof__(*(p)) v_ = (v); memcpy((p), &v_, sizeof(v_)); })
#define get_unaligned_le16(p) ({ uint16_t v_; memcpy(&v_, (p), 2); v_; })
static inline void put_unaligned_le16(uint16_t v, void *p)
{
	memcpy(p, &v, 2);
}
#define BITS_PER_LONG (8 * sizeof(long))
static inline unsigned long __fls(unsigned long w)
{
	return BITS_PER_LONG - 1 - __builtin_clzl(w);
}
#endif
