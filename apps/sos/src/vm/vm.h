#ifndef _LIBOS_VM_H_
#define _LIBOS_VM_H_

#include <sel4/sel4.h>
#include <errno.h>

#include "vm/addrspace.h"

/*
 * Initialise frame table. Reserve memory and initialise values for frame table
 *
 * Returns 0 iff successful
 */
int frame_init(void);


/*
 * Check if there is still free frame in the frametable
 */
bool frame_has_free(void);


/*
 * Callback for frame_alloc
 * @param kvaddr the return kernel address that SOS can use or NULL if there is
 *               no more memory
 */
typedef void (*frame_alloc_cb_t)(void *token, seL4_Word kvaddr);

/*
 * Allocate a new frame that could be used in SOS.
 * This is an asynchronous function, when it finishes,
 * it will call the *callback* function with *token* passed in unchanged
 */
int frame_alloc(seL4_Word vaddr, addrspace_t* as, bool noswap, frame_alloc_cb_t callback, void *token);

/*
 * Free the frame with this SOS's vaddr
 *
 * Returns 0 only if successul
 */
int frame_free(seL4_Word vaddr);

/*
 * Get the frame's cap associated with this sos's vaddr
 */
int frame_get_cap(seL4_Word vaddr, seL4_CPtr *frame_cap);

/*
 * Handle VM fault for SOS
 * This will be the one who reply to the client
 */
void sos_VMFaultHandler(seL4_CPtr reply, seL4_Word fault_addr, seL4_Word fsr);

/*
 * Lock/Unlock a frame
 */
int frame_lock_frame(seL4_Word vaddr);
int frame_unlock_frame(seL4_Word vaddr);
int frame_is_locked(seL4_Word vaddr, bool *is_locked);

/*
 * Get kvaddr of a avaliable frame
 * TODO: I don't think this is necessary anymore - Vy
 */
seL4_Word get_free_frame_kvaddr();

addrspace_t* frame_get_as(seL4_Word kvaddr);

seL4_Word frame_get_vaddr(seL4_Word kvaddr);

int set_frame_referenced(seL4_Word kvaddr);

#endif /* _LIBOS_VM_H_ */
