#ifndef _LIBOS_PROCESS_H_
#define _LIBOS_PROCESS_H_

#include <sel4/sel4.h>
#include <cspace/cspace.h>
#include "vm/addrspace.h"
#include "syscall/file.h"

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

    struct filetable* p_filetable;
};

process_t tty_test_process;

#define curproc     (cur_proc())

process_t* cur_proc(void);
addrspace_t* proc_getas(void);
cspace_t* proc_getcroot(void);

#endif /* _LIBOS_PROCESS_H_ */
