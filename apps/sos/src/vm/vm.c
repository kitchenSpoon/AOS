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

#define verbose 0
#include <sys/debug.h>

#define NFRAMES                  (FRAME_MEMORY / PAGE_SIZE)
#define ID_TO_VADDR(id)     ((id)*PAGE_SIZE + FRAME_VSTART)

#define RW_BIT    (1<<11)

static bool
_check_segfault(addrspace_t *as, seL4_Word fault_addr, seL4_Word fsr, region_t **reg_ret) {
    if (fault_addr == 0) {
        /* Derefenrecing NULL? Segfault */
        dprintf(3, "App tried to derefence NULL\n");
        return true;
    }

    /* Check if this is a valid address */
    region_t* reg = region_probe(as, fault_addr);
    if (reg == NULL) {
        /* Invalid address, segfault */
        dprintf(3, "App tried to access a address without a region (invalid address), kill it\n");
        return true;
    }

    /* Check for the permission */
    bool fault_when_write = (bool)(fsr & RW_BIT);
    bool fault_when_read = !fault_when_write;

    if (fault_when_write && !(reg->rights & seL4_CanWrite)) {
        dprintf(3, "write to read only\n");
        return true;
    }

    if (fault_when_read && !(reg->rights & seL4_CanRead)) {
        dprintf(3, "read from no-readble mem\n");
        return true;
    }

    *reg_ret = reg;
    return false;
}

static int
_set_page_reference(addrspace_t *as, seL4_Word vaddr, uint32_t rights) {

    int err;

    seL4_CPtr kframe_cap, frame_cap;

    seL4_Word vpage = PAGE_ALIGN(vaddr);
    int x = PT_L1_INDEX(vpage);
    int y = PT_L2_INDEX(vpage);

    if (as->as_pd_regs[x] == NULL) {
        return EINVAL;
    }

    seL4_Word kvaddr = (as->as_pd_regs[x][y] & PTE_KVADDR_MASK);
    dprintf(3, "mapping back into kvaddr -> 0x%08x, vaddr = 0x%08x\n", kvaddr, vaddr);

    err = frame_get_cap(kvaddr, &kframe_cap);
    //assert(!err); // This kvaddr is ready to use, there should be no error

    /* Copy the frame cap as we need to map it into 2 address spaces */
    frame_cap = cspace_copy_cap(cur_cspace, cur_cspace, kframe_cap, rights);
    if (frame_cap == CSPACE_NULL) {
        dprintf(3, "vmf: failed copying frame cap\n");
        return EFAULT;
    }

    err = seL4_ARM_Page_Map(frame_cap, as->as_sel4_pd, vpage,
            rights, seL4_ARM_Default_VMAttributes);
    if(err == seL4_FailedLookup){
        dprintf(3, "vmf: failed mapping application frame to sel4\n");
        return EFAULT;
    }

    as->as_pd_caps[x][y] = frame_cap;

    err = frame_set_referenced(kvaddr);
    if(err){
        dprintf(3, "vmf: setting frame referenced error\n");
        return err;
    }

    return 0;
}

/**********************************************************************
 * VMFault handler code
 *********************************************************************/

typedef struct {
    seL4_CPtr reply_cap;
    addrspace_t *as;
    seL4_Word vaddr;
    bool is_code;
    region_t* reg;
    pid_t pid;
} VMF_cont_t;

static void _sos_VMFaultHandler_reply(void* token, int err);

void
sos_VMFaultHandler(seL4_CPtr reply_cap, seL4_Word fault_addr, seL4_Word fsr, bool is_code){
    dprintf(3, "sos vmfault handler \n");

    int err;

    dprintf(3, "sos vmfault handler, getting as \n");
    addrspace_t *as = proc_getas();
    dprintf(3, "sos vmfault handler, gotten as \n");
    if (as == NULL) {
        dprintf(3, "app as is NULL\n");
        /* Kernel is probably failed when bootstraping */
        set_cur_proc(PROC_NULL);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }

    region_t *reg;

    /* Is this a segfault? */
    if (_check_segfault(as, fault_addr, fsr, &reg)) {
        dprintf(3, "vmf: segfault\n");
        proc_destroy(proc_get_id());
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
        dprintf(3, "vmfault out of mem\n");
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
    cont->pid       = proc_get_id();

    /* Check if this page is an new, unmaped page or is it just swapped out */
    if (sos_page_is_inuse(as, fault_addr)) {
        if (sos_page_is_swapped(as, fault_addr)) {
            dprintf(3, "vmf tries to swapin\n");
            /* This page is swapped out, we need to swap it back in */
            err = swap_in(cont->as, cont->reg->rights, cont->vaddr,
                      cont->is_code, _sos_VMFaultHandler_reply, cont);
            if (err) {
                _sos_VMFaultHandler_reply((void*)cont, err);
                return;
            }
            return;
        } else {
            if (sos_page_is_locked(as, fault_addr)) {
                dprintf(3, "vmf page is locked\n");
                _sos_VMFaultHandler_reply((void*)cont, EFAULT);
                return;
            }

            dprintf(3, "vmf second chance mapping page back in\n");
            err = _set_page_reference(cont->as, cont->vaddr, cont->reg->rights);
            _sos_VMFaultHandler_reply((void*)cont, err);
            return;
        }
    } else {
        /* This page has never been mapped, so do that and return */
        dprintf(3, "vmf tries to map a page\n");
        inc_proc_size_proc(cur_proc());
        err = sos_page_map(proc_get_id(), as, fault_addr, reg->rights, _sos_VMFaultHandler_reply, (void*)cont, false);
        if(err){
            dec_proc_size_proc(cur_proc());
            _sos_VMFaultHandler_reply((void*)cont, err);
        }
        return;
    }
    /* Otherwise, this is not handled */
    dprintf(3, "vmf error at the end\n");
    _sos_VMFaultHandler_reply((void*)cont, EFAULT);
    return;
}

static void
_sos_VMFaultHandler_reply(void* token, int err){
    dprintf(3, "sos_vmf_reply called\n");

    VMF_cont_t *cont= (VMF_cont_t*)token;
    if(err){
        dprintf(3, "sos_vmf received an err\n");
    }
    if (!is_proc_alive(cont->pid)) {
        cspace_free_slot(cur_cspace, cont->reply_cap);
        free(cont);
        return;
    }

    /* Flush the i-cache. Ideally, this should only be done when faulting on text segment */
    //if (cont->is_code) {
    if (!err) {
        seL4_Word vpage = PAGE_ALIGN(cont->vaddr);
        int x = PT_L1_INDEX(vpage);
        int y = PT_L2_INDEX(vpage);

        seL4_Word kvaddr = (cont->as->as_pd_regs[x][y] & PTE_KVADDR_MASK);
        seL4_CPtr kframe_cap;

        err = frame_get_cap(kvaddr, &kframe_cap);
        //assert(!err); // This kvaddr is ready to use, there should be no error
        seL4_ARM_Page_Unify_Instruction(kframe_cap, 0, PAGESIZE);
    }
    //}
    /* If there is an err here, it is not the process's fault
     * It is either the kernel running out of memory or swapping doesn't work
     */
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 0);
    seL4_Send(cont->reply_cap, reply);
    cspace_free_slot(cur_cspace, cont->reply_cap);
    free(cont);
    set_cur_proc(PROC_NULL);
}

