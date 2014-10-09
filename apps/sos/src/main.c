/*
 * Copyright 2014, NICTA
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(NICTA_BSD)
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include <cspace/cspace.h>

#include <cpio/cpio.h>
#include <nfs/nfs.h>
#include <elf/elf.h>
#include <serial/serial.h>

#include "dev/network.h"

#include "ut_manager/ut.h"
#include "vm/vm.h"
#include "vm/mapping.h"
#include "vm/vmem_layout.h"
#include "vfs/vfs.h"
#include "dev/clock.h"
#include "dev/nfs_dev.h"
#include "proc/proc.h"
#include "vm/addrspace.h"
#include "syscall/syscall.h"
#include "vm/swap.h"
#include <limits.h>

#include <autoconf.h>

#define verbose 5
#include <sys/debug.h>
#include <sys/panic.h>

/* This is the index where a clients syscall enpoint will
 * be stored in the clients cspace. */
#define USER_EP_CAP          (1)
/* To differencient between async and and sync IPC, we assign a
 * badge to the async endpoint. The badge that we receive will
 * be the bitwise 'OR' of the async endpoint badge and the badges
 * of all pending notifications. */
#define IRQ_EP_BADGE         (1 << (seL4_BadgeBits - 1))
/* All badged IRQs set high bet, then we use uniq bits to
 * distinguish interrupt sources */
#define IRQ_BADGE_NETWORK (1 << 0)
#define IRQ_BADGE_TIMER (1 << 1)

#define TTY_NAME             CONFIG_SOS_STARTUP_APP
#define TTY_PRIORITY         (0)
#define TTY_EP_BADGE         (101)

#define ROOT_PATH           "/"

/* The linker will link this symbol to the start address  *
 * of an archive of attached applications.                */
extern char _cpio_archive[];

const seL4_BootInfo* _boot_info;

extern process_t tty_test_process;

seL4_CPtr _sos_ipc_ep_cap;
seL4_CPtr _sos_interrupt_ep_cap;

/**
 * NFS mount point
 */
extern fhandle_t mnt_point;

void handle_syscall(seL4_Word badge, int num_args) {

    seL4_Word syscall_number;
    seL4_CPtr reply_cap;

    syscall_number = seL4_GetMR(0);

    /* Save the caller */
    reply_cap = cspace_save_reply_cap(cur_cspace);
    assert(reply_cap != CSPACE_NULL);

    /* Process system call */
    switch (syscall_number) {
    case SOS_SYSCALL_PRINT:
    {
        size_t msg_len = num_args;
        char data[seL4_MsgMaxLength];
        for (size_t i=0; i<msg_len; i++) {
            data[i] = (char)seL4_GetMR(i+1);
        }

        serv_sys_print(reply_cap, data, msg_len);

        break;
    }
    case SOS_SYSCALL_SYSBRK:
    {
        seL4_Word newbrk = (seL4_Word)seL4_GetMR(1);
        serv_sys_sbrk(reply_cap, newbrk);
        break;
    }
    case SOS_SYSCALL_OPEN:
    {
        printf("\n---sos open called---\n");
        seL4_Word path = (seL4_Word)seL4_GetMR(1);
        size_t nbyte   = (size_t)seL4_GetMR(2);
        uint32_t flags = (uint32_t)seL4_GetMR(3);
        serv_sys_open(reply_cap, path, nbyte, flags);
        break;
    }
    case SOS_SYSCALL_CLOSE:
    {
        int fd = seL4_GetMR(1);
        serv_sys_close(reply_cap, fd);
        break;
    }
    case SOS_SYSCALL_READ:
    {
        printf("\n---sos read called at %lu---\n", (long unsigned)time_stamp());
        int fd        = (int)seL4_GetMR(1);
        seL4_Word buf = (seL4_Word)seL4_GetMR(2);
        size_t nbyte  = (size_t)seL4_GetMR(3);
        serv_sys_read(reply_cap, fd, buf, nbyte);

        break;
    }
    case SOS_SYSCALL_WRITE:
    {
        printf("\n---sos write called at %lu---\n", (long unsigned)time_stamp());
        int fd          = (int)seL4_GetMR(1);
        seL4_Word buf   = (seL4_Word)seL4_GetMR(2);
        size_t nbyte    = (size_t)seL4_GetMR(3);
        serv_sys_write(reply_cap, fd, buf, nbyte);
        break;
    }
    case SOS_SYSCALL_SLEEP:
    {
        serv_sys_sleep(reply_cap, seL4_GetMR(1));
        break;
    }
    case SOS_SYSCALL_TIMESTAMP:
    {
        serv_sys_timestamp(reply_cap);
        break;
    }
    case SOS_SYSCALL_GETDIRENT:
    {
        printf("\n---sos getdirent called at %lu---\n", (long unsigned)time_stamp());
        int pos          = (int)seL4_GetMR(1);
        char *name       = (char *)seL4_GetMR(2);
        size_t nbyte     = (size_t)seL4_GetMR(3);
        serv_sys_getdirent(reply_cap, pos, name, nbyte);
        break;
    }
    case SOS_SYSCALL_STAT:
    {
        char *path          = (char *)seL4_GetMR(1);
        size_t path_len     = (size_t)seL4_GetMR(2);
        sos_stat_t *stat    = (sos_stat_t *)seL4_GetMR(3);
        serv_sys_stat(reply_cap, path, path_len, stat);
        break;
    }
    default:
        printf("Unknown syscall %d\n", syscall_number);
        /* we don't want to reply to an unknown syscall */
    }

}

void handle_pagefault(void) {
    seL4_Word pc = seL4_GetMR(0);
    seL4_Word fault_addr = seL4_GetMR(1);
    bool ifault = (bool)seL4_GetMR(2);
    seL4_Word fsr = seL4_GetMR(3);
    dprintf(0, "vm fault at 0x%08x, pc = 0x%08x, %s\n", fault_addr, pc,
            ifault ? "Instruction Fault" : "Data fault");

    if (ifault) {
        // we don't handle this
    } else {
        seL4_CPtr reply_cap;

        /* Save the caller */
        reply_cap = cspace_save_reply_cap(cur_cspace);
        assert(reply_cap != CSPACE_NULL);

        sos_VMFaultHandler(reply_cap, fault_addr, fsr);
    }
}

void syscall_loop(seL4_CPtr ep) {

    while (1) {
        //printf("looping\n");
        seL4_Word badge;
        seL4_Word label;
        seL4_MessageInfo_t message;

        message = seL4_Wait(ep, &badge);
        //printf("badge=0x%x\n", badge);
        label = seL4_MessageInfo_get_label(message);
        if(badge & IRQ_EP_BADGE){
            /* Interrupt */
            if (badge & IRQ_BADGE_NETWORK) {
                network_irq();
            }
            if (badge & IRQ_BADGE_TIMER) {
                int ret = timer_interrupt();
                if (ret != CLOCK_R_OK) {
                    //What now?
                }
            }
        }else if(label == seL4_VMFault){
            /* Page fault */
            handle_pagefault();

        }else if(label == seL4_NoFault) {
            /* System call */
            handle_syscall(badge, seL4_MessageInfo_get_length(message) - 1);

        }else{
            printf("Rootserver got an unknown message\n");
        }
    }
}


static void print_bootinfo(const seL4_BootInfo* info) {
    int i;

    /* General info */
    dprintf(1, "Info Page:  %p\n", info);
    dprintf(1,"IPC Buffer: %p\n", info->ipcBuffer);
    dprintf(1,"Node ID: %d (of %d)\n",info->nodeID, info->numNodes);
    dprintf(1,"IOPT levels: %d\n",info->numIOPTLevels);
    dprintf(1,"Init cnode size bits: %d\n", info->initThreadCNodeSizeBits);

    /* Cap details */
    dprintf(1,"\nCap details:\n");
    dprintf(1,"Type              Start      End\n");
    dprintf(1,"Empty             0x%08x 0x%08x\n", info->empty.start, info->empty.end);
    dprintf(1,"Shared frames     0x%08x 0x%08x\n", info->sharedFrames.start,
                                                   info->sharedFrames.end);
    dprintf(1,"User image frames 0x%08x 0x%08x\n", info->userImageFrames.start,
                                                   info->userImageFrames.end);
    dprintf(1,"User image PTs    0x%08x 0x%08x\n", info->userImagePTs.start,
                                                   info->userImagePTs.end);
    dprintf(1,"Untypeds          0x%08x 0x%08x\n", info->untyped.start, info->untyped.end);

    /* Untyped details */
    dprintf(1,"\nUntyped details:\n");
    dprintf(1,"Untyped Slot       Paddr      Bits\n");
    for (i = 0; i < info->untyped.end-info->untyped.start; i++) {
        dprintf(1,"%3d     0x%08x 0x%08x %d\n", i, info->untyped.start + i,
                                                   info->untypedPaddrList[i],
                                                   info->untypedSizeBitsList[i]);
    }

    /* Device untyped details */
    dprintf(1,"\nDevice untyped details:\n");
    dprintf(1,"Untyped Slot       Paddr      Bits\n");
    for (i = 0; i < info->deviceUntyped.end-info->deviceUntyped.start; i++) {
        dprintf(1,"%3d     0x%08x 0x%08x %d\n", i, info->deviceUntyped.start + i,
                                                   info->untypedPaddrList[i + (info->untyped.end - info->untyped.start)],
                                                   info->untypedSizeBitsList[i + (info->untyped.end-info->untyped.start)]);
    }

    dprintf(1,"-----------------------------------------\n\n");

    /* Print cpio data */
    dprintf(1,"Parsing cpio data:\n");
    dprintf(1,"--------------------------------------------------------\n");
    dprintf(1,"| index |        name      |  address   | size (bytes) |\n");
    dprintf(1,"|------------------------------------------------------|\n");
    for(i = 0;; i++) {
        unsigned long size;
        const char *name;
        void *data;

        data = cpio_get_entry(_cpio_archive, i, &name, &size);
        if(data != NULL){
            dprintf(1,"| %3d   | %16s | %p | %12d |\n", i, name, data, size);
        }else{
            break;
        }
    }
    dprintf(1,"--------------------------------------------------------\n");
}


typedef struct{
    char* elf_base;
} start_first_process_cont_t;


void start_first_process_part3(void* token, int err){
    printf("start first process part3\n");
    conditional_panic(err, "Failed to load elf image");
    start_first_process_cont_t* cont = (start_first_process_cont_t*)token;

    /* set up the stack & the heap */
    as_define_stack(tty_test_process.as, PROCESS_STACK_TOP, PROCESS_STACK_SIZE);
    conditional_panic(tty_test_process.as->as_stack == NULL, "Heap failed to be defined");
    as_define_heap(tty_test_process.as);
    conditional_panic(tty_test_process.as->as_heap == NULL, "Heap failed to be defined");

    /* Map in the IPC buffer for the thread */
    err = map_page(tty_test_process.ipc_buffer_cap, tty_test_process.vroot,
                   PROCESS_IPC_BUFFER,
                   seL4_AllRights, seL4_ARM_Default_VMAttributes);
    conditional_panic(err, "Unable to map IPC buffer for user app");

    /* Initialise filetable for this process */
    err = filetable_init(NULL, NULL, NULL);
    conditional_panic(err, "Unable to initialise filetable for user app");

    /* Start the new process */
    seL4_UserContext context;

    memset(&context, 0, sizeof(context));
    context.pc = elf_getEntryPoint(cont->elf_base);
    context.sp = PROCESS_STACK_TOP;
    seL4_TCB_WriteRegisters(tty_test_process.tcb_cap, 1, 0, 2, &context);

    /* Clear */
    free(cont);
}

void start_first_process_part2(void* token, addrspace_t *as){
    printf("start first process part2\n");
    conditional_panic(as==NULL, "start_first_process_part2: as is NULL\n");
    conditional_panic(as->as_pd_caps == NULL || as->as_pd_regs == NULL,
                      "start_first_process_part2: as contains invalid fields\n");

    start_first_process_cont_t* cont = (start_first_process_cont_t*)token;

    tty_test_process.as = as;
    conditional_panic(tty_test_process.as == NULL, "Failed to initialise address space");

    /* load the elf image */
    elf_load(tty_test_process.as, cont->elf_base, start_first_process_part3, token);
}

void start_first_process(char* app_name, seL4_CPtr fault_ep) {
    printf("start first process\n");
    int err;

    //seL4_Word stack_addr;
    //seL4_CPtr stack_cap;
    seL4_CPtr user_ep_cap;

    /* These required for loading program sections */
    char* elf_base;
    unsigned long elf_size;

    /* Create a VSpace */
    tty_test_process.vroot_addr = ut_alloc(seL4_PageDirBits);
    conditional_panic(!tty_test_process.vroot_addr,
                      "No memory for new Page Directory");
    err = cspace_ut_retype_addr(tty_test_process.vroot_addr,
                                seL4_ARM_PageDirectoryObject,
                                seL4_PageDirBits,
                                cur_cspace,
                                &tty_test_process.vroot);
    conditional_panic(err, "Failed to allocate page directory cap for client");

    /* Create a simple 1 level CSpace */
    tty_test_process.croot = cspace_create(1);
    assert(tty_test_process.croot != NULL);

    /* Create an IPC buffer */
    tty_test_process.ipc_buffer_addr = ut_alloc(seL4_PageBits);
    conditional_panic(!tty_test_process.ipc_buffer_addr, "No memory for ipc buffer");
    err =  cspace_ut_retype_addr(tty_test_process.ipc_buffer_addr,
                                 seL4_ARM_SmallPageObject,
                                 seL4_PageBits,
                                 cur_cspace,
                                 &tty_test_process.ipc_buffer_cap);
    conditional_panic(err, "Unable to allocate page for IPC buffer");

    /* Copy the fault endpoint to the user app to enable IPC */
    user_ep_cap = cspace_mint_cap(tty_test_process.croot,
                                  cur_cspace,
                                  fault_ep,
                                  seL4_AllRights,
                                  seL4_CapData_Badge_new(TTY_EP_BADGE));
    /* should be the first slot in the space, hack I know */
    assert(user_ep_cap == 1);
    assert(user_ep_cap == USER_EP_CAP);

    /* Create a new TCB object */
    tty_test_process.tcb_addr = ut_alloc(seL4_TCBBits);
    conditional_panic(!tty_test_process.tcb_addr, "No memory for new TCB");
    err =  cspace_ut_retype_addr(tty_test_process.tcb_addr,
                                 seL4_TCBObject,
                                 seL4_TCBBits,
                                 cur_cspace,
                                 &tty_test_process.tcb_cap);
    conditional_panic(err, "Failed to create TCB");

    /* Configure the TCB */
    err = seL4_TCB_Configure(tty_test_process.tcb_cap, user_ep_cap, TTY_PRIORITY,
                             tty_test_process.croot->root_cnode, seL4_NilData,
                             tty_test_process.vroot, seL4_NilData, PROCESS_IPC_BUFFER,
                             tty_test_process.ipc_buffer_cap);
    conditional_panic(err, "Unable to configure new TCB");


    /* parse the cpio image */
    dprintf(1, "\nStarting \"%s\"...\n", app_name);
    elf_base = cpio_get_file(_cpio_archive, app_name, &elf_size);
    conditional_panic(!elf_base, "Unable to locate cpio header");

    /* Initialise the continuation preparing to call as_create */
    start_first_process_cont_t* cont = malloc(sizeof(start_first_process_cont_t));
    conditional_panic(cont == NULL, "Unable to allocate memory for first process");

    cont->elf_base = elf_base;

    /* initialise address space */
    as_create(tty_test_process.vroot, start_first_process_part2, (void*)cont);
}

static void _sos_ipc_init(seL4_CPtr* ipc_ep, seL4_CPtr* async_ep){
    seL4_Word ep_addr, aep_addr;
    int err;

    /* Create an Async endpoint for interrupts */
    aep_addr = ut_alloc(seL4_EndpointBits);
    conditional_panic(!aep_addr, "No memory for async endpoint");
    err = cspace_ut_retype_addr(aep_addr,
                                seL4_AsyncEndpointObject,
                                seL4_EndpointBits,
                                cur_cspace,
                                async_ep);
    conditional_panic(err, "Failed to allocate c-slot for Interrupt endpoint");

    /* Bind the Async endpoint to our TCB */
    err = seL4_TCB_BindAEP(seL4_CapInitThreadTCB, *async_ep);
    conditional_panic(err, "Failed to bind ASync EP to TCB");


    /* Create an endpoint for user application IPC */
    ep_addr = ut_alloc(seL4_EndpointBits);
    conditional_panic(!ep_addr, "No memory for endpoint");
    err = cspace_ut_retype_addr(ep_addr,
                                seL4_EndpointObject,
                                seL4_EndpointBits,
                                cur_cspace,
                                ipc_ep);
    conditional_panic(err, "Failed to allocate c-slot for IPC endpoint");
}


static void _sos_init(seL4_CPtr* ipc_ep, seL4_CPtr* async_ep){
    seL4_Word dma_addr;
    seL4_Word low, high;
    int err;

    /* Retrieve boot info from seL4 */
    _boot_info = seL4_GetBootInfo();
    conditional_panic(!_boot_info, "Failed to retrieve boot info\n");
    if(verbose > 0){
        print_bootinfo(_boot_info);
    }

    /* Initialise the untyped sub system and reserve memory for DMA */
    err = ut_table_init(_boot_info);
    conditional_panic(err, "Failed to initialise Untyped Table\n");
    /* DMA uses a large amount of memory that will never be freed */
    dma_addr = ut_steal_mem(DMA_SIZE_BITS);
    conditional_panic(dma_addr == 0, "Failed to reserve DMA memory\n");

    /* find available memory */
    ut_find_memory(&low, &high);

    /* Initialise the untyped memory allocator */
    ut_allocator_init(low, high);

    /* Initialise the cspace manager */
    err = cspace_root_task_bootstrap(ut_alloc, ut_free, ut_translate,
                                     malloc, free);
    conditional_panic(err, "Failed to initialise the c space\n");

    /* Initialise DMA memory */
    err = dma_init(dma_addr, DMA_SIZE_BITS);
    conditional_panic(err, "Failed to intiialise DMA memory\n");

    /* Initialise IPC */
    _sos_ipc_init(ipc_ep, async_ep);

    /* Initialise frame table */
    err = frame_init();
    conditional_panic(err, "Failed to initialise frame table\n");
}

#define TEST_1      1
#define TEST_2      2
#define TEST_3      4

#define TEST_N_FRAMES 10
int ftc1, ftc2, ftc3;
static void
ft_test_1(void* token, seL4_Word kvaddr) {
    (void)token;
    int err;
    assert(kvaddr);
    printf("ft_test_1: kvaddr[%d] = 0x%08x\n", ftc1, kvaddr);

    ftc1++;

    /* Test you can touch the page */
    *(int*)kvaddr = 0x37;
    assert(*(int*)kvaddr == 0x37);

    if (ftc1 < TEST_N_FRAMES) {
        err = frame_alloc(0, NULL, true, ft_test_1, NULL);
        assert(!err);
    } else {
        printf("ft_test_1: Done!!!\n");
    }
}

static void
ft_test_2(void* token, seL4_Word kvaddr) {
    (void)token;
    int err;
    printf("ft_test_2: kvaddr[%d] = 0x%08x\n", ftc2, kvaddr);
    assert(kvaddr);
    ftc2++;

    /* Test you can touch the page */
    int val = rand();
    *(int*)kvaddr = val;
    assert(*(int*)kvaddr == val);

    err = frame_alloc(0,NULL,true,ft_test_2, NULL);
    assert(!err);
}

static void
ft_test_3(void* token, seL4_Word kvaddr) {
    (void)token;
    int err;
    printf("ft_test_3: kvaddr[%d] = 0x%08x\n", ftc3, kvaddr);
    assert(kvaddr);

    /* Test you can touch the page */
    int val = rand();
    *(int*)kvaddr = val;
    assert(*(int*)kvaddr == val);
    frame_free(kvaddr);

    err = frame_alloc(0,NULL,true,ft_test_3, NULL);
    assert(!err);
}

static void
frametable_test(uint32_t test_mask) {
    int err;
    //srand(0); this will eventually cause ft_test 2 to try
    //and access invalid kvaddr srand(1) will last longer before
    //hitting kvaddr == 0,(which is invalid)
    srand(1);
    if (test_mask & TEST_1) {
        printf("Starting test 1...\n");
        printf("Allocate %d frames and touch them\n", TEST_N_FRAMES);
        ftc1 = 0;
        err = frame_alloc(0,NULL,true,ft_test_1, NULL);
        assert(!err);
    }
    if (test_mask & TEST_2) {
        printf("Starting test 2...\n");
        printf("Test that frame_alloc runs out of memory after a while\n");
        ftc2 = 0;
        err = frame_alloc(0,NULL,true,ft_test_2, NULL);
        assert(!err);
    }
    if (test_mask & TEST_3) {
        printf("Starting test 3...\n");
        printf("Test that you never run out of memory if you always free frames.\n");
        ftc3 = 0;
        err = frame_alloc(0,NULL,true,ft_test_3, NULL);
        assert(!err);
    }
}

/*
 * This function need to be called after network_init
 * As it requires nfs to be mounted
 */
static void
filesystem_init(void) {
    int err;

    struct vnode* vn = malloc(sizeof(struct vnode));
    conditional_panic(vn == NULL, "Failed to allocate mountpoint vnode memory\n");

    vn->vn_name = (char*)malloc(strlen(ROOT_PATH));
    conditional_panic(vn->vn_name == NULL, "Failed to allocate mountpoint vnode memory\n");
    strcpy(vn->vn_name, ROOT_PATH);

    vn->vn_ops = malloc(sizeof(struct vnode_ops));
    conditional_panic(vn->vn_ops == NULL, "Failed to allocate mountpoint vnode memory\n");

    err = nfs_dev_init_mntpoint_vnode(vn, &mnt_point);
    conditional_panic(err, "Failed to initialise mountpoint vnode\n");

    vn->vn_opencount = 1;

    err = vfs_vnt_insert(vn);
    conditional_panic(err, "Failed to insert mountpoint vnode to vnode table\n");

    /* Setup the timeout for NFS */
    nfs_dev_setup_timeout();
}

static inline seL4_CPtr badge_irq_ep(seL4_CPtr ep, seL4_Word badge) {
    seL4_CPtr badged_cap = cspace_mint_cap(cur_cspace, cur_cspace, ep, seL4_AllRights, seL4_CapData_Badge_new(badge | IRQ_EP_BADGE));
    conditional_panic(!badged_cap, "Failed to allocate badged cap");
    return badged_cap;
}

//typedef struct {
//    seL4_Word kvaddr;
//    int free_slot;
//} swap_test_cont;
//
//void swap_test3(void* token, int err){
//    printf("swapping test 3\n");
//    swap_test_cont *cont = (swap_test_cont*)token;
//    printf("%s\n",(char*)(cont->kvaddr));
//}
//
//void swap_test2_swap_out(void *token, int err){
//    printf("swapin going to swap something out again\n");
//    swap_test_cont *cont = (swap_test_cont*)token;
//    printf("%s\n",(char*)(cont->kvaddr));
//
//    /* swapout */
//    swap_test_cont *cont3 = malloc(sizeof(swap_test_cont));
//    char* p_stuff3 = (char*)frame_alloc();
//    cont3->kvaddr = (seL4_Word)p_stuff3;
//    cont3->free_slot = 0; // should be 0
//    printf("swap total loops = %d\n",PAGE_SIZE/sizeof(char));
//    for(int i = 0; i < PAGE_SIZE/sizeof(char); i++) {
//        //printf("swap loop at = %d\n", i);
//        p_stuff3[i] = 'c';
//    }
//    //asd
//    seL4_Word dummy_vaddr = 0xdeadbeef;
//    p_stuff3[PAGE_SIZE-1] = '\0';
//    printf("string = %s\n", p_stuff3);
//    swap_out((seL4_Word)p_stuff3, dummy_vaddr, swap_test3, (void*)cont3);
//}
//
//void swap_test2_out_again(void *token, int err){
//    /* swapin */
//    printf("swapping test 2\n");
//    addrspace_t *as = proc_getas();
//    swap_test_cont *cont = (swap_test_cont*)token;
//    //region_t* reg = _region_probe(as, token->kvaddr);
//    //0x07 == allrights
//
//    //TODO fix this, fake vaddr, this is a hack
//    seL4_Word vaddr = (cont->free_slot)<<2;
//    printf("swap_test2 kvaddr = %p\n",(void*)cont->kvaddr);
//    swap_in(as, 0x07 , vaddr, cont->kvaddr, swap_test2_swap_out, token);
//}
//
//void swap_test2(void *token, int err){
//    /* swapin */
//    printf("swapping test 2\n");
//    addrspace_t *as = proc_getas();
//    swap_test_cont *cont = (swap_test_cont*)token;
//    //region_t* reg = _region_probe(as, token->kvaddr);
//    //0x07 == allrights
//
//    //TODO fix this, fake vaddr, this is a hack
//    seL4_Word vaddr = (cont->free_slot)<<2;
//    char * p_stuff = (char*)cont->kvaddr;
//    for(int i = 0; i < PAGE_SIZE/sizeof(char); i++) {
//        //printf("swap loop at = %d\n", i);
//        p_stuff[i] = '0';
//    }
//    p_stuff[PAGE_SIZE-1] = '\0';
//    printf("string = %s\n", p_stuff);
//    printf("swap_test2 kvaddr = %p\n",(void*)cont->kvaddr);
//    swap_in(as, 0x07 , vaddr, cont->kvaddr, swap_test3, token);
//}
//void swap_test(uint32_t id, void *data){
//    printf("swap test\n");
//    seL4_Word dummy_vaddr = 0xdeadbeef;
//
//    swap_test_cont *cont1 = malloc(sizeof(swap_test_cont));
//    char* p_stuff = //(char*)frame_alloc();
//    cont1->kvaddr = (seL4_Word)p_stuff;
//    cont1->free_slot = 0; // should be 0
//    printf("swap total loops = %d\n",PAGE_SIZE/sizeof(char));
//    for(int i = 0; i < PAGE_SIZE/sizeof(char); i++) {
//        //printf("swap loop at = %d\n", i);
//        p_stuff[i] = 'a';
//    }
//    p_stuff[PAGE_SIZE-1] = '\0';
//    printf("string = %s\n", p_stuff);
//    swap_out((seL4_Word)p_stuff, dummy_vaddr, swap_test2, (void*)cont1);
//
//    swap_test_cont *cont2 = malloc(sizeof(swap_test_cont));
//    char* p_stuff2 = (char*)frame_alloc();
//    cont2->kvaddr = (seL4_Word)p_stuff2;
//    cont2->free_slot = 1; // should be 1
//    printf("swap total loops = %d\n",PAGE_SIZE/sizeof(char));
//    for(int i = 0; i < PAGE_SIZE/sizeof(char); i++) {
//        //printf("swap loop at = %d\n", i);
//        p_stuff2[i] = 'b';
//    }
//    p_stuff2[PAGE_SIZE-1] = '\0';
//    printf("string = %s\n", p_stuff2);
//    swap_out((seL4_Word)p_stuff2, dummy_vaddr, swap_test2, (void*)cont2);
//}

/*
 * Main entry point - called by crt.
 */
int main(void) {
    int result;

    dprintf(0, "\nSOS Starting...\n");

    _sos_init(&_sos_ipc_ep_cap, &_sos_interrupt_ep_cap);

    /* Initialise the network hardware */
    network_init(badge_irq_ep(_sos_interrupt_ep_cap, IRQ_BADGE_NETWORK));


    /* Initialize timer driver */
    result = start_timer(badge_irq_ep(_sos_interrupt_ep_cap, IRQ_BADGE_TIMER));
    conditional_panic(result != CLOCK_R_OK, "Failed to initialize timer\n");

    /* Init file system */
    filesystem_init();
    frametable_test(TEST_1 | TEST_2);

    ///* Register swap test */
    //register_timer(1000000, swap_test, NULL); //100ms

    /* Start the user application */
    //start_first_process(TTY_NAME, _sos_ipc_ep_cap);

    ///* Wait on synchronous endpoint for IPC */
    dprintf(0, "\nSOS entering syscall loop\n");
    syscall_loop(_sos_ipc_ep_cap);

    /* Not reached */
    return 0;
}
