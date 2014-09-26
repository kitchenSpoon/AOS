#ifndef _LIBOS_SWAP_H_
#define _LIBOS_SWAP_H_

#include <stdint.h>
#include <errno.h>

#include "vm/addrspace.h"
#include "vfs/vnode.h"

/*
 * Bitmap use to track free slots in our swap file
 * 1 is used
 * 0 is free
 */
#define NUM_FREE_SLOTS (32)
uint32_t free_slots[NUM_FREE_SLOTS];
typedef void (*swap_init_cb_t)(void *token, int err);

void swap_init(swap_init_cb_t callback, void *token);
int swap_find_free_slot(void);
int swap_in(addrspace_t as, seL4_Word vaddr, seL4_Word free_kvaddr);
int swap_out(seL4_Word kvaddr);

#endif /* _LIBOS_SWAP_H */
