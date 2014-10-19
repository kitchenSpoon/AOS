#ifndef _LIBOS_UTILITY_H_
#define _LIBOS_UTILITY_H_

#include <limits.h>

/* Maximum size to send to NFS */
#define NFS_SEND_SIZE   1024 //This needs to be less than UDP package size

/* Minimum of two values. */
#define MIN(a,b) (((a)<(b))?(a):(b))

/* Page size and stuffs */
#define PAGE_OFFSET_MASK        ((PAGESIZE) - 1)
#define PAGE_ALIGN(addr)        ((addr) & ~(PAGE_OFFSET_MASK))
#define PAGE_OFFSET(addr)       ((addr) & (PAGE_OFFSET_MASK))
#define IS_PAGESIZE_ALIGNED(addr) !((addr) &  (PAGE_OFFSET_MASK))

#endif /* _LIBOS_UTILITY_H_ */
