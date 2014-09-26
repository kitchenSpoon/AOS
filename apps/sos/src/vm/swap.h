#ifndef _LIBOS_SWAP_H_
#define _LIBOS_SWAP_H_
#include <stdio.h>
#include <assert.h>
#include <stdint.h>

/* Bitmap use to track free slots in our swap file */

#define NUM_FREE_SLOTS (32)
uint32_t free_slots[NUM_FREE_SLOTS];

typedef void (*swap_in_cb_t)(uintptr_t token, int err);

#endif /* _LIBOS_SWAP_H */
