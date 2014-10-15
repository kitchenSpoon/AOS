#ifndef _LIBOS_PROCESS_H_
#define _LIBOS_PROCESS_H_

#include <sel4/sel4.h>
#include <cspace/cspace.h>
#include "vm/addrspace.h"
#include "syscall/file.h"
#include "vm/vmem_layout.h"

//sosh also defines this for themself
#define MAX_PROC 16

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
typedef void (*serv_process_create_cb_t)(void* token, int err);


//a list of process
process_t* processes[MAX_PROC];

void serv_process_create(char* app_name, seL4_CPtr fault_ep, serv_process_create_cb_t callback, void* token);

#define CURPROC     (cur_proc())

process_t* cur_proc(void);
addrspace_t* proc_getas(void);
cspace_t* proc_getcroot(void);

#endif /* _LIBOS_PROCESS_H_ */
