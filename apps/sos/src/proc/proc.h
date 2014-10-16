#ifndef _LIBOS_PROCESS_H_
#define _LIBOS_PROCESS_H_

#include <sel4/sel4.h>
#include <cspace/cspace.h>
#include "vm/addrspace.h"
#include "vm/vmem_layout.h"

//sosh also defines this for themselves
#define MAX_PROC 100

/* This is the index where a clients syscall enpoint will
 * be stored in the clients cspace. */
#define USER_EP_CAP         1
#define USER_PRIORITY       0
#define USER_EP_BADGE       (1 << (seL4_BadgeBits - 2))

#define CURPROC             (cur_proc())
#define PROC_NULL           (-1)

typedef int pid_t;

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

    pid_t pid;
    unsigned size;
    unsigned stime;
    char* name; // max 32 bytes, as defined by the client

    struct filetable* p_filetable;
};

process_t* processes[MAX_PROC];

/* These are callback functions corresponding to the proc_* functions below */
typedef void (*proc_create_cb_t)(void* token, int err, pid_t pid);

/* This function should be called before any process is created */
void proc_list_init(void);

/* Kernel can use this function to set the current process
 * If the pid is PROC_NULL then the current process will be NULL */
void set_cur_proc(pid_t pid);

/* Create a process from the executable at *path*, this process shall
 * communicate with sos through the *fault_ep* */
void proc_create(char* path, seL4_CPtr fault_ep, proc_create_cb_t callback, void* token);

/* Destroy a process with this pid, clean up process data and return the back
 * to seL4*/
int proc_destroy(pid_t pid);

/* Get PID of the current process */
pid_t proc_get_id(void);

/* Wait for a certain process to exit, if pid == -1, wait for any process to
 * exit */
int proc_wait(pid_t pid);

process_t* cur_proc(void);
addrspace_t* proc_getas(void);
cspace_t* proc_getcroot(void);

#endif /* _LIBOS_PROCESS_H_ */
