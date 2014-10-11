#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <limits.h>
#include <sel4/sel4.h>
#include <cspace/cspace.h>
#include <ut_manager/ut.h>
#include <strings.h>
#include <errno.h>

#include "tool/utility.h"
#include "vm/vm.h"
#include "vm/mapping.h"
#include "vm/vmem_layout.h"
#include "vm/addrspace.h"
#include "vm/swap.h"
#include "proc/proc.h"

#define NFRAMES                  (FRAME_MEMORY / PAGE_SIZE)
#define ID_TO_VADDR(id)     ((id)*PAGE_SIZE + FRAME_VSTART)

#define INDEX_1_MASK        (0xffc00000)
#define INDEX_2_MASK        (0x003ff000)
#define PT_L1_INDEX(a)      (((a) & INDEX_1_MASK) >> 22)
#define PT_L2_INDEX(a)      (((a) & INDEX_2_MASK) >> 12)

#define RW_BIT    (1<<11)
static
region_t*
_region_probe(struct addrspace* as, seL4_Word addr) {
    assert(as != NULL);
    assert(addr != 0);

    if(as->as_stack != NULL && as->as_stack->vbase <= addr && addr < as->as_stack->vtop)
        return as->as_stack;

    if(as->as_heap != NULL && as->as_heap->vbase <= addr && addr < as->as_heap->vtop)
        return as->as_heap;

    for (region_t *r = as->as_rhead; r != NULL; r = r->next) {
        if (r->vbase <= addr && addr < r->vtop) {
            return r;
        }
    }
    return NULL;
}

typedef struct {
    seL4_CPtr reply_cap;
    addrspace_t *as;
    seL4_Word vaddr;
    seL4_Word kvaddr;
    region_t* reg;
} VMF_cont_t;

//static seL4_Word
//rand_chance_swap(){
//    int id = rand() % NFRAMES;
//    return ID_TO_VADDR(id);
//}

static void
sos_VMFaultHandler_reply(void* token, int err){
    printf("sos_vmf_end\n");

    VMF_cont_t *state = (VMF_cont_t*)token;
    if(err){
        printf("sos_vmf received an error\n");
        if (state->kvaddr != 0) {
            frame_free(state->kvaddr);
        }
    }

    /* If there is an err here, it is not the process's fault
     * It is either the kernel running out of memory or swapping doesn't work
     */
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 0);
    seL4_Send(state->reply_cap, reply);
    cspace_free_slot(cur_cspace, state->reply_cap);
    free(state);
}

static void
sos_VMFaultHandler_swap_in_end(void* token, int err){
    printf("sos_vmf_swapout to swapin, swap in ends\n");
    sos_VMFaultHandler_reply(token, err);
}

//static void
//sos_VMFaultHandler_swapout_to_swapin(void* token, int err){
//    printf("sos_vmf_swapout to swapin\n");
//    VMF_cont_t *state = (VMF_cont_t*)token;
//    region_t* reg = _region_probe(state->as, state->vaddr);
//    err = swap_in(state->as, reg->rights, state->vaddr, state->kvaddr, sos_VMFaultHandler_swap_in_end, token);
//    if(err) {
//        //something
//    }
//}

//static void
//sos_VMFaultHandler_swapout_to_pagein(void* token, int err){
//    printf("sos_vmf_swapout to pagein\n");
//    VMF_cont_t *state = (VMF_cont_t*)token;
//    printf("sos_vmf_swapout to pagein1\n");
//    region_t* reg = _region_probe(state->as, state->vaddr);
//    printf("sos_vmf_swapout to pagein2\n");
//    //this is hacky
//    //frame_free(state->kvaddr);
//
//    err = sos_page_map(state->as, state->vaddr, reg->rights);
//    printf("sos_vmf_swapout to pagein3 err = %d\n", err);
//
//    sos_VMFaultHandler_reply(token, 0);
//}

static void
sos_VMFaultHandler_swap_in_1(void *token, seL4_Word kvaddr) {
    if (kvaddr == 0) {
        sos_VMFaultHandler_reply(token, EFAULT);
        return;
    }

    VMF_cont_t *cont = (VMF_cont_t*)token;
    cont->kvaddr = kvaddr;
    int err = swap_in(cont->as, cont->reg->rights, cont->vaddr, kvaddr,
                      sos_VMFaultHandler_swap_in_end, cont);
    if (err) {
        sos_VMFaultHandler_reply((void*)cont, EFAULT);
        return;
    }
}

void
sos_VMFaultHandler(seL4_CPtr reply_cap, seL4_Word fault_addr, seL4_Word fsr){
    int err = 0;

    printf("sos vmfault handler \n");
    if (fault_addr == 0) {
        /* Derefenrecing NULL? Segfault */
        printf("App tried to derefence NULL, kill it\n");
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }

    addrspace_t *as = proc_getas();
    if (as == NULL) {
        printf("app as is NULL\n");
        /* Kernel is probably failed when bootstraping */
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }

    /* Check if this is a valid address */
    region_t* reg = _region_probe(as, fault_addr);
    if (reg == NULL) {
        /* Invalid address, segfault */
        printf("App tried to access a address without a region (invalid address), kill it\n");
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }

    /* Check for the permission */
    bool fault_when_write = (bool)(fsr & RW_BIT);
    bool fault_when_read = !fault_when_write;

    if (fault_when_write && !(reg->rights & seL4_CanWrite)) {
        printf("write to read only\n");
        /* Write to a read-only memory, segfault */
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }

    if (fault_when_read && !(reg->rights & seL4_CanRead)) {
        printf("read from no-readble mem\n");
        /* Read from a non-readable memory, segfault */
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }

    /*
     * If it comes here, this must be a valid address Therefore, this page is
     * either swapped out has never been mapped in
     */

    VMF_cont_t *cont = malloc(sizeof(VMF_cont_t));
    if (cont == NULL) {
        printf("vmfault out of mem\n");
        /* We cannot handle the fault but the process still can run
         * There will be more faults coming though */
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 0);
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }

    cont->reply_cap = reply_cap;
    cont->as = as;
    cont->kvaddr = 0;
    cont->vaddr = fault_addr;
    cont->reg = reg;

    /* Check if this page is an new, unmaped page or is it just swapped out */
    if (sos_page_is_inuse(as, fault_addr)) {
        if (sos_page_is_swapped(as, fault_addr)) {
            printf("vmf tries to swapin\n");
            /* This page is swapped out, we need to swap it back in */
            err = frame_alloc(PAGE_ALIGN(fault_addr), as, false, sos_VMFaultHandler_swap_in_1, (void*)cont);
            if (err) {
                sos_VMFaultHandler_reply((void*)cont, err);
                return;
            }
            return;
        } else {
            //printf("vmf: process tries to access an inused, non-swapped page\n");
            //printf("most likely this page is not mapped in correctly\n");

            printf("vmf second chance mapping page back in\n");
            //our second chance swap this page out
            //simply map this page in
            //assert(); 
            seL4_CPtr kframe_cap, frame_cap;
            seL4_Word vpage = PAGE_ALIGN(fault_addr);
            int x = PT_L1_INDEX(vpage);
            int y = PT_L2_INDEX(vpage);
            //TODO: check as->as_pd_regs != NULL first
            seL4_Word kvaddr = (as->as_pd_regs[x][y] & PTE_KVADDR_MASK);
            printf("mapping back into kvaddr -> 0x%08x\n",kvaddr);
            err = frame_get_cap(kvaddr, &kframe_cap);
            assert(!err); // This kvaddr is ready to use, there should be no error

            /* Copy the frame cap as we need to map it into 2 address spaces */
            //TODO check if I got reg->rights correctly
            frame_cap = cspace_copy_cap(cur_cspace, cur_cspace, kframe_cap, reg->rights);
            if (frame_cap == CSPACE_NULL) {
                printf("vmf: failed copying frame cap\n");
                return;
            }

            //TODO check if I got reg->rights correctly
            err = seL4_ARM_Page_Map(frame_cap, as->as_sel4_pd, PAGE_ALIGN(vpage),
                                    reg->rights, seL4_ARM_Default_VMAttributes);
            if(err == seL4_FailedLookup){
                /* Assume the error was because we have no page table
                 * And at this point, page table should already be mapped in.
                 * So this is an error */
                printf("vmf: failed mapping application frame to sel4\n");
                return;
            }

            err = set_frame_referenced(kvaddr);
            if(err){
                printf("vmf: setting frame referenced error\n");
            }
            return;
        }
    } else {
        /* This page has never been mapped, so do that and return */
        printf("vmf tries to map a page\n");
        int err;
        err = sos_page_map(as, fault_addr, reg->rights,sos_VMFaultHandler_reply, (void*)cont, false);
        if(err){
            sos_VMFaultHandler_reply((void*)cont, err);
        }
        return;
    }
    /* Otherwise, this is not handled */
    printf("vmf error at the end\n");
    sos_VMFaultHandler_reply((void*)cont, EFAULT);
    return;
}
