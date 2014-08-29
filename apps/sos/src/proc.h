#ifndef _LIBOS_PROCESS_H_
#define _LIBOS_PROCESS_H_

#include <sel4/sel4.h>
#include <cspace/cspace.h>
#include "addrspace.h"

typedef struct process process_t;
struct process {

    seL4_Word tcb_addr;
    seL4_TCB tcb_cap;

    seL4_Word vroot_addr;
    seL4_ARM_PageDirectory vroot;

    seL4_Word ipc_buffer_addr;
    seL4_CPtr ipc_buffer_cap;

    cspace_t *croot;

    addrspace_t *as;

};

process_t tty_test_process;

process_t* cur_proc(void);
addrspace_t* proc_getas(void);
seL4_ARM_PageDirectory proc_getvroot(void);
cspace_t* proc_getcroot(void);

#endif /* _LIBOS_PROCESS_H_ */
