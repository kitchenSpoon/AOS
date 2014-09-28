#ifndef _LIBOS_SWAP_H_
#define _LIBOS_SWAP_H_

#include <stdint.h>
#include <errno.h>

#include "vm/addrspace.h"
#include "vfs/vnode.h"

typedef void (*swap_out_cb_t)(void *token, int err);

/*
 * Perform a swap in
 * This is an asynchronous syscall
 * @param as - The address space of the memory that we want to swap in
 * @param vaddr - The page data that we want to swap in
 * @param free_kvaddr - The memory that we we are copying data to
 */
int swap_in(addrspace_t as, seL4_Word vaddr, seL4_Word free_kvaddr);

/*
 * Perform a swap out for the frame pointed to by kvaddr
 * This is an asynchronous syscall
 * @param kvaddr - The frame that is going to be swapped out
 * @param callback - the function that will be called when swap_out finished
 * @param token - this will be passed unchanged to the callback function
 */
void swap_out(seL4_Word kvaddr, swap_out_cb_t callback, void *token);

#endif /* _LIBOS_SWAP_H */
