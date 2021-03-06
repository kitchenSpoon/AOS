#include "proc/proc.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <cpio/cpio.h>
#include <ut_manager/ut.h>
#include <elf/elf.h>
#include <vm/mapping.h>

#include "syscall/file.h"
#include "vm/addrspace.h"
#include "vm/elf.h"
#include "dev/clock.h"

#define verbose 0
#include <sys/debug.h>

#define MAX_PID (1<<27) //max badge value is 0xfffffff, do not go above 27
#define RANGE_PER_SLOT ((int)MAX_PID/MAX_PROC)

extern char _cpio_archive[];

static process_t *_cur_proc = NULL;

static int first_free_pslot;
static int next_free_pslot[MAX_PROC];
static int next_free_pid[MAX_PROC];
static proc_wait_node_t _wait_queue = NULL;

/**********************************************************************
 * Process wait related utility functions
 **********************************************************************/

struct proc_wait_node {
    proc_wait_cb_t callback;
    void *token;
    struct proc_wait_node *next;
};

/* This function will allocate a proc_wait_node_t and initialise the node with
 * data provided */
static proc_wait_node_t
_wait_node_create(proc_wait_cb_t callback, void *token) {
    proc_wait_node_t node = malloc(sizeof(struct proc_wait_node));
    if (node == NULL) {
        return NULL;
    }
    node->callback  = callback;
    node->token     = token;
    node->next      = NULL;
    return node;
}

static proc_wait_node_t
_wait_node_add(proc_wait_node_t head, proc_wait_node_t node) {
    //assert(node != NULL);
    node->next = head;
    return head = node;
}

/**********************************************************************
 * Process ID related structs and functions
 **********************************************************************/

static pid_t
_cal_next_free_pid(pid_t pid, int slot){
    pid = ((pid+1) % RANGE_PER_SLOT) + (RANGE_PER_SLOT*slot);
    return pid;
}

static void
_free_proc_slot(pid_t pid){
    int slot = (int)(pid/RANGE_PER_SLOT);
    processes[slot] = NULL;
    next_free_pslot[slot] = first_free_pslot;
    first_free_pslot = slot;
}

void proc_list_init(void){
    first_free_pslot = 0;
    for(int i = 0; i < MAX_PROC; i++){
        processes[i] = NULL;
        next_free_pslot[i] = i+1;
        next_free_pid[i] = (int)RANGE_PER_SLOT * i;
        dprintf(3, "nextfreepid[%d] = %d\n",i,next_free_pid[i]);
    }

    //set the last processes to point to a invalid next_free slot
    next_free_pslot[MAX_PROC-1] = -1;
}

process_t *proc_getproc(pid_t pid) {
    dprintf(3, "Get proc pid = %d\n", pid);
    if (pid == PROC_NULL) {
        return NULL;
    }

    int slot = pid/RANGE_PER_SLOT;
    if(processes[slot] != NULL && processes[slot]->pid == pid){
        return processes[slot];
    } else {
        return NULL;
    }
}

pid_t proc_get_id(){
    return (_cur_proc == NULL) ? PROC_NULL : _cur_proc->pid;
}

/****************************************************************
 * Process size related structs and functions
 ***************************************************************/
 
void inc_proc_size_proc(process_t* proc){
    if(proc != NULL){
        proc->size++;
    }
}

void inc_proc_size(pid_t pid){
    process_t *proc = proc_getproc(pid);
    if (proc != NULL) {
        proc->size++;
    }
}

void dec_proc_size_proc(process_t* proc){
    if(proc != NULL){
        dprintf(3, "decr_proc_size_proc pid = %d\n",proc->pid);
        proc->size--;
    }
}

void dec_proc_size(pid_t pid){
    process_t *proc = proc_getproc(pid);
    if (proc != NULL) {
        proc->size--;
    }
}

/****************************************************************
 * General Process functions
 ***************************************************************/

bool is_proc_alive(pid_t pid) {
    dprintf(3, "is proc alive? pid = %d\n", pid);
    process_t *proc = proc_getproc(pid);
    return (proc != NULL);
}

void set_cur_proc(pid_t pid) {
    dprintf(3, "set_cur_proc pid = %d\n", pid);
    _cur_proc = proc_getproc(pid);
    if(_cur_proc == NULL){
        dprintf(3, "cur proc is NULL = %d\n",pid);
    }
}

process_t* cur_proc(void) {
    if (_cur_proc == NULL) {
        dprintf(3, "cur_proc: Someone is getting a NULL proc\n");
    }
    return _cur_proc;
}

addrspace_t* proc_getas(void) {
    dprintf(3, "proc_getas called by proc = %d\n", proc_get_id());
    return (_cur_proc == NULL) ? NULL : (_cur_proc->as);
}

cspace_t* proc_getcroot(void) {
    dprintf(3, "proc_getcroot called by proc = %d\n", proc_get_id());
    return (_cur_proc == NULL) ? 0 : (_cur_proc->croot);
}

/****************************************************************
 * Process destroy utility functions
 ***************************************************************/

static void
_free_proc_data(process_t *proc) {

    dprintf(3, "_free_proc_data: freeing name\n");
    /* Free process name */
    if (proc->name) {
        free(proc->name);
    }

    dprintf(3, "_free_proc_data: freeing ft\n");
    /* Free Filetable */
    if (proc->p_filetable) {
        filetable_destroy(proc->p_filetable);
    }

    dprintf(3, "_free_proc_data: freeing as\n");
    /* Free Addrspace. Need to do this before freeing vroot */
    if (proc->as) {
        as_destroy(proc->as);
    }

    dprintf(3, "_free_proc_data: freeing tcb\n");
    /* Free TCB */
    if (proc->tcb_cap) {
        cspace_delete_cap(cur_cspace, proc->tcb_cap);
    }
    if (proc->tcb_addr) {
        ut_free(proc->tcb_addr, seL4_TCBBits);
    }

    dprintf(3, "_free_proc_data: freeing ipc\n");
    /* Free IPC */
    if (proc->ipc_buffer_cap) {
        cspace_delete_cap(cur_cspace, proc->ipc_buffer_cap);
    }
    if (proc->ipc_buffer_addr) {
        ut_free(proc->ipc_buffer_addr, seL4_PageBits);
    }

    dprintf(3, "_free_proc_data: freeing vspace\n");
    /* Free VSpace */
    if (proc->vroot) {
        cspace_delete_cap(cur_cspace, proc->vroot);
    }
    if (proc->vroot_addr) {
        ut_free(proc->vroot_addr, seL4_PageDirBits);
    }

    dprintf(3, "_free_proc_data: freeing cspace\n");
    /* Free CSpace
     * This will also free user_ep_cap created in this cspace*/
    if (proc->croot) {
        cspace_destroy(proc->croot);
    }
}

/****************************************************************
 * Process Create
 ***************************************************************/

typedef struct{
    seL4_Word elf_entry;
    process_t* proc;
    proc_create_cb_t callback;
    void* token;
} process_create_cont_t;

static void _proc_create_part2(void *token, addrspace_t *as);
static void _proc_create_part3(void *token, int err, seL4_Word elf_entry);
static void _proc_create_part4(void *token, int err);
static void _proc_create_end(void* token, int err);

void proc_create(char* path, size_t len, seL4_CPtr fault_ep, proc_create_cb_t callback, void* token) {
    dprintf(3, "process_create\n");
    dprintf(3, "creating process at %s\n", path);
    int err;

    seL4_CPtr user_ep_cap;

    /* These required for loading program sections */
    dprintf(3, "creating process cont\n");
    process_create_cont_t *cont = malloc(sizeof(process_create_cont_t));
    if (cont == NULL) {
        callback(token, ENOMEM, -1);
        return;
    }
    cont->elf_entry = 0;
    cont->proc      = NULL;
    cont->callback  = callback;
    cont->token     = token;

    dprintf(3, "creating process\n");
    process_t* new_proc = malloc(sizeof(process_t));
    if(new_proc == NULL){
        dprintf(3, "No memory to create new process\n");
        _proc_create_end((void*)cont, ENOMEM);
        return;
    }

    new_proc->tcb_addr          = 0;
    new_proc->tcb_cap           = 0;
    new_proc->vroot_addr        = 0;
    new_proc->vroot             = 0;
    new_proc->ipc_buffer_addr   = 0;
    new_proc->ipc_buffer_cap    = 0;
    new_proc->croot             = NULL;
    new_proc->as                = NULL;
    new_proc->p_filetable       = NULL;
    new_proc->pid               = -1;
    new_proc->name              = NULL;
    new_proc->name_len          = len;
    new_proc->size              = 0;
    new_proc->stime             = (unsigned)time_stamp()/1000; //microsec to millsec
    new_proc->p_wait_queue      = NULL;
    new_proc->p_initialised     = false;

    cont->proc = new_proc;
    //try to insert new proc into list
    if(first_free_pslot == -1){
        // Can't find a free process slot
        dprintf(3, "sos_process_create, No free slot for new active process\n");
        _proc_create_end((void*)cont, EFAULT);
        return;
    }
    int i = first_free_pslot;
    processes[i] = new_proc;
    new_proc->pid = next_free_pid[i];
    next_free_pid[i] = _cal_next_free_pid(next_free_pid[i], i);
    first_free_pslot = next_free_pslot[i];
    next_free_pslot[i] = -1;

    new_proc->name = malloc(len+1);
    if (new_proc->name == NULL) {
        dprintf(3, "sos_process_create, No memory for name\n");
        _proc_create_end((void*)cont, ENOMEM);
        return;
    }
    new_proc->name_len          = len;
    strncpy(new_proc->name, path, len);
    new_proc->name[len]         = '\0';

    /* Create a VSpace */
    new_proc->vroot_addr = ut_alloc(seL4_PageDirBits);
    if(!new_proc->vroot_addr){
        dprintf(3, "sos_process_create, No memory for new Page Directory\n");
        _proc_create_end((void*)cont, ENOMEM);
        return;
    }

    err = cspace_ut_retype_addr(new_proc->vroot_addr,
                                seL4_ARM_PageDirectoryObject,
                                seL4_PageDirBits,
                                cur_cspace,
                                &new_proc->vroot);
    if(err){
        dprintf(3, "sos_process_create, Failed to allocate page directory cap for client\n");
        _proc_create_end((void*)cont, EFAULT);
        return;
    }

    /* Create a simple 1 level CSpace */
    new_proc->croot = cspace_create(1);
    if(new_proc->croot == NULL){
        dprintf(3, "sos_process_create, Failed to create CSpace\n");
        _proc_create_end((void*)cont, EFAULT);
        return;
    }

    /* Create an IPC buffer */
    new_proc->ipc_buffer_addr = ut_alloc(seL4_PageBits);
    if(!new_proc->ipc_buffer_addr){
        dprintf(3, "sos_process_create, No memory for ipc buffer\n");
        _proc_create_end((void*)cont, ENOMEM);
        return;
    }
    err =  cspace_ut_retype_addr(new_proc->ipc_buffer_addr,
                                 seL4_ARM_SmallPageObject,
                                 seL4_PageBits,
                                 cur_cspace,
                                 &new_proc->ipc_buffer_cap);
    if(err){
        dprintf(3, "sos_process_create, Unable to allocate page for IPC buffer\n");
        _proc_create_end((void*)cont, EFAULT);
        return;
    }


    dprintf(3, "new proc pid = %d\n",new_proc->pid);
    /* Copy the fault endpoint to the user app to enable IPC */
    user_ep_cap = cspace_mint_cap(new_proc->croot,
                                  cur_cspace,
                                  fault_ep,
                                  seL4_AllRights,
                                  seL4_CapData_Badge_new(USER_EP_BADGE | new_proc->pid));

    /* should be the first slot in the space*/
    //assert(user_ep_cap == 1);
    //assert(user_ep_cap == USER_EP_CAP);

    /* Create a new TCB object */
    new_proc->tcb_addr = ut_alloc(seL4_TCBBits);
    if(!new_proc->tcb_addr){
        dprintf(3, "sos_process_create, No memory for new TCB\n");
        _proc_create_end((void*)cont, ENOMEM);
        return;
    }
    err =  cspace_ut_retype_addr(new_proc->tcb_addr,
                                 seL4_TCBObject,
                                 seL4_TCBBits,
                                 cur_cspace,
                                 &new_proc->tcb_cap);
    if(err){
        dprintf(3, "sos_process_create, Failed to create TCB\n");
        _proc_create_end((void*)cont, EFAULT);
        return;
    }

    /* Configure the TCB */
    err = seL4_TCB_Configure(new_proc->tcb_cap, user_ep_cap, USER_PRIORITY,
                             new_proc->croot->root_cnode, seL4_NilData,
                             new_proc->vroot, seL4_NilData, PROCESS_IPC_BUFFER,
                             new_proc->ipc_buffer_cap);
    if(err){
        dprintf(3, "sos_process_create, Unable to configure new TCB\n");
        _proc_create_end((void*)cont, EFAULT);
        return;
    }

    dprintf(3, "\nStarting \"%s\"...\n", path);

    /* initialise address space */
    as_create(new_proc->vroot, _proc_create_part2, (void*)cont);
}

static void
_proc_create_part2(void* token, addrspace_t *as){
    dprintf(3, "start process create part2\n");
    if(as == NULL){
        dprintf(3, "sos_process_create_part2, Failed to initialise address space\n");
        _proc_create_end(token, EFAULT);
        return;
    }

    process_create_cont_t* cont = (process_create_cont_t*)token;
    cont->proc->as = as;

    /* load the elf image */
    elf_load(cont->proc->pid, cont->proc->as, cont->proc->name, cont->proc, _proc_create_part3, token);
}

static void
_proc_create_part3(void* token, int err, seL4_Word elf_entry){
    dprintf(3, "start process create part3\n");
    process_create_cont_t* cont = (process_create_cont_t*)token;

    if(err){
        dprintf(3, "sos_process_create_part3, Failed to load elf image\n");
        _proc_create_end(token, err);
        return;
    }

    cont->elf_entry = elf_entry;

    /* set up the stack & the heap */
    as_define_stack(cont->proc->as, PROCESS_STACK_TOP, PROCESS_STACK_SIZE);
    if(cont->proc->as->as_stack == NULL){
        dprintf(3, "sos_process_create_part3, Stack failed to be defined\n");
        _proc_create_end((void*)cont, EFAULT);
        return;
    }
    as_define_heap(cont->proc->as);
    if(cont->proc->as->as_heap == NULL){
        dprintf(3, "sos_process_create_part3, Heap failed to be defined\n");
        _proc_create_end((void*)cont, EFAULT);
        return;
    }

    //This might cause this page to be overwrite? Currently it won't
    //because process doesn't have mmap The fix is simply creating a region for
    //it and map the page in using sos_page_map and create a callback for it
    //as_define_region();
    /* Map in the IPC buffer for the thread */
    dprintf(3, "sos_process_create_part3, mapping ipc buf...\n");
    err = map_page(cont->proc->ipc_buffer_cap, cont->proc->vroot,
                   PROCESS_IPC_BUFFER,
                   seL4_AllRights, seL4_ARM_Default_VMAttributes);
    if(err){
        dprintf(3, "sos_process_create_part3, Unable to map IPC buffer for user app\n");
        _proc_create_end((void*)cont, EFAULT);
        return;
    }
    inc_proc_size_proc(cont->proc);

    /* Initialise filetable for this process */
    dprintf(3, "sos_process_create_part3, filetable initing...\n");
    cont->proc->p_filetable = malloc(sizeof(struct filetable));
    if (cont->proc->p_filetable == NULL) {
        dprintf(3, "sos_process_create_part3, No memory for filetable\n");
        _proc_create_end((void*)cont, ENOMEM);
        return;
    }
    err = filetable_init(cont->proc->p_filetable, _proc_create_part4, (void*)cont);
    if(err){
        dprintf(3, "sos_process_create_part3, Unable to initialise filetable for user app\n");
        _proc_create_end((void*)cont, err);
        return;
    }
}

static void
_proc_create_part4(void* token, int err) {
    dprintf(3, "start process create part4\n");
    process_create_cont_t* cont = (process_create_cont_t*)token;

    if (err) {
        dprintf(3, "failed initialising filetable\n");
        _proc_create_end((void*)cont, err);
        return;
    }

    dprintf(3, "sos_process_create_part3, filetable initing done...\n");
    /* Start the new process */
    dprintf(3, "start it\n");
    seL4_UserContext context;

    memset(&context, 0, sizeof(context));
    context.pc = cont->elf_entry;
    context.sp = PROCESS_STACK_TOP;
    seL4_TCB_WriteRegisters(cont->proc->tcb_cap, 1, 0, 2, &context);

    _proc_create_end((void*)cont, 0);
}

static void
_proc_create_end(void* token, int err){
    dprintf(3, "start process create end\n");
    process_create_cont_t* cont = (process_create_cont_t*)token;
    //assert(cont != NULL);

    if (err) {
        if (cont->proc != NULL) {
            _free_proc_slot(cont->proc->pid);
            _free_proc_data(cont->proc);
            free(cont->proc);
        }
        cont->callback(cont->token, err, -1);
        free(cont);
        return;
    }

    cont->proc->p_initialised = true;
    cont->callback(cont->token, err, cont->proc->pid);
    free(cont);
}


/****************************************************************
 *  Process Destroy
 ***************************************************************/

int proc_destroy(pid_t pid) {
    dprintf(3, "proc_destroyed called, pid = %d, proc = %d\n", pid, proc_get_id());

    process_t *proc = NULL;
    proc_wait_node_t node = NULL;
    proc = proc_getproc(pid);
    if (proc == NULL) {
        return EINVAL;
    }

    if (!proc->p_initialised) {
        // Cannot kill a process that has not been initialised
        return EFAULT;
    }
    dprintf(3, "proc_destroy: freeing proc in processes list\n");
    _free_proc_slot(proc->pid);

    dprintf(3, "proc_destroy: freeing proc\n");
    _free_proc_data(proc);

    dprintf(3, "proc_destroy: waking up processes in global wait queue\n");
    /* Wake up processes in the global wait queue */
    node = _wait_queue;
    while (node != NULL) {
        node->callback(node->token, pid);
        proc_wait_node_t prev = node;
        node = node->next;
        free(prev);
    }
    _wait_queue = NULL;

    dprintf(3, "proc_destroy: waking up processes in process's wait queue\n");
    /* Wake up processes in the wait queue of this process */
    node = proc->p_wait_queue;
    while (node != NULL) {
        node->callback(node->token, pid);
        proc_wait_node_t prev = node;
        node = node->next;
        free(prev);
    }
    proc->p_wait_queue = NULL;

    free(proc);

    return 0;
}

/****************************************************************
 * Process Wait
 ***************************************************************/

int proc_wait(pid_t pid, proc_wait_cb_t callback, void *token){
    dprintf(3, "proc_wait called, proc %d waiting for pid %d\n", proc_get_id(), pid);
    if (pid == -1) {
        proc_wait_node_t node = _wait_node_create(callback, token);
        if (node == NULL) {
            return ENOMEM;
        }
        _wait_queue = _wait_node_add(_wait_queue, node);
        return 0;
    }

    for (int i = 0; i < MAX_PROC; i++) {
        if (processes[i] != NULL && processes[i]->pid == pid) {

            proc_wait_node_t node = _wait_node_create(callback, token);
            if (node == NULL) {
                return ENOMEM;
            }
            processes[i]->p_wait_queue =
                _wait_node_add(processes[i]->p_wait_queue, node);
            return 0;
        }
    }

    return EINVAL;
}
