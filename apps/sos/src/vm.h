#ifndef _LIBOS_VM_H_
#define _LIBOS_VM_H_

#include <errno.h>

#define FRAME_IS_OK         0
#define FRAME_IS_UNINT      (-1)
#define FRAME_IS_FAIL       (-2)
/*
 * Initialise frame table. Reserve memory and initialise values for frame table 
 *
 * Returns FRAME_IS_OK iff successful
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
 * Returns FRAME_IS_OK only if successul
 */ 
int frame_free(seL4_Word vaddr);

/*
 * Get the capability of the frame pointed by vaddr
 */
seL4_CPtr frame_get_cap(seL4_Word vaddr);

int sos_VMFaultHandler(seL4_Word fault_addr, int fault_type);

#endif /* _LIBOS_VM_H_ */
