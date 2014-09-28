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

//typedef struct {
//    seL4_CPtr reply;
//    addrspace_t *as;
//    seL4_CapRights rights;
//    seL4_Word vaddr;
//    seL4_Word kvaddr;
//} VMF_cont_t;
//
//void sos_VMFaultHandler_end(uintptr_t token, int err){
//    //reply
//
//    VMF_cont_t *state = (VMF_cont_t*)token;
//
//    if (err) {
//        /* SOS doesn't handle the fault, the process is doing something
//         * wrong, kill it! */
//        // Just not replying to it for now
//        printf("Process is (pretend to be) killed\n");
//    } else {
//        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 0);
//        seL4_Send(state->reply, reply);
//    }
//
//    /* Free the saved reply cap */
//    cspace_free_slot(cur_cspace, state->reply);
//    free(state);
//}
//
//void sos_VMFaultHandler_swap_in_end(uintptr_t token, int err){
//    sos_VMFaultHandler_end(token, err);
//}
//
//
//void sos_VMFaultHandler_swap_out_end(uintptr_t token){
//    VMF_cont_t *state = (VMF_cont_t*)token;
//    region_t* reg = _region_probe(state->as, state->vaddr);
//    swap_in(state->as, reg->rights, state->vaddr, state->kvaddr, sos_VMFaultHandler_swap_in_end, (void*)token);
//}
//int
//sos_VMFaultHandler(seL4_CPtr reply, seL4_Word fault_addr, seL4_Word fsr){
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
//    VMF_cont_t *token = malloc(sizeof(VMF_cont_t));
//    token->reply = reply;
//
//    if (sos_page_is_mapped(as, fault_addr)) {
//        /* Check if page is swapped in */
//        if(sos_page_is_swapped(as, fault_addr)){
//            /* Swapped it back in */
//            seL4_Word kvaddr = get_free_frame_kvaddr();
//            if(kvaddr == -1){//not enough_memory){
//                //swap_out(sos_VMFaultHandler_swap_out_end);
//            } else {
//              region_t* reg = _region_probe(as, fault_addr);
//              token = malloc(sizeof(token));
//              seL4_Word kvaddr = 1;//find_free_frame();
//              swap_in(as, reg->rights, fault_addr,kvaddr,sos_VMFaultHandler_swap_in_end,token);
//            }
//        } else {
//            /* This must be a readonly fault */
//            return EACCES;
//        }
//    } else {
//        int err = 0;
//        bool fault_when_write = (bool)(fsr & RW_BIT);
//        bool fault_when_read = !fault_when_write;
//
//        /* Check if the fault address is in a valid region */
//        region_t* reg = _region_probe(as, fault_addr);
//        if(reg != NULL){
//            if (fault_when_write && !(reg->rights & seL4_CanWrite)) {
//                return EACCES;
//            }
//            if (fault_when_read && !(reg->rights & seL4_CanRead)) {
//                return EACCES;
//            }
//            err = sos_page_map(as, fault_addr, reg->rights);
//            if (err) {
//                return err;
//            }
//
//            return 0;
//        }
//        sos_VMFaultHandler_end((uintptr_t)token, err);
//    }
//
//    return EFAULT;
//}

int
sos_VMFaultHandler(seL4_Word fault_addr, seL4_Word fsr){
    if (fault_addr == 0) {
        /* Derefenrecing NULL? */
        return EINVAL;
    }

    addrspace_t *as = proc_getas();
    if (as == NULL) {
        /* Kernel is probably failed when bootstraping */
        return EFAULT;
    }

    if (sos_page_is_mapped(as, fault_addr)) {
        /* This must be a readonly fault */
        return EACCES;
    }

    int err;
    bool fault_when_write = (bool)(fsr & RW_BIT);
    bool fault_when_read = !fault_when_write;

    /* Check if the fault address is in a valid region */
    region_t* reg = _region_probe(as, fault_addr);
    if(reg != NULL){
        if (fault_when_write && !(reg->rights & seL4_CanWrite)) {
            return EACCES;
        }
        if (fault_when_read && !(reg->rights & seL4_CanRead)) {
            return EACCES;
        }
        err = sos_page_map(as, fault_addr, reg->rights);
        if (err) {
            return err;
        }

        return 0;
    }

    return EFAULT;
}
