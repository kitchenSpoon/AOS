#include <sel4/sel4.h>
#include <stdio.h>
#include <limits.h>
#include <assert.h>
#include <string.h>
#include <strings.h>
#include <ut_manager/ut.h>

#include "vm/vm.h"
#include "vm/swap.h"
#include "vm/addrspace.h"
#include "vm/mapping.h"
#include "tool/utility.h"

#define verbose 0
#include <sys/debug.h>

#define STATUS_USED     0
#define STATUS_FREE     1

#define PAGEDIR_BITS        (12)
#define PAGETABLE_BITS      (12)
#define PAGETABLE_PAGES     (PAGE_SIZE >> 2)
/***********************************************************************
 * sos_page_map
 ***********************************************************************/

typedef struct {
    sos_page_map_cb_t callback;
    void *token;
    pid_t pid;
    addrspace_t* as;
    seL4_Word vpage;
    uint32_t permissions;
    bool noswap;
} sos_page_map_cont_t;

static void _sos_page_map_2_alloc_cap_pt(void* token, seL4_Word kvaddr);
static void _sos_page_map_3(void* token, seL4_Word kvaddr);
static void _sos_page_map_4_alloc_frame(void* token);
static void _sos_page_map_5(void* token, seL4_Word kvaddr);
static int _map_sel4_page(addrspace_t *as, seL4_CPtr frame_cap, seL4_Word vpage,
          seL4_CapRights rights, seL4_ARM_VMAttributes attr);

int
sos_page_map(pid_t pid, addrspace_t *as, seL4_Word vaddr, uint32_t permissions,
             sos_page_map_cb_t callback, void* token, bool noswap) {
    dprintf(3, "sos_page_map\n");
    if (as == NULL) {
        return EINVAL;
    }

    if (as->as_pd_caps == NULL || as->as_pd_regs == NULL) {
        /* Did you even call as_create? */
        dprintf(3, "sos_page_map err einval 0\n");
        return EFAULT;
    }

    seL4_Word vpage = PAGE_ALIGN(vaddr);

    sos_page_map_cont_t* cont = malloc(sizeof(sos_page_map_cont_t));
    if(cont == NULL){
        dprintf(3, "sos_page_map err nomem\n");
        return ENOMEM;
    }
    cont->pid = pid;
    cont->as = as;
    cont->vpage = vpage;
    cont->permissions = permissions;
    cont->callback = callback;
    cont->token = token;
    cont->noswap = noswap;

    int x, err;

    x = PT_L1_INDEX(vpage);

    if (as->as_pd_regs[x] == NULL) {
        /* Create pagetable if needed */

        assert(as->as_pd_caps[x] == NULL);

        /* Allocate memory for the 2nd level pagetable for regs */
        err = frame_alloc(0, NULL, PROC_NULL, true, _sos_page_map_2_alloc_cap_pt, (void*)cont);
        if (err) {
            free(cont);
            return EFAULT;
        }
        return 0;
    }

     _sos_page_map_4_alloc_frame((void*)cont);
    return 0;
}

static void
_sos_page_map_2_alloc_cap_pt(void* token, seL4_Word kvaddr){
    dprintf(3, "sos_page_map 2\n");
    sos_page_map_cont_t* cont = (sos_page_map_cont_t*)token;

    if (kvaddr == 0) {
        dprintf(3, "warning: _sos_page_map_2_alloc_cap_pt not enough memory for lvl2 pagetable\n");
        cont->callback(cont->token, ENOMEM);
        free(cont);
        return;
    }

    seL4_Word vpage = PAGE_ALIGN(cont->vpage);
    int x = PT_L1_INDEX(vpage);
    cont->as->as_pd_regs[x] = (pagetable_t)kvaddr;

    /* Allocate memory for the 2nd level pagetable for caps */
    int err = frame_alloc(0, NULL, PROC_NULL, true, _sos_page_map_3, token);
    if (err) {
        frame_free(kvaddr);
        cont->callback(cont->token, EFAULT);
        free(cont);
        return;
    }
}

static void
_sos_page_map_3(void* token, seL4_Word kvaddr){
    dprintf(3, "sos_page_map 3\n");
    sos_page_map_cont_t* cont = (sos_page_map_cont_t*)token;

    int x = PT_L1_INDEX(cont->vpage);
    if (kvaddr == 0) {
        dprintf(3, "warning: _sos_page_map_3 not enough memory for lvl2 pagetable\n");
        frame_free((seL4_Word)(cont->as->as_pd_regs[x]));
        cont->callback(cont->token, ENOMEM);
        free(cont);
        return;
    }

    cont->as->as_pd_caps[x] = (pagetable_t)kvaddr;

     _sos_page_map_4_alloc_frame(token);
}

static void
_sos_page_map_4_alloc_frame(void* token){
    dprintf(3, "sos_page_map 4\n");
    sos_page_map_cont_t* cont = (sos_page_map_cont_t*)token;

    int x = PT_L1_INDEX(cont->vpage);
    int y = PT_L2_INDEX(cont->vpage);

    if ((cont->as->as_pd_regs[x][y] & PTE_IN_USE_BIT) &&
            !(cont->as->as_pd_regs[x][y] & PTE_SWAPPED)) {
        /* page already mapped */
        cont->callback(cont->token, EINVAL);
        free(cont);
        return;
    }

    /* Allocate memory for the frame */
    int err = frame_alloc(cont->vpage, cont->as, cont->pid, cont->noswap, _sos_page_map_5, token);
    if (err) {
        dprintf(3, "_sos_page_map_4_alloc_frame: failed to allocate frame\n");
        cont->callback(cont->token, EINVAL);
        free(cont);
        return;
    }
}

static void
_sos_page_map_5(void* token, seL4_Word kvaddr){
    dprintf(3, "sos_page_map 5\n");

    sos_page_map_cont_t* cont = (sos_page_map_cont_t*)token;

    seL4_CPtr kframe_cap, frame_cap;

    if (!kvaddr) {
        dprintf(3, "_sos_page_map_5 failed to allocate memory for frame\n");
        cont->callback((void*)(cont->token), ENOMEM);
        free(cont);
        return;
    }

    int err = frame_get_cap(kvaddr, &kframe_cap);
    assert(!err); // There should be no error

    /* Copy the frame cap as we need to map it into 2 address spaces */
    frame_cap = cspace_copy_cap(cur_cspace, cur_cspace, kframe_cap, cont->permissions);
    if (frame_cap == CSPACE_NULL) {
        frame_free(kvaddr);
        cont->callback((void*)(cont->token), EFAULT);
        free(cont);
        return;
    }

    /* Map the frame into application's address space */
    err = _map_sel4_page(cont->as, frame_cap, cont->vpage, cont->permissions,
                         seL4_ARM_Default_VMAttributes);
    if (err) {
        frame_free(kvaddr);
        cspace_delete_cap(cur_cspace, frame_cap);
        cont->callback((void*)(cont->token), err);
        free(cont);
        return;
    }

    //set frame referenced
    frame_set_referenced(kvaddr);

    /* Insert PTE into application's pagetable */
    int x = PT_L1_INDEX(cont->vpage);
    int y = PT_L2_INDEX(cont->vpage);
    cont->as->as_pd_regs[x][y] = (kvaddr | PTE_IN_USE_BIT) & (~PTE_SWAPPED);
    cont->as->as_pd_caps[x][y] = frame_cap;

    dprintf(3, "_sos_page_map_5 called back up\n");
    /* Calling back up */
    cont->callback((void*)(cont->token), 0);
    free(cont);
    return;
}

static void
_insert_pt(addrspace_t *as, seL4_ARM_PageTable pt_cap, seL4_Word pt_addr) {
    sel4_pt_node_t* node = malloc(sizeof(sel4_pt_node_t));
    node->pt = pt_cap;
    node->pt_addr = pt_addr;
    node->next = as->as_pt_head;
    as->as_pt_head = node;
}

/**
 * Maps a page table into the root servers page directory
 * @param vaddr The virtual address of the mapping
 * @return 0 on success
 */
static int
_map_page_table(addrspace_t *as, seL4_ARM_PageDirectory pd, seL4_Word vpage){
    seL4_Word pt_addr;
    seL4_ARM_PageTable pt_cap;
    int err;

    /* Allocate a PT object */
    pt_addr = ut_alloc(seL4_PageTableBits);
    if(pt_addr == 0){
        return ENOMEM;
    }
    /* Create the frame cap */
    err =  cspace_ut_retype_addr(pt_addr, seL4_ARM_PageTableObject, seL4_PageTableBits,
                                 cur_cspace, &pt_cap);
    if(err){
        ut_free(pt_addr, seL4_PageTableBits);
        return EFAULT;
    }
    /* Tell seL4 to map the PT in for us */
    err = seL4_ARM_PageTable_Map(pt_cap, pd, vpage, seL4_ARM_Default_VMAttributes);
    if (err) {
        ut_free(pt_addr, seL4_PageTableBits);
        cspace_delete_cap(cur_cspace, pt_cap);
        return EFAULT;
    }

    _insert_pt(as, pt_cap, pt_addr);
    return 0;
}

static int
_map_sel4_page(addrspace_t *as, seL4_CPtr frame_cap, seL4_Word vpage,
               seL4_CapRights rights, seL4_ARM_VMAttributes attr) {

    seL4_ARM_PageDirectory pd = as->as_sel4_pd;
    int err;

    /* Attempt the mapping */
    err = seL4_ARM_Page_Map(frame_cap, pd, vpage, rights, attr);
    if(err == seL4_FailedLookup){
        /* Assume the error was because we have no page table */
        err = _map_page_table(as, pd, vpage);
        if(!err){
            /* Try the mapping again */
            err = seL4_ARM_Page_Map(frame_cap, pd, vpage, rights, attr);
        }
    }

    return err ? EFAULT : 0;
}

/***********************************************************************
 * sos_page_unmap
 ***********************************************************************/
int
sos_page_unmap(addrspace_t *as, seL4_Word vaddr){
    dprintf(3, "sos_page_unmap entered\n");
    seL4_Word vpage = PAGE_ALIGN(vaddr);
    int x = PT_L1_INDEX(vpage);
    int y = PT_L2_INDEX(vpage);

    if(as == NULL ||
            (as->as_pd_caps == NULL || as->as_pd_caps[x] == NULL) ||
            (as->as_pd_regs == NULL || as->as_pd_regs[x] == NULL)) {
        dprintf(3, "sos_page_unmap err 1\n");
        return EINVAL;
    }

    assert(as->as_pd_caps[x][y] != 0);
    int err;
    err = seL4_ARM_Page_Unmap(as->as_pd_caps[x][y]);
    if (err) {
        dprintf(3, "sos_page_unmap err 2\n");
        return EFAULT;
    }
    cspace_delete_cap(cur_cspace, as->as_pd_caps[x][y]);

    return 0;
}

/***********************************************************************
 * sos_page_free
 ***********************************************************************/
void
sos_page_free(addrspace_t *as, seL4_Word vaddr) {
    //dprintf(3, "sos_page_free\n");
    seL4_Word vpage = PAGE_ALIGN(vaddr);
    int x = PT_L1_INDEX(vpage);
    int y = PT_L2_INDEX(vpage);

    if(as == NULL ||
            (as->as_pd_caps == NULL || as->as_pd_caps[x] == NULL) ||
            (as->as_pd_regs == NULL || as->as_pd_regs[x] == NULL)) {
        return;
    }

    if(!(as->as_pd_regs[x][y] & PTE_IN_USE_BIT)) return;

    if (as->as_pd_regs[x][y] & PTE_SWAPPED) {
        swap_free_slot((as->as_pd_regs[x][y] & PTE_SWAP_MASK)>>PTE_SWAP_OFFSET);
    } else {
        int err;
        seL4_Word kvaddr = as->as_pd_regs[x][y] & PTE_KVADDR_MASK;
        //check if page to be destroy is second map paged out
        if(is_frame_referenced(kvaddr)){
            err = sos_page_unmap(as, vpage);
            if (err) {
                return;
            }
        }
        bool is_locked;
        frame_is_locked(kvaddr, &is_locked);
        if (is_locked) {
            // The frame is being swapped, don't do anything
        } else {
            frame_free(as->as_pd_regs[x][y] & PTE_KVADDR_MASK);
        }
    }
    as->as_pd_regs[x][y] = 0;
}


/***********************************************************************
 * Simple setter and getter functions
 * - sos_page_is_swapped
 * - sos_page_is_inuse
 * - sos_page_is_locked
 * - sos_get_kvaddr
 * - sos_get_kframe_cap
 ***********************************************************************/

bool
sos_page_is_inuse(addrspace_t *as, seL4_Word vaddr) {
    dprintf(3, "sos_page_is_inuse, vaddr = 0x%08x\n", vaddr);
    if (as == NULL || as->as_pd_caps == NULL || as->as_pd_regs == NULL) {
        return false;
    }
    seL4_Word vpage = PAGE_ALIGN(vaddr);
    int x = PT_L1_INDEX(vpage);
    int y = PT_L2_INDEX(vpage);
    return (as->as_pd_regs[x] != NULL && (as->as_pd_regs[x][y] & PTE_IN_USE_BIT));
}

bool
sos_page_is_swapped(addrspace_t *as, seL4_Word vaddr) {
    dprintf(3, "sos_page_is_swapped is called\n");
    if (as == NULL || as->as_pd_caps == NULL || as->as_pd_regs == NULL) {
        dprintf(3, "sos_page_is_swapped Invalid inputs\n");
        return false;
    }

    seL4_Word vpage = PAGE_ALIGN(vaddr);
    int x = PT_L1_INDEX(vpage);
    int y = PT_L2_INDEX(vpage);
    if(as->as_pd_regs[x] == NULL){
    dprintf(3, "1sos_page_is_swapped\n");

    }
    return (as->as_pd_regs[x] != NULL && (as->as_pd_regs[x][y] & PTE_SWAPPED));
}

bool
sos_page_is_locked(addrspace_t *as, seL4_Word vaddr) {
    dprintf(3, "sos_page_is_locked called, vaddr = 0x%08x\n", vaddr);
    if (as == NULL || as->as_pd_caps == NULL || as->as_pd_regs == NULL) {
        return false;
    }
    seL4_Word vpage = PAGE_ALIGN(vaddr);
    int x = PT_L1_INDEX(vpage);
    int y = PT_L2_INDEX(vpage);

    if (as->as_pd_regs[x] != NULL) {
        seL4_Word kvaddr = as->as_pd_regs[x][y] & PTE_KVADDR_MASK;
        bool is_locked;
        frame_is_locked(kvaddr, &is_locked);
        return is_locked;
    }
    return false;
}

int sos_get_kvaddr(addrspace_t *as, seL4_Word vaddr, seL4_Word *kvaddr) {
    if (as == NULL) {
        return EINVAL;
    }
    if (as->as_pd_regs == NULL || as->as_pd_caps == NULL) {
        /* Did you even call as_create? */
        return EFAULT;
    }

    int x = PT_L1_INDEX(vaddr);
    int y = PT_L2_INDEX(vaddr);
    if (as->as_pd_regs[x] == NULL || !(as->as_pd_regs[x][y] & PTE_IN_USE_BIT)) {
        return EINVAL;
    }

    *kvaddr = as->as_pd_regs[x][y] & PTE_KVADDR_MASK;
    *kvaddr += vaddr - PAGE_ALIGN(vaddr);
    return 0;
}

int sos_get_kframe_cap(addrspace_t *as, seL4_Word vaddr, seL4_CPtr *kframe_cap) {
    *kframe_cap = 0;
    if (as == NULL) {
        return EINVAL;
    }
    if (as->as_pd_regs == NULL || as->as_pd_caps == NULL) {
        /* Did you even call as_create? */
        return EFAULT;
    }

    int err;
    int x = PT_L1_INDEX(vaddr);
    int y = PT_L2_INDEX(vaddr);
    if (as->as_pd_regs[x] == NULL || !(as->as_pd_regs[x][y] & PTE_IN_USE_BIT) ||
            (as->as_pd_regs[x][y] & PTE_SWAPPED)) {
        return EINVAL;
    }

    seL4_Word kvaddr = as->as_pd_regs[x][y] & PTE_KVADDR_MASK;
    err = frame_get_cap(kvaddr, kframe_cap);
    if (err) {
        return err;
    }
    return 0;
}
