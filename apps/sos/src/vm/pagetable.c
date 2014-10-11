#include <sel4/sel4.h>
#include <stdio.h>
#include <limits.h>
#include <assert.h>
#include <string.h>
#include <strings.h>
#include <ut_manager/ut.h>

#include "vm/vm.h"
#include "vm/addrspace.h"
#include "vm/mapping.h"
#include "tool/utility.h"

#define STATUS_USED     0
#define STATUS_FREE     1

#define PAGEDIR_BITS        (12)
#define PAGETABLE_BITS      (12)
#define PAGETABLE_PAGES     (PAGE_SIZE >> 2)

static void
_insert_pt(addrspace_t *as, seL4_ARM_PageTable pt_cap) {
    sel4_pt_node_t* node = malloc(sizeof(sel4_pt_node_t));
    node->pt = pt_cap;
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

    _insert_pt(as, pt_cap);
    return 0;
}

static int
_map_page(addrspace_t *as, seL4_CPtr frame_cap, seL4_Word vpage,
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

typedef struct {
    sos_page_map_cb_t callback;
    void *token;
    addrspace_t* as;
    seL4_Word vpage;
    uint32_t permissions;
    bool noswap;
} sos_page_map_cont_t;

static void
sos_page_map_part5(void* token, seL4_Word kvaddr){
    printf("sos_page_map 5\n");

    sos_page_map_cont_t* cont = (sos_page_map_cont_t*)token;

    seL4_CPtr kframe_cap, frame_cap;

    if (!kvaddr) {
        printf("sos_page_map_part5 failed to allocate memory for frame\n");
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
    err = _map_page(cont->as, frame_cap, cont->vpage, cont->permissions,
                    seL4_ARM_Default_VMAttributes);
    if (err) {
        frame_free(kvaddr);
        cspace_delete_cap(cur_cspace, frame_cap);
        cont->callback((void*)(cont->token), err);
        free(cont);
        return;
    }

    //set frame referenced
    set_frame_referenced(kvaddr);

    /* Insert PTE into application's pagetable */
    int x = PT_L1_INDEX(cont->vpage);
    int y = PT_L2_INDEX(cont->vpage);
    cont->as->as_pd_regs[x][y] = (kvaddr | PTE_IN_USE_BIT) & (~PTE_SWAPPED);
    cont->as->as_pd_caps[x][y] = frame_cap;

    /* Calling back up */
    cont->callback((void*)(cont->token), 0);
    free(cont);
    return;

}

static void
sos_page_map_part4(void* token){
    printf("sos_page_map 4\n");
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
    int err = frame_alloc(cont->vpage, cont->as, cont->noswap, sos_page_map_part5, token);
    if (err) {
        printf("sos_page_map_part4: failed to allocate frame\n");
        cont->callback(cont->token, EINVAL);
        free(cont);
        return;
    }
}

static void
sos_page_map_part3(void* token, seL4_Word kvaddr){
    printf("sos_page_map 3\n");
    sos_page_map_cont_t* cont = (sos_page_map_cont_t*)token;

    int x = PT_L1_INDEX(cont->vpage);
    if (kvaddr == 0) {
        printf("warning: sos_page_map_part3 not enough memory for lvl2 pagetable\n");
        frame_free((seL4_Word)(cont->as->as_pd_regs[x]));
        cont->callback(cont->token, ENOMEM);
        free(cont);
        return;
    }

    cont->as->as_pd_caps[x] = (pagetable_t)kvaddr;

    sos_page_map_part4(token);
}

static void
sos_page_map_part2(void* token, seL4_Word kvaddr){
    printf("sos_page_map 2\n");
    sos_page_map_cont_t* cont = (sos_page_map_cont_t*)token;

    if (kvaddr == 0) {
        printf("warning: sos_page_map_part2 not enough memory for lvl2 pagetable\n");
        cont->callback(cont->token, ENOMEM);
        free(cont);
        return;
    }

    seL4_Word vpage = PAGE_ALIGN(cont->vpage);
    int x = PT_L1_INDEX(vpage);
    cont->as->as_pd_regs[x] = (pagetable_t)kvaddr;

    /* Allocate memory for the 2nd level pagetable for caps */
    int err = frame_alloc(0, NULL, true, sos_page_map_part3, token);
    if (err) {
        frame_free(kvaddr);
        cont->callback(cont->token, EFAULT);
        free(cont);
        return;
    }
}

int
sos_page_map(addrspace_t *as, seL4_Word vaddr, uint32_t permissions,
        sos_page_map_cb_t callback, void* token, bool noswap) {
    printf("sos_page_map\n");
    if (as == NULL) {
        return EINVAL;
    }


    if (as->as_pd_caps == NULL || as->as_pd_regs == NULL) {
        /* Did you even call as_create? */
        printf("sos_page_map err einval 0\n");
        return EFAULT;
    }

    seL4_Word vpage = PAGE_ALIGN(vaddr);

    sos_page_map_cont_t* cont = malloc(sizeof(sos_page_map_cont_t));
    if(cont == NULL){
        printf("sos_page_map err nomem\n");
        return ENOMEM;
    }
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
        err = frame_alloc(0, NULL, true, sos_page_map_part2, (void*)cont);
        if (err) {
            free(cont);
            return EFAULT;
        }
        return 0;
    }

    sos_page_map_part4((void*)cont);
    return 0;
}

int
sos_page_unmap(addrspace_t *as, seL4_Word vaddr){
    printf("sos_page_unmap entered\n");
    seL4_Word vpage = PAGE_ALIGN(vaddr);
    int x = PT_L1_INDEX(vpage);
    int y = PT_L2_INDEX(vpage);

    if(as == NULL ||
            (as->as_pd_caps == NULL || as->as_pd_caps[x] == NULL) ||
            (as->as_pd_regs == NULL || as->as_pd_regs[x] == NULL)) {
        printf("sos_page_unmap err 1\n");
        return EINVAL;
    }

    assert(as->as_pd_caps[x][y] != 0);
    int err;
    err = seL4_ARM_Page_Unmap(as->as_pd_caps[x][y]);
    if (err) {
        printf("sos_page_unmap err 2\n");
        return EFAULT;
    }
    cspace_delete_cap(cur_cspace, as->as_pd_caps[x][y]);

    return 0;
}

void
sos_page_free(addrspace_t *as, seL4_Word vaddr) {
    seL4_Word vpage = PAGE_ALIGN(vaddr);
    int x = PT_L1_INDEX(vpage);
    int y = PT_L2_INDEX(vpage);

    if(as == NULL ||
            (as->as_pd_caps == NULL || as->as_pd_caps[x] == NULL) ||
            (as->as_pd_regs == NULL || as->as_pd_regs[x] == NULL)) {
        return;
    }

    int err;
    err = sos_page_unmap(as, vaddr);
    if (err) {
        return;
    }

    if (as->as_pd_regs[x][y] & PTE_SWAPPED) {
        //TODO: free the slot in the swap file
    } else {
        frame_free(as->as_pd_regs[x][y] & PTE_KVADDR_MASK);
        as->as_pd_regs[x][y] = 0;
    }
}


bool
sos_page_is_swapped(addrspace_t *as, seL4_Word vaddr) {
    if (as == NULL || as->as_pd_caps == NULL || as->as_pd_regs == NULL) {
        printf("sos_page_is_swapped Invalid inputs\n");
        return false;
    }

    seL4_Word vpage = PAGE_ALIGN(vaddr);
    int x = PT_L1_INDEX(vpage);
    int y = PT_L2_INDEX(vpage);
    return (as->as_pd_regs[x] != NULL && (as->as_pd_regs[x][y] & PTE_SWAPPED));
}

bool
sos_page_is_inuse(addrspace_t *as, seL4_Word vaddr) {
    if (as == NULL || as->as_pd_caps == NULL || as->as_pd_regs == NULL) {
        return false;
    }

    seL4_Word vpage = PAGE_ALIGN(vaddr);
    int x = PT_L1_INDEX(vpage);
    int y = PT_L2_INDEX(vpage);
    return (as->as_pd_regs[x] != NULL && (as->as_pd_regs[x][y] & PTE_IN_USE_BIT));
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
    //TODO: what if this page is swapped out?
    if (as->as_pd_regs[x] == NULL || !(as->as_pd_regs[x][y] & PTE_IN_USE_BIT)) {
        return EINVAL;
    }

    seL4_Word kvaddr = as->as_pd_regs[x][y] & PTE_KVADDR_MASK;
    err = frame_get_cap(kvaddr, kframe_cap);
    if (err) {
        return err;
    }
    return 0;
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
