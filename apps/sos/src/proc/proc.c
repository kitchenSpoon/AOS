#include "proc/proc.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <cpio/cpio.h>
#include <ut_manager/ut.h>
#include <elf/elf.h>
#include <vm/mapping.h>

extern process_t tty_test_process;
extern char _cpio_archive[];

#define USER_BADGE 1 // hack TODO change this
#define USER_PRIORITY  15 // hack TODO change this

//TODO: hacking before having cur_proc() function
process_t* cur_proc(void) {
    return &tty_test_process;
}

addrspace_t* proc_getas(void) {
    return (cur_proc()->as);
}

cspace_t* proc_getcroot(void) {
    return (cur_proc()->croot);
}

typedef struct{
    char* elf_base;
    process_t* proc;
    serv_process_create_cb_t callback;
    void* token;
} process_create_cont_t;

void serv_process_create_end(void* token, int err){

    process_create_cont_t* cont = (process_create_cont_t*)token;
    cont->callback(cont->token, err);

    /* Clear */
    free(cont);
}

void serv_process_create_part3(void* token, int err){
    printf("start process create part3\n");
    process_create_cont_t* cont = (process_create_cont_t*)token;

    if(err){
        printf("sos_process_create_part3, Failed to load elf image\n");
        //free VSPACE
        //free CSPACE
        //free IPC_BUF
        //free TCB
        //free AS
        //call sos_process_create_end(ENOMEM);
        return;
    }

    /* set up the stack & the heap */
    as_define_stack(cont->proc->as, PROCESS_STACK_TOP, PROCESS_STACK_SIZE);
    if(cont->proc->as->as_stack == NULL){
        printf("sos_process_create_part3, Stack failed to be defined\n");
        //free VSPACE
        //free CSPACE
        //free IPC_BUF
        //free TCB
        //free AS
        //call sos_process_create_end(ENOMEM);
        return;
    }
    as_define_heap(cont->proc->as);
    if(cont->proc->as->as_heap == NULL){
        printf("sos_process_create_part3, Heap failed to be defined\n");
        //free VSPACE
        //free CSPACE
        //free IPC_BUF
        //free TCB
        //free AS
        //call sos_process_create_end(ENOMEM);
        return;
    }

    //TODO: this might cause this page to be overwrite? Currently it won't
    //because process doesn't have mmap The fix is simply creating a region for
    //it and map the page in using sos_page_map and create a callback for it
    //as_define_region();
    /* Map in the IPC buffer for the thread */
    err = map_page(cont->proc->ipc_buffer_cap, cont->proc->vroot,
                   PROCESS_IPC_BUFFER,
                   seL4_AllRights, seL4_ARM_Default_VMAttributes);
    if(err){
        printf("sos_process_create_part3, Unable to map IPC buffer for user app\n");
        //free VSPACE
        //free CSPACE
        //free IPC_BUF
        //free TCB
        //free AS
        //call sos_process_create_end(ENOMEM);
        return;
    }

    /* Initialise filetable for this process */
    err = filetable_init(NULL, NULL, NULL);
    if(err){
        printf("sos_process_create_part3, Unable to initialise filetable for user app\n");
        //free VSPACE
        //free CSPACE
        //free IPC_BUF
        //free TCB
        //free AS
        //unmap IPC buffer
        //call sos_process_create_end(ENOMEM);
        return;
    }

    /* Start the new process */
    seL4_UserContext context;

    memset(&context, 0, sizeof(context));
    context.pc = elf_getEntryPoint(cont->elf_base);
    context.sp = PROCESS_STACK_TOP;
    seL4_TCB_WriteRegisters(cont->proc->tcb_cap, 1, 0, 2, &context);

    serv_process_create_end(token, 0);
}

void serv_process_create_part2(void* token, addrspace_t *as){
    printf("start process create part2\n");
    if(as == NULL || as->as_pd_caps == NULL || as->as_pd_regs == NULL){
        printf("sos_process_create_part2, as == NULL || as->as_pd_caps == NULL || as->as_pd_regs == NULL\n");
        printf("Failed to initialise address space\n");
        //free VSPACE
        //free CSPACE
        //free IPC_BUF
        //free TCB
        //call sos_process_create_end(ENOMEM);
        return;
    }

    process_create_cont_t* cont = (process_create_cont_t*)token;
    cont->proc->as = as;

    /* load the elf image */
    elf_load(cont->proc->as, cont->elf_base, serv_process_create_part3, token);
}

void serv_process_create(char* app_name, seL4_CPtr fault_ep, serv_process_create_cb_t callback, void* token) {
    printf("sos_process_create\n");
    printf("creating process %s\n",app_name); // somewhere before this we have to add \0 add the end or get the length of the name
    int err;

    process_t* new_proc = malloc(sizeof(process_t));
    if(new_proc == NULL){
        printf("No memory to create new process\n");
        //call sos_process_create_end(ENOMEM);
        return;
    }

    seL4_CPtr user_ep_cap;

    /* These required for loading program sections */
    char* elf_base;
    unsigned long elf_size;

    /* Create a VSpace */
    new_proc->vroot_addr = ut_alloc(seL4_PageDirBits);
    if(!new_proc->vroot_addr){
        printf("sos_process_create, No memory for new Page Directory\n");
        //call sos_process_create_end(ENOMEM);
        return;
    }

    err = cspace_ut_retype_addr(new_proc->vroot_addr,
                                seL4_ARM_PageDirectoryObject,
                                seL4_PageDirBits,
                                cur_cspace,
                                &new_proc->vroot);
    if(err){
        printf("sos_process_create, Failed to allocate page directory cap for client\n");
        //call sos_process_create_end(ENOMEM);
        return;
    }

    /* Create a simple 1 level CSpace */
    new_proc->croot = cspace_create(1);
    if(new_proc->croot == NULL){
        printf("sos_process_create, Failed to create CSpace\n");
        //free VSPACE
        //call sos_process_create_end(ENOMEM);
        return;
    }

    /* Create an IPC buffer */
    new_proc->ipc_buffer_addr = ut_alloc(seL4_PageBits);
    if(!new_proc->ipc_buffer_addr){
        printf("sos_process_create, No memory for ipc buffer\n");
        //free VSPACE
        //free CSPACE
        //call sos_process_create_end(ENOMEM);
        return;
    }
    err =  cspace_ut_retype_addr(new_proc->ipc_buffer_addr,
                                 seL4_ARM_SmallPageObject,
                                 seL4_PageBits,
                                 cur_cspace,
                                 &tty_test_process.ipc_buffer_cap);
    if(err){
        printf("sos_process_create, Unable to allocate page for IPC buffer\n");
        //free VSPACE
        //free CSPACE
        //call sos_process_create_end(ENOMEM);
        return;
    }

    /* Copy the fault endpoint to the user app to enable IPC */
    user_ep_cap = cspace_mint_cap(new_proc->croot,
                                  cur_cspace,
                                  fault_ep,
                                  seL4_AllRights,
                                  seL4_CapData_Badge_new(USER_BADGE)); //TODO add user badge
    /* should be the first slot in the space, hack I know */
    //assert(user_ep_cap == 1);
    //assert(user_ep_cap == USER_EP_CAP);

    /* Create a new TCB object */
    new_proc->tcb_addr = ut_alloc(seL4_TCBBits);
    if(!new_proc->tcb_addr){
        printf("sos_process_create, No memory for new TCB\n");
        //free VSPACE
        //free CSPACE
        //free IPC_BUF
        //call sos_process_create_end(ENOMEM);
        return;
    }
    err =  cspace_ut_retype_addr(new_proc->tcb_addr,
                                 seL4_TCBObject,
                                 seL4_TCBBits,
                                 cur_cspace,
                                 &new_proc->tcb_cap);
    if(err){
        printf("sos_process_create, Failed to create TCB\n");
        //free VSPACE
        //free CSPACE
        //free IPC_BUF
        //call sos_process_create_end(ENOMEM);
        return;
    }

    /* Configure the TCB */
    err = seL4_TCB_Configure(new_proc->tcb_cap, user_ep_cap, USER_PRIORITY,
                             new_proc->croot->root_cnode, seL4_NilData,
                             new_proc->vroot, seL4_NilData, PROCESS_IPC_BUFFER,
                             new_proc->ipc_buffer_cap);
    if(err){
        printf("sos_process_create, Unable to configure new TCB\n");
        //free VSPACE
        //free CSPACE
        //free IPC_BUF
        //free TCB
        //call sos_process_create_end(ENOMEM);
        return;
    }

    /* parse the cpio image */
    dprintf(1, "\nStarting \"%s\"...\n", app_name);
    elf_base = cpio_get_file(_cpio_archive, app_name, &elf_size);
    if(!elf_base){
        printf("sos_process_create, Unable to locate cpio header\n");
        //free VSPACE
        //free CSPACE
        //free IPC_BUF
        //free TCB
        //call sos_process_create_end(ENOMEM);
        return;
    }

    /* Initialise the continuation preparing to call as_create */
    process_create_cont_t* cont = malloc(sizeof(process_create_cont_t));
    if(cont == NULL){
        printf("sos_process_create, Unable to allocate memory for first process\n");
        //free VSPACE
        //free CSPACE
        //free IPC_BUF
        //free TCB
        //call sos_process_create_end(ENOMEM);
        return;
    }

    cont->elf_base = elf_base;
    cont->proc = new_proc;
    cont->callback = callback;
    cont->token = token;

    /* initialise address space */
    as_create(new_proc->vroot, serv_process_create_part2, (void*)cont);
}


