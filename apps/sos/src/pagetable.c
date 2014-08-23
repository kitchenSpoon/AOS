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

#define INDEX_1_MASK        (0xffc00000)
#define INDEX_2_MASK        (0x003ff000)
#define INDEX_1(a)          ((a) & INDEX_1_MASK >> 22)
#define INDEX_2(a)          ((a) & INDEX_2_MASK >> 12)

int
sos_page_map(addrspace_t *as,
             seL4_ARM_PageDirectory app_sel4_pd, cspace_t *app_cspace,
             seL4_Word vaddr, seL4_Word* kvaddr) {
    // This is the sos_map_page() function mentioned in the Milestone note
    // We could probably follow the map_page() code
    // Except that we need to do a lot more than that
    // We need to do 3 things:
    //     Call frame_alloc to get map SOS's vaddr to seL4's frame
    //     Get the frame cap from the frametable, map the application's vaddr to the same frame
    //     Map the application's vaddr to its own pagetable (the one we implement)

    int err;
    pagetable_entry_t pte;

    /* Link SOS's VM to seL4 pagetable */
    pte.kvaddr = frame_alloc();
    if (!pte.kvaddr) {
        return PAGE_IS_FAIL;
    }
    pte.kframe_cap = frame_get_cap(pte.kvaddr);

    /* Link application's VM to seL4 pagetable */
    pte.frame_cap = cspace_copy_cap(app_cspace, 
                                 cur_cspace, 
                                 pte.kframe_cap,
                                 seL4_AllRights);
    if (pte.frame_cap == CSPACE_NULL) {
        frame_free(pte.kvaddr);
        return PAGE_IS_FAIL;
    }

    //todo: need to implement our own map_page if we want to free
    err = map_page(pte.frame_cap, app_sel4_pd, vaddr, 
                   seL4_AllRights, seL4_ARM_Default_VMAttributes);
    if (err) {
        frame_free(pte.kvaddr);
        cspace_delete_cap(app_cspace, pte.frame_cap);
        return PAGE_IS_FAIL;
    }

    /* Insert PTE into application's pagetable */
    int x = INDEX_1(vaddr);
    int y = INDEX_2(vaddr);
    assert(as->as_pd != NULL);
    as->as_pd[x] = (pagetable_t)frame_alloc();
    if (as->as_pd[x] == NULL) {
        /* No memory */
        frame_free(pte.kvaddr);
        cspace_delete_cap(app_cspace, pte.frame_cap);
        //todo: unmap_page when implemented local map_page
        return PAGE_IS_FAIL;
    }

    as->as_pd[x][y] = (pagetable_entry_t*)malloc(sizeof(pagetable_entry_t));
    if (as->as_pd[x][y] == NULL) {
        frame_free(pte.kvaddr);
        cspace_delete_cap(app_cspace, pte.frame_cap);
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
    cspace_delete_cap(app_cspace, pte.frame_cap);
*/
    return 0;
}
