#ifndef _LIBOS_UTILITY_H_
#define _LIBOS_UTILITY_H_

#include <limits.h>

/* Minimum of two values. */
#define MIN(a,b) (((a)<(b))?(a):(b))

/* Page size and stuffs */
#define PAGE_OFFSET_MASK              ((PAGESIZE) - 1)
#define PAGE_ALIGN(addr)      ((addr) & ~(PAGE_OFFSET_MASK))
#define IS_PAGESIZE_ALIGNED(addr) !((addr) &  (PAGE_OFFSET_MASK))

#endif /* _LIBOS_UTILITY_H_ */
