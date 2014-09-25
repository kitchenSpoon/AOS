#ifndef _LIBOS_VM_H_
#define _LIBOS_VM_H_

#include <sel4/sel4.h>
#include <errno.h>

/* Return status of frametable functions */
#define FRAMETABLE_OK       0   /* Function succeed */
#define FRAMETABLE_FAILED   1   /* Internal failure of frametable functions */
#define FRAMETABLE_INVAL    2   /* Invalid input is given */

/*
 * Initialise frame table. Reserve memory and initialise values for frame table
 *
 * Returns 0 iff successful
 */
int frame_init(void);

/*
 * Allocate a new frame that could be used in SOS.
 *
 * Returns the vaddr of the allocated frame or NULL if failed
 */
seL4_Word frame_alloc(void);

/*
 * Free the frame with this SOS's vaddr
 *
 * Returns FRAMETABLE_OK only if successul
 */
int frame_free(seL4_Word vaddr);

/*
 * Get the frame's cap associated with this sos's vaddr
 *
 * Returns FRAMETABLE_OK iff succeed
 */
int frame_get_cap(seL4_Word vaddr, seL4_CPtr *frame_cap);

int sos_VMFaultHandler(seL4_Word fault_addr, seL4_Word fsr);

#endif /* _LIBOS_VM_H_ */
