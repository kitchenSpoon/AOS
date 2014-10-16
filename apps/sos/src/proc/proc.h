#ifndef _LIBOS_PROCESS_H_
#define _LIBOS_PROCESS_H_

#include <sel4/sel4.h>
#include <cspace/cspace.h>
#include "vm/addrspace.h"
#include "vm/vmem_layout.h"

//sosh also defines this for themselves
#define MAX_PROC 100

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

    uint32_t pid;
    unsigned size;
    unsigned stime;
    char* name; // max 32 bytes, as defined by the client

    struct filetable* p_filetable;
};

process_t tty_test_process;
process_t* sosh_test_process;
typedef void (*proc_create_cb_t)(void* token, int err, int pid);


//a list of process
process_t* processes[MAX_PROC];

void proc_list_init(void);
void set_cur_proc(uint32_t pid);

/* Create a process from the executable at *path*, this process shall
 * communicate with sos through the *fault_ep* */
void proc_create(char* path, seL4_CPtr fault_ep, proc_create_cb_t callback, void* token);
int proc_destroy(int id);
int proc_get_id(void);
int proc_wait(int id);

#define CURPROC     (cur_proc())

process_t* cur_proc(void);
addrspace_t* proc_getas(void);
cspace_t* proc_getcroot(void);

#endif /* _LIBOS_PROCESS_H_ */
