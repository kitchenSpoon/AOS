#include <sel4/sel4.h>
#include <stdio.h>
#include <limits.h>
#include <assert.h>
#include <string.h>

#include "vm.h"
#include "addrspace.h"
#include "mapping.h"

#define STATUS_USED     0
#define STATUS_FREE     1

#define PAGEDIR_BITS        (12)
#define PAGETABLE_BITS      (12)
#define PAGETABLE_PAGES     (PAGE_SIZE >> 2)

#define INDEX_1_MASK        (0xffc00000)
#define INDEX_2_MASK        (0x003ff000)
#define INDEX_1(a)          (((a) & INDEX_1_MASK) >> 22)
#define INDEX_2(a)          (((a) & INDEX_2_MASK) >> 12)

#define PAGEMASK              ((PAGE_SIZE) - 1)
#define PAGE_ALIGN(addr)      ((addr) & ~(PAGEMASK))
#define IS_PAGESIZE_ALIGNED(addr) !((addr) &  (PAGEMASK))

static
int _create_pagetable(addrspace_t *as, int x) {
    assert(as != NULL);
    as->as_pd[x] = (pagetable_t)frame_alloc();
    if (as->as_pd[x] == NULL) {
        return PAGE_IS_FAIL;
    }
    return 0;
}

int
sos_page_map(addrspace_t *as, seL4_ARM_PageDirectory app_sel4_pd,
             seL4_Word vaddr, seL4_Word* kvaddr) {
    printf("sos_page_page called for 0x%08x\n", vaddr);
    if (as == NULL) {
        return PAGE_IS_FAIL;
    }

    if (as->as_pd == NULL) {
        /* Did you even call as_create? */
        return PAGE_IS_FAIL;
    }

    int err;
    pagetable_entry_t pte;
    seL4_Word vpage = PAGE_ALIGN(vaddr);

    /* First we create a frame in SOS */
    pte.kvaddr = frame_alloc();
    if (!pte.kvaddr) {
        return PAGE_IS_FAIL;
    }
    pte.kframe_cap = frame_get_cap(pte.kvaddr);


    /* Copy the frame cap as we need to map it into 2 address spaces */
    pte.frame_cap = cspace_copy_cap(cur_cspace, cur_cspace, pte.kframe_cap, seL4_AllRights);
    if (pte.frame_cap == CSPACE_NULL) {
        frame_free(pte.kvaddr);
        return PAGE_IS_FAIL;
    }

    /* Map the frame into application's address spaces */
    //todo: need to implement our own map_page if we want to free
    err = map_page(pte.frame_cap, app_sel4_pd, vpage, 
                   seL4_AllRights, seL4_ARM_Default_VMAttributes);
    if (err) {
        frame_free(pte.kvaddr);
        cspace_delete_cap(cur_cspace, pte.frame_cap);
        return PAGE_IS_FAIL;
    }

    /* Insert PTE into application's pagetable */
    //TODO: move this step to above level and change the rollback steps
    int x = INDEX_1(vpage);
    int y = INDEX_2(vpage);

    if (as->as_pd[x] == NULL) {
        err = _create_pagetable(as, x);
        if (err) {
            frame_free(pte.kvaddr);
            cspace_delete_cap(cur_cspace, pte.frame_cap);
            //todo: unmap_page when implemented local map_page
            return PAGE_IS_FAIL;
        }
    }

    if (as->as_pd[x][y] != NULL) {
        /* page already exists */
        frame_free(pte.kvaddr);
        cspace_delete_cap(cur_cspace, pte.frame_cap);
        //todo: unmap_page when implemented local map_page
        return PAGE_IS_FAIL;
    }
    as->as_pd[x][y] = (pagetable_entry_t*)malloc(sizeof(pagetable_entry_t));
    if (as->as_pd[x][y] == NULL) {
        frame_free(pte.kvaddr);
        cspace_delete_cap(cur_cspace, pte.frame_cap);
        //todo: unmap_page when implemented local map_page
        return PAGE_IS_FAIL;
    }
    *(as->as_pd[x][y]) = pte;
    *kvaddr = pte.kvaddr;

    return 0;
}

int
sos_page_unmap(pagedir_t* pd, seL4_Word vaddr){
/*

    //TODO
    
    //remove PTE from application's pagetable
    int x = INDEX_1(vaddr);
    int y = INDEX_2(vaddr);
    
    if (as->as_pd[x] != NULL) {
        pagetable_entry_t *pte = as->as_pd[x][y];
        as->as_pd[x][y] = NULL;
        free(pte);
    }
    
    //unmap page from application's pd?
    
    //frame_free
    frame_free(pte.kvaddr);
    cspace_delete_cap(cur_cspace, pte.frame_cap);
*/
    return 0;
}
