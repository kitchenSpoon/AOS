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

#define RW_BIT    (1<<11)

typedef struct {
    seL4_CPtr reply_cap;
    addrspace_t *as;
    seL4_Word vaddr;
    bool is_code;
    region_t* reg;
} VMF_cont_t;

static void
sos_VMFaultHandler_reply(void* token, int err){
    printf("sos_vmf_reply called\n");

    VMF_cont_t *state = (VMF_cont_t*)token;
    if(err){
        printf("sos_vmf received an error\n");
    }

    /* Flush the i-cache if this is an instruction fault */
    //if (state->is_code) {
        seL4_Word vpage = PAGE_ALIGN(state->vaddr);
        int x = PT_L1_INDEX(vpage);
        int y = PT_L2_INDEX(vpage);

        seL4_Word kvaddr = (state->as->as_pd_regs[x][y] & PTE_KVADDR_MASK);
        seL4_CPtr kframe_cap;

        err = frame_get_cap(kvaddr, &kframe_cap);
        assert(!err); // This kvaddr is ready to use, there should be no error
        seL4_ARM_Page_Unify_Instruction(kframe_cap, 0, PAGESIZE);
    //}
    /* If there is an err here, it is not the process's fault
     * It is either the kernel running out of memory or swapping doesn't work
     */
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 0);
    seL4_Send(state->reply_cap, reply);
    cspace_free_slot(cur_cspace, state->reply_cap);
    free(state);
    set_cur_proc(PROC_NULL);
}

static void
sos_VMFaultHandler_swap_in_end(void* token, int err){
    printf("sos_vmf_swap_in_end\n");
    sos_VMFaultHandler_reply(token, err);
}

static bool
_check_segfault(addrspace_t *as, seL4_Word fault_addr, seL4_Word fsr, region_t **reg_ret) {
    if (fault_addr == 0) {
        /* Derefenrecing NULL? Segfault */
        printf("App tried to derefence NULL, kill it\n");
        return true;
    }

    /* Check if this is a valid address */
    region_t* reg = region_probe(as, fault_addr);
    if (reg == NULL) {
        /* Invalid address, segfault */
        printf("App tried to access a address without a region (invalid address), kill it\n");
        return true;
    }

    /* Check for the permission */
    bool fault_when_write = (bool)(fsr & RW_BIT);
    bool fault_when_read = !fault_when_write;

    if (fault_when_write && !(reg->rights & seL4_CanWrite)) {
        printf("write to read only\n");
        return true;
    }

    if (fault_when_read && !(reg->rights & seL4_CanRead)) {
        printf("read from no-readble mem\n");
        return true;
    }

    *reg_ret = reg;
    return false;
}

static int
_set_page_reference(VMF_cont_t *cont) {

    int err;

    seL4_CPtr kframe_cap, frame_cap;

    seL4_Word vpage = PAGE_ALIGN(cont->vaddr);
    int x = PT_L1_INDEX(vpage);
    int y = PT_L2_INDEX(vpage);

    if (cont->as->as_pd_regs[x] == NULL) {
        return EINVAL;
    }

    seL4_Word kvaddr = (cont->as->as_pd_regs[x][y] & PTE_KVADDR_MASK);
    printf("mapping back into kvaddr -> 0x%08x, vaddr = 0x%08x\n", kvaddr, cont->vaddr);

    //printf("---test_data---\n");
    //char *kvaddr_str = (char*)kvaddr;
    //for (int i=0; i<PAGE_SIZE; i++) {
    //    printf("%c", kvaddr_str[i]);
    //}
    //printf("\n---test_data end---\n");


    err = frame_get_cap(kvaddr, &kframe_cap);
    assert(!err); // This kvaddr is ready to use, there should be no error

    /* Copy the frame cap as we need to map it into 2 address spaces */
    frame_cap = cspace_copy_cap(cur_cspace, cur_cspace, kframe_cap, cont->reg->rights);
    if (frame_cap == CSPACE_NULL) {
        printf("vmf: failed copying frame cap\n");
        return EFAULT;
    }

    err = seL4_ARM_Page_Map(frame_cap, cont->as->as_sel4_pd, vpage,
            cont->reg->rights, seL4_ARM_Default_VMAttributes);
    if(err == seL4_FailedLookup){
        printf("vmf: failed mapping application frame to sel4\n");
        return EFAULT;
    }

    cont->as->as_pd_caps[x][y] = frame_cap;

    err = set_frame_referenced(kvaddr);
    if(err){
        printf("vmf: setting frame referenced error\n");
        return err;
    }

    return 0;
}

void
sos_VMFaultHandler(seL4_CPtr reply_cap, seL4_Word fault_addr, seL4_Word fsr, bool is_code){
    printf("sos vmfault handler \n");

    int err;

    printf("sos vmfault handler, getting as \n");
    addrspace_t *as = proc_getas();
    printf("sos vmfault handler, gotten as \n");
    if (as == NULL) {
        printf("app as is NULL\n");
        /* Kernel is probably failed when bootstraping */
        set_cur_proc(PROC_NULL);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }

    region_t *reg;

    /* Is this a segfault? */
    if (_check_segfault(as, fault_addr, fsr, &reg)) {
        printf("vmf: segfault\n");
        set_cur_proc(PROC_NULL);
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
        set_cur_proc(PROC_NULL);
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 0);
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }

    cont->reply_cap = reply_cap;
    cont->as        = as;
    cont->vaddr     = fault_addr;
    cont->is_code   = is_code;
    cont->reg       = reg;

    /* Check if this page is an new, unmaped page or is it just swapped out */
    if (sos_page_is_inuse(as, fault_addr)) {
        if (sos_page_is_swapped(as, fault_addr)) {
            printf("vmf tries to swapin\n");
            /* This page is swapped out, we need to swap it back in */
            err = swap_in(cont->as, cont->reg->rights, cont->vaddr,
                      cont->is_code, sos_VMFaultHandler_swap_in_end, cont);
            if (err) {
                sos_VMFaultHandler_reply((void*)cont, err);
                return;
            }
            return;
        } else {
            printf("vmf second chance mapping page back in\n");
            err = _set_page_reference(cont);
            sos_VMFaultHandler_reply((void*)cont, err);
            return;
        }
    } else {
        /* This page has never been mapped, so do that and return */
        printf("vmf tries to map a page\n");
        inc_cur_proc_size();
        err = sos_page_map(as, fault_addr, reg->rights,sos_VMFaultHandler_reply, (void*)cont, false);
        if(err){
            dec_cur_proc_size();
            sos_VMFaultHandler_reply((void*)cont, err);
        }
        return;
    }
    /* Otherwise, this is not handled */
    printf("vmf error at the end\n");
    sos_VMFaultHandler_reply((void*)cont, EFAULT);
    return;
}
