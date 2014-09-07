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

#define INDEX_1_MASK        (0xffc00000)
#define INDEX_2_MASK        (0x003ff000)
#define PT_L1_INDEX(a)      (((a) & INDEX_1_MASK) >> 22)
#define PT_L2_INDEX(a)      (((a) & INDEX_2_MASK) >> 12)

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
_map_page_table(addrspace_t *as, seL4_ARM_PageDirectory pd, seL4_Word vaddr){
    seL4_Word pt_addr;
    seL4_ARM_PageTable pt_cap;
    int err;

    /* Allocate a PT object */
    pt_addr = ut_alloc(seL4_PageTableBits);
    if(pt_addr == 0){
        return ENOMEM;
    }
    /* Create the frame cap */
    err =  cspace_ut_retype_addr(pt_addr,
                                 seL4_ARM_PageTableObject,
                                 seL4_PageTableBits,
                                 cur_cspace,
                                 &pt_cap);
    if(err){
        ut_free(pt_addr, seL4_PageTableBits);
        return EFAULT;
    }
    /* Tell seL4 to map the PT in for us */
    err = seL4_ARM_PageTable_Map(pt_cap,
                                 pd,
                                 vaddr,
                                 seL4_ARM_Default_VMAttributes);
    if (err) {
        ut_free(pt_addr, seL4_PageTableBits);
        cspace_delete_cap(cur_cspace, pt_cap);
        return EFAULT;
    }

    _insert_pt(as, pt_cap);
    return 0;
}

static int
_map_page(addrspace_t *as, seL4_CPtr frame_cap, seL4_Word vaddr,
          seL4_CapRights rights, seL4_ARM_VMAttributes attr) {

    seL4_ARM_PageDirectory pd = as->as_sel4_pd;
    int err;

    /* Attempt the mapping */
    err = seL4_ARM_Page_Map(frame_cap, pd, vaddr, rights, attr);
    if(err == seL4_FailedLookup){
        /* Assume the error was because we have no page table */
        err = _map_page_table(as, pd, vaddr);
        if(!err){
            /* Try the mapping again */
            err = seL4_ARM_Page_Map(frame_cap, pd, vaddr, rights, attr);
        }
    }

    return err ? EFAULT : 0;
}

int
sos_page_map(addrspace_t *as, seL4_Word vaddr, uint32_t permissions) {
    if (as == NULL) {
        return EINVAL;
    }

    if (as->as_pd_caps == NULL || as->as_pd_regs == NULL) {
        /* Did you even call as_create? */
        return EFAULT;
    }

    int x, y, err;
    seL4_Word vpage, kvaddr;
    seL4_CPtr kframe_cap, frame_cap;


    vpage = PAGE_ALIGN(vaddr);
    x = PT_L1_INDEX(vpage);
    y = PT_L2_INDEX(vpage);

    if (as->as_pd_regs[x] == NULL) {
        assert(as->as_pd_caps[x] == NULL);

        /* Create pagetable if needed */
        as->as_pd_regs[x] = (pagetable_t)frame_alloc();
        if (as->as_pd_regs[x] == NULL) {
            return ENOMEM;
        }

        as->as_pd_caps[x] = (pagetable_t)frame_alloc();
        if (as->as_pd_caps[x] == 0) {
            frame_free((seL4_Word)as->as_pd_regs[x]);
            return ENOMEM;
        }
    }

    if (as->as_pd_regs[x][y] & PTE_IN_USE_BIT) {
        /* page already mapped */
        return EINVAL;
    }


    /* First we create a frame in SOS */
    kvaddr = frame_alloc();
    if (!kvaddr) {
        return ENOMEM;
    }
    err = frame_get_cap(kvaddr, &kframe_cap);
    assert(!err); // There should be no error


    /* Copy the frame cap as we need to map it into 2 address spaces */
    frame_cap = cspace_copy_cap(cur_cspace, cur_cspace, kframe_cap, permissions);
    if (frame_cap == CSPACE_NULL) {
        frame_free(kvaddr);
        return EFAULT;
    }

    /* Map the frame into application's address spaces */
    err = _map_page(as, frame_cap, vpage, permissions,
                    seL4_ARM_Default_VMAttributes);
    if (err) {
        frame_free(kvaddr);
        cspace_delete_cap(cur_cspace, frame_cap);
        return err;
    }

    /* Insert PTE into application's pagetable */
    as->as_pd_regs[x][y] = kvaddr | PTE_IN_USE_BIT;
    as->as_pd_caps[x][y] = frame_cap;
    return 0;
}

int
sos_page_unmap(pagedir_t* pd, seL4_Word vaddr){
    return 0;
}

bool
sos_page_is_mapped(addrspace_t *as, seL4_Word vaddr) {
    if (as == NULL || as->as_pd_caps == NULL || as->as_pd_regs == NULL) {
        return false;
    }

    int x = PT_L1_INDEX(vaddr);
    int y = PT_L2_INDEX(vaddr);
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