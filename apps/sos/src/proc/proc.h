#ifndef _LIBOS_PROCESS_H_
#define _LIBOS_PROCESS_H_

#include <sel4/sel4.h>
#include <cspace/cspace.h>
#include "vm/addrspace.h"
#include "vm/vmem_layout.h"

//sosh also defines this for themselves
#define MAX_PROC 64

/* This is the index where a clients syscall enpoint will
 * be stored in the clients cspace. */
#define USER_EP_CAP         1
#define USER_PRIORITY       0
#define USER_EP_BADGE       (1 << (seL4_BadgeBits - 2))

#define CURPROC             (cur_proc())
#define PROC_NULL           (-1)

typedef int pid_t;

typedef struct proc_wait_node* proc_wait_node_t;

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
    char *name; // max 32 bytes, as defined by the client
    size_t name_len;

    struct filetable* p_filetable;
    proc_wait_node_t p_wait_queue;

    bool p_initialised;
};

typedef struct {
    process_t *proc;
    int next_free;
} free_proc_slot_t;

int next_free_proc_slot;
free_proc_slot_t* processes[MAX_PROC];

/* These are callback functions corresponding to the proc_* functions below */
typedef void (*proc_create_cb_t)(void *token, int err, pid_t pid);
typedef void (*proc_wait_cb_t)(void *token, pid_t pid);

/* This function should be called before any process is created */
void proc_list_init(void);

process_t* cur_proc(void);
addrspace_t* proc_getas(void);
cspace_t* proc_getcroot(void);
pid_t proc_get_id(void);

bool is_proc_alive(pid_t pid);
void inc_proc_size_proc(process_t* proc);
void inc_proc_size(pid_t pid);
void dec_proc_size_proc(process_t* proc);
void dec_proc_size(pid_t pid);

/* Kernel can use this function to set the current process
 * If the pid is PROC_NULL then the current process will be NULL */
void set_cur_proc(pid_t pid);

/* Get the process with this pid */
process_t *proc_getproc(pid_t pid);

/* Create a process from the executable at *path*, this process shall
 * communicate with sos through the *fault_ep* */
void proc_create(char* path, size_t len, seL4_CPtr fault_ep, proc_create_cb_t callback, void* token);

/* Destroy a process with this pid, clean up process data and return the back
 * to seL4*/
int proc_destroy(pid_t pid);

/* Wait for a certain process to exit
 * if pid == -1, wait for any process to exit
 * "callback" will be called whenever the specified process finished */
int proc_wait(pid_t pid, proc_wait_cb_t callback, void *token);
#endif /* _LIBOS_PROCESS_H_ */
