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
#include <syscall/file.h>
#include "vm/elf.h"

extern char _cpio_archive[];

static uint32_t next_free_pid = 0;
static process_t* _cur_proc = NULL;

void proc_list_init(void){
    for(int i = 0; i < MAX_PROC; i++){
        processes[i] = NULL;
    }
}

void inc_proc_size_proc(process_t* proc){
    if(proc != NULL){
        printf("incr_proc_size_proc pid = %d\n",proc->pid);
        proc->size++;
    } else {
        printf("incr_proc_size_proc NULL\n");
    }
}

void inc_proc_size(int pid){
    printf("incr_proc_size pid = %d\n",pid);
    for(int i = 0; i < MAX_PROC; i++){
        if(processes[i] != NULL && processes[i]->pid == pid){
            processes[i]->size++;
            return;
        }
    }
}

void inc_cur_proc_size(void){
    printf("inc_cur_proc_size\n");
    if(_cur_proc != NULL){
        printf("inc_cur_proc_size sucess\n");
        inc_proc_size(_cur_proc->pid);
    }
}

void dec_proc_size(int pid){
    for(int i = 0; i < MAX_PROC; i++){
        if(processes[i] != NULL && processes[i]->pid == pid){
            processes[i]->size--;
            return;
        }
    }
}

void dec_cur_proc_size(void){
    printf("dec_cur_proc_size\n");
    if(_cur_proc != NULL){
        printf("dec_cur_proc_size sucess\n");
        dec_proc_size(_cur_proc->pid);
    }
}

void set_cur_proc(pid_t pid) {
    printf("set_cur_proc\n");
    if (pid == PROC_NULL) {
        printf("set_cur_proc NULL\n");
        _cur_proc = NULL;
        return;
    }

    for(int i = 0; i < MAX_PROC; i++){
        //if(processes[i] != NULL) printf("searching for cur_proc, we are at proc = %u\n",processes[i]->pid);
        if(processes[i] != NULL && processes[i]->pid == pid){
            _cur_proc = processes[i];
            printf("set_cur_proc_success\n");
            return;
        }
    }

    /* Do this so that we can catch bug quickly */
    printf("set_cur_proc: Could not find process with pid in process list\n");
    printf("              Setting cur_proc to NULL\n");
    _cur_proc = NULL;
}

process_t* cur_proc(void) {
    if (_cur_proc == NULL) {
        printf("cur_proc: Someone is getting a NULL proc\n");
    }
    return _cur_proc;
}

addrspace_t* proc_getas(void) {
    return (_cur_proc == NULL) ? NULL : (_cur_proc->as);
}

cspace_t* proc_getcroot(void) {
    return (_cur_proc == NULL) ? 0 : (_cur_proc->croot);
}

typedef struct{
    char* elf_base;
    process_t* proc;
    proc_create_cb_t callback;
    void* token;
} process_create_cont_t;

static void
proc_create_end(void* token, int err){
    printf("start process create end\n");
    process_create_cont_t* cont = (process_create_cont_t*)token;
    assert(cont != NULL);

    /* Might need to clean up allocated data for the process */
    if (err && cont->proc != NULL) {
        process_t *proc = cont->proc;

        /* Free TCB */
        if (proc->tcb_cap) {
            cspace_delete_cap(cur_cspace, proc->tcb_cap);
        }
        if (proc->tcb_addr) {
            ut_free(proc->tcb_addr, seL4_PageBits);
        }

        /* Free VSpace */
        if (proc->vroot) {
            cspace_delete_cap(cur_cspace, proc->vroot);
        }
        if (proc->vroot_addr) {
            ut_free(proc->vroot_addr, seL4_PageDirBits);
        }

        /* Free IPC */
        if (proc->ipc_buffer_cap) {
            cspace_delete_cap(cur_cspace, proc->vroot);
        }
        if (proc->ipc_buffer_addr) {
            ut_free(proc->ipc_buffer_cap, seL4_PageBits);
        }

        /* Free CSpace
         * This will also free user_ep_cap created in this cspace*/
        if (proc->croot) {
            cspace_destroy(proc->croot);
        }

        /* Free Addrspace */
        if (proc->as) {
            as_destroy(proc->as);
        }

        /* Free Filetable */
        if (proc->p_filetable) {
            filetable_destroy(proc->p_filetable);
        }

        cont->callback(cont->token, err, -1);
        free(cont);
        return;
    }

    cont->callback(cont->token, err, cont->proc->pid);
    free(cont);
}

static void
proc_create_part3(void* token, int err){
    printf("start process create part3\n");
    process_create_cont_t* cont = (process_create_cont_t*)token;

    if(err){
        printf("sos_process_create_part3, Failed to load elf image\n");
        proc_create_end(token, err);
        return;
    }

    /* set up the stack & the heap */
    as_define_stack(cont->proc->as, PROCESS_STACK_TOP, PROCESS_STACK_SIZE);
    if(cont->proc->as->as_stack == NULL){
        printf("sos_process_create_part3, Stack failed to be defined\n");
        proc_create_end((void*)cont, EFAULT);
        return;
    }
    as_define_heap(cont->proc->as);
    if(cont->proc->as->as_heap == NULL){
        printf("sos_process_create_part3, Heap failed to be defined\n");
        proc_create_end((void*)cont, EFAULT);
        return;
    }

    //TODO: this might cause this page to be overwrite? Currently it won't
    //because process doesn't have mmap The fix is simply creating a region for
    //it and map the page in using sos_page_map and create a callback for it
    //as_define_region();
    /* Map in the IPC buffer for the thread */
    printf("sos_process_create_part3, mapping ipc buf...\n");
    err = map_page(cont->proc->ipc_buffer_cap, cont->proc->vroot,
                   PROCESS_IPC_BUFFER,
                   seL4_AllRights, seL4_ARM_Default_VMAttributes);
    if(err){
        printf("sos_process_create_part3, Unable to map IPC buffer for user app\n");
        proc_create_end((void*)cont, EFAULT);
        return;
    }
    inc_proc_size(cont->proc->pid);

    /* Initialise filetable for this process */
    printf("sos_process_create_part3, filetable initing...\n");
    err = filetable_init(NULL, NULL, NULL, cont->proc);
    if(err){
        printf("sos_process_create_part3, Unable to initialise filetable for user app\n");
        proc_create_end((void*)cont, err);
        return;
    }

    //Add new process to our process list
    bool found = false;
    for(int i = 0; i < MAX_PROC; i++){
        if(processes[i] == NULL){
            processes[i] = cont->proc;
            found = true;
            break;
        }
    }
    if(!found){
        printf("sos_process_create_part3, can't find a slot in our process list, too many process\n");
        proc_create_end((void*)cont, EFAULT);
        return;
    }

    printf("sos_process_create_part3, filetable initing done...\n");
    /* Start the new process */
    printf("start it\n");
    seL4_UserContext context;

    memset(&context, 0, sizeof(context));
    context.pc = elf_getEntryPoint(cont->elf_base);
    context.sp = PROCESS_STACK_TOP;
    seL4_TCB_WriteRegisters(cont->proc->tcb_cap, 1, 0, 2, &context);

    proc_create_end(token, 0);
}

static void
proc_create_part2(void* token, addrspace_t *as){
    printf("start process create part2\n");
    if(as == NULL){
        printf("sos_process_create_part2, Failed to initialise address space\n");
        proc_create_end(token, EFAULT);
        return;
    }

    process_create_cont_t* cont = (process_create_cont_t*)token;
    cont->proc->as = as;

    /* load the elf image */
    elf_load(cont->proc->as, cont->elf_base, cont->proc, proc_create_part3, token);
}

void proc_create(char* path, size_t len, seL4_CPtr fault_ep, proc_create_cb_t callback, void* token) {
    printf("process_create\n");
    printf("creating process at %s\n", path);
    int err;

    seL4_CPtr user_ep_cap;

    /* These required for loading program sections */
    char* elf_base;
    unsigned long elf_size;

    printf("creating process cont\n");
    process_create_cont_t *cont = malloc(sizeof(process_create_cont_t));
    if (cont == NULL) {
        callback(token, ENOMEM, -1);
        return;
    }
    cont->elf_base  = NULL;
    cont->proc      = NULL;
    cont->callback  = callback;
    cont->token     = token;

    printf("creating process\n"); // somewhere before this we have to add \0 add the end or get the length of the name
    process_t* new_proc = malloc(sizeof(process_t));
    if(new_proc == NULL){
        printf("No memory to create new process\n");
        proc_create_end((void*)cont, ENOMEM);
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
    new_proc->name              = malloc(len+1);
    new_proc->name_len          = len;
    strcpy(new_proc->name, path);
    new_proc->name[len]         = '\0';
    new_proc->size              = 0;
    new_proc->stime             = (unsigned)time_stamp()/1000; //microsec to millsec

    cont->proc = new_proc;

    /* Create a VSpace */
    new_proc->vroot_addr = ut_alloc(seL4_PageDirBits);
    if(!new_proc->vroot_addr){
        printf("sos_process_create, No memory for new Page Directory\n");
        proc_create_end((void*)cont, ENOMEM);
        return;
    }

    err = cspace_ut_retype_addr(new_proc->vroot_addr,
                                seL4_ARM_PageDirectoryObject,
                                seL4_PageDirBits,
                                cur_cspace,
                                &new_proc->vroot);
    if(err){
        printf("sos_process_create, Failed to allocate page directory cap for client\n");
        proc_create_end((void*)cont, EFAULT);
        return;
    }

    /* Create a simple 1 level CSpace */
    new_proc->croot = cspace_create(1);
    if(new_proc->croot == NULL){
        printf("sos_process_create, Failed to create CSpace\n");
        proc_create_end((void*)cont, EFAULT);
        return;
    }

    /* Create an IPC buffer */
    new_proc->ipc_buffer_addr = ut_alloc(seL4_PageBits);
    if(!new_proc->ipc_buffer_addr){
        printf("sos_process_create, No memory for ipc buffer\n");
        proc_create_end((void*)cont, ENOMEM);
        return;
    }
    err =  cspace_ut_retype_addr(new_proc->ipc_buffer_addr,
                                 seL4_ARM_SmallPageObject,
                                 seL4_PageBits,
                                 cur_cspace,
                                 &new_proc->ipc_buffer_cap);
    if(err){
        printf("sos_process_create, Unable to allocate page for IPC buffer\n");
        proc_create_end((void*)cont, EFAULT);
        return;
    }

    if (next_free_pid & USER_EP_BADGE) {
        printf("sos_process_create: SOS ran out of pid\n");
        proc_create_end((void*)cont, EFAULT);
        return;
    }
    /* Copy the fault endpoint to the user app to enable IPC */
    user_ep_cap = cspace_mint_cap(new_proc->croot,
                                  cur_cspace,
                                  fault_ep,
                                  seL4_AllRights,
                                  seL4_CapData_Badge_new(USER_EP_BADGE | next_free_pid));

    /* should be the first slot in the space, hack I know */
    assert(user_ep_cap == 1);
    assert(user_ep_cap == USER_EP_CAP);

    new_proc->pid = next_free_pid;
    next_free_pid++;


    /* Create a new TCB object */
    new_proc->tcb_addr = ut_alloc(seL4_TCBBits);
    if(!new_proc->tcb_addr){
        printf("sos_process_create, No memory for new TCB\n");
        proc_create_end((void*)cont, ENOMEM);
        return;
    }
    err =  cspace_ut_retype_addr(new_proc->tcb_addr,
                                 seL4_TCBObject,
                                 seL4_TCBBits,
                                 cur_cspace,
                                 &new_proc->tcb_cap);
    if(err){
        printf("sos_process_create, Failed to create TCB\n");
        proc_create_end((void*)cont, EFAULT);
        return;
    }

    /* Configure the TCB */
    err = seL4_TCB_Configure(new_proc->tcb_cap, user_ep_cap, USER_PRIORITY,
                             new_proc->croot->root_cnode, seL4_NilData,
                             new_proc->vroot, seL4_NilData, PROCESS_IPC_BUFFER,
                             new_proc->ipc_buffer_cap);
    if(err){
        printf("sos_process_create, Unable to configure new TCB\n");
        proc_create_end((void*)cont, EFAULT);
        return;
    }

    /* parse the cpio image */
    printf("\nStarting \"%s\"...\n", path);
    elf_base = cpio_get_file(_cpio_archive, path, &elf_size);
    if(!elf_base){
        printf("sos_process_create, Unable to locate cpio header\n");
        proc_create_end((void*)cont, EFAULT);
        return;
    }
    cont->elf_base = elf_base;

    /* initialise address space */
    as_create(new_proc->vroot, proc_create_part2, (void*)cont);
}


int proc_destroy(int pid) {
    (void)pid;
    return 0;
}

pid_t proc_get_id(){
    return (_cur_proc == NULL) ? PROC_NULL : _cur_proc->pid;
}

int proc_wait(pid_t pid){
    (void)pid;
    return 0;
}
