#ifndef _LIBOS_SWAP_H_

/* Bitmap use to track free slots in our swap file */

#define NUM_FREE_SLOTS (32)
uint32_t free_slots[32];

#endif /* _LIBOS_SWAP_H */
