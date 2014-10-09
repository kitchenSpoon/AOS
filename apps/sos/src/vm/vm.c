#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <limits.h>
#include <sel4/sel4.h>
#include <cspace/cspace.h>
#include <ut_manager/ut.h>
#include <strings.h>
#include <errno.h>

#include "vm/vm.h"
#include "vm/mapping.h"
#include "vm/vmem_layout.h"
#include "vm/addrspace.h"
#include "vm/swap.h"
#include "proc/proc.h"

#define NFRAMES                  (FRAME_MEMORY / PAGE_SIZE)
#define ID_TO_VADDR(id)     ((id)*PAGE_SIZE + FRAME_VSTART) 

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
    seL4_CapRights rights;
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
    if(err){
        printf("sos_vmf received an error\n");
    }

    VMF_cont_t *state = (VMF_cont_t*)token;

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
    VMF_cont_t *cont = (VMF_cont_t*)token;

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

    if (fault_addr == 0) {
        /* Derefenrecing NULL? Segfault */
        printf("App tried to derefence NULL, kill it\n");
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }

    addrspace_t *as = proc_getas();
    if (as == NULL) {
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
        /* Write to a read-only memory, segfault */
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }

    if (fault_when_read && !(reg->rights & seL4_CanRead)) {
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
        /* We cannot handle the fault but the process still can run
         * There will be more faults coming though */
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 0);
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }
    cont->reply_cap = reply_cap;
    cont->as = as;
    cont->vaddr = fault_addr;
    cont->reg = reg;

    /* Check if this page is an new, unmaped page or is it just swapped out */
    if (sos_page_is_mapped(as, fault_addr)) {
        if (sos_page_is_swapped(as, fault_addr)) {
            printf("vmf tries to swapin\n");
            /* This page is swapped out, we need to swap it back in */
            //seL4_Word kvaddr = frame_alloc();
            err = frame_alloc(sos_VMFaultHandler_swap_in_1, (void*)cont);
            if (err) {
                sos_VMFaultHandler_reply((void*)cont, err);
                return;
            }
            //swapin here
            return;
        } else {

            printf("vmf tries to access a mapped (but not swapped out) page again,\nmost likely app does not have certain rights to this address\n");
        }
    } else {
        /* This page has never been mapped, so do that and return */
        //TODO: this function will need to be broken down here
        printf("vmf tries to map a page\n");
        int err = sos_page_map(as, fault_addr, reg->rights,sos_VMFaultHandler_reply, (void*)cont);
        if(err){
            sos_VMFaultHandler_reply((void*)cont, err);
        }
        return;
    }
    /* Otherwise, this is not handled */
    printf("vmf error at the end\n");
    sos_VMFaultHandler_reply((void*)cont, EFAULT);
    return;
//    if (sos_page_is_mapped(as, fault_addr)) {
//        if(sos_page_is_swapped(as, fault_addr)){
//            /* Swapped it back in */
//            seL4_Word kvaddr = get_free_frame_kvaddr();
//            if(kvaddr == -1){//not enough_memory){
//                kvaddr = rand_chance_swap();
//                token->kvaddr = kvaddr;
//                swap_out(kvaddr, fault_addr, sos_VMFaultHandler_swapout_to_swapin, (void*)token);
//                return 0;
//            } else {
//              region_t* reg = _region_probe(as, fault_addr);
//              token = malloc(sizeof(token));
//              //kvaddr is 1 because of some random reason, fix this when this code is actually called
//              seL4_Word kvaddr = 1;//find_free_frame();
//              int err = swap_in(as, reg->rights, fault_addr,kvaddr,sos_VMFaultHandler_swap_in_end,token);
//              if(err){
//                //something
//                return err;
//              }
//              return 0;
//            }
//        } else {
//            /* This must be a readonly fault */
//            printf("vmf err1\n");
//            return EACCES;
//        }
//    } else {
//        int err = 0;
//
//        /* Check if the fault address is in a valid region */
//        region_t* reg = _region_probe(as, fault_addr);
//        if(reg != NULL){
//            if (fault_when_write && !(reg->rights & seL4_CanWrite)) {
//            printf("vmf err2\n");
//                return EACCES;
//            }
//            if (fault_when_read && !(reg->rights & seL4_CanRead)) {
//            printf("vmf err3\n");
//                return EACCES;
//            }
//            //TODO check if we have enough memory here
//            seL4_Word kvaddr = get_free_frame_kvaddr();
//            if(kvaddr == -1){//not enough_memory){
//                for(int i = 0; i < 1; i++){
//                    kvaddr = rand_chance_swap();
//
//                    token->kvaddr = kvaddr;
//                    printf("vm fault not enough memory, performing swapout now\n");
//                    swap_out(kvaddr , fault_addr, sos_VMFaultHandler_swapout_to_pagein, (void*)token);
//                    printf("vm fault swapout done\n");
//
//                }
//            } else {
//                err = sos_page_map(as, fault_addr, reg->rights);
//                if (err) {
//                    return err;
//                }
//                sos_VMFaultHandler_reply(token, 0);
//            }
//            return 0;
//        }
//        printf("vmf region is NULL\n");
//        sos_VMFaultHandler_reply(token, EFAULT);
//        return EFAULT;
//    }
//    return EFAULT;
}

//int
//sos_VMFaultHandler(seL4_Word fault_addr, seL4_Word fsr){
//    if (fault_addr == 0) {
//        /* Derefenrecing NULL? */
//        return EINVAL;
//    }
//
//    addrspace_t *as = proc_getas();
//    if (as == NULL) {
//        /* Kernel is probably failed when bootstraping */
//        return EFAULT;
//    }
//
//    if (sos_page_is_mapped(as, fault_addr)) {
//        /* This must be a readonly fault */
//        return EACCES;
//    }
//
//    int err;
//    bool fault_when_write = (bool)(fsr & RW_BIT);
//    bool fault_when_read = !fault_when_write;
//
//    /* Check if the fault address is in a valid region */
//    region_t* reg = _region_probe(as, fault_addr);
//    if(reg != NULL){
//        if (fault_when_write && !(reg->rights & seL4_CanWrite)) {
//            return EACCES;
//        }
//        if (fault_when_read && !(reg->rights & seL4_CanRead)) {
//            return EACCES;
//        }
//        err = sos_page_map(as, fault_addr, reg->rights);
//        if (err) {
//            return err;
//        }
//
//        return 0;
//    }
//
//    return EFAULT;
//}
