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
#include "tool/utility.h"
#include <limits.h>

#include <autoconf.h>

#define verbose 5
#include <sys/debug.h>
#include <sys/panic.h>

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

#define ROOT_PATH           "/"

/* The linker will link this symbol to the start address  *
 * of an archive of attached applications.                */
extern char _cpio_archive[];

const seL4_BootInfo* _boot_info;

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
    case SOS_SYSCALL_PROC_CREATE:
    {
        char *path          = (char *)seL4_GetMR(1);
        size_t len          = (size_t)seL4_GetMR(2);
        serv_proc_create(path, len, _sos_ipc_ep_cap, reply_cap);
        break;
    }
    case SOS_SYSCALL_PROC_DESTROY:
    {
        pid_t pid             = (pid_t)seL4_GetMR(1);
        serv_proc_destroy(pid, reply_cap);
        break;
    }
    case SOS_SYSCALL_PROC_GET_ID:
    {
        serv_proc_get_id(reply_cap);
        break;
    }
    case SOS_SYSCALL_PROC_WAIT:
    {
        pid_t pid          = (pid_t)seL4_GetMR(1);
        serv_proc_wait(pid, reply_cap);
        break;
    }
    case SOS_SYSCALL_PROC_STATUS:
    {
        //char *app_name          = (char *)seL4_GetMR(1);
        //serv_proc_create(app_name, _sos_ipc_ep_cap, reply_cap);
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
    dprintf(0, "vm fault at 0x%08x, align = 0x%08x, pc = 0x%08x, proc = %d, %s\n",
            fault_addr, PAGE_ALIGN(fault_addr), pc, proc_get_id(), ifault ? "iFault" : "dFault");

    seL4_CPtr reply_cap;

    /* Save the caller */
    reply_cap = cspace_save_reply_cap(cur_cspace);
    assert(reply_cap != CSPACE_NULL);
    sos_VMFaultHandler(reply_cap, fault_addr, fsr, ifault);
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
            printf("user with pid = %d, 0x%08x is having a vmfault\n", badge & ~USER_EP_BADGE, badge);
            set_cur_proc(badge & ~USER_EP_BADGE);
            handle_pagefault();

        }else if(label == seL4_NoFault) {
            /* System call */
            printf("user with pid = %d, 0x%08x is having a vmfault\n", badge & ~USER_EP_BADGE, badge);
            set_cur_proc(badge & ~USER_EP_BADGE);
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
_filesystem_init(void) {
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


/*
 * Main entry point - called by crt.
 */

void main2(void* token, int err, pid_t id){
    dprintf(0, "\nSOS entering syscall loop\n");
    syscall_loop(_sos_ipc_ep_cap);
}

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
    _filesystem_init();
    //frametable_test(TEST_1 | TEST_2);

    /* Start the user application */
    //start_first_process(TTY_NAME, _sos_ipc_ep_cap);

    proc_list_init();
    proc_create(TTY_NAME, _sos_ipc_ep_cap, main2, NULL);

    return 0;

    dprintf(0, "\nSOS entering syscall loop\n");
    syscall_loop(_sos_ipc_ep_cap);

    /* Not reached */
    return 0;
}
