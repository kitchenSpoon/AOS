#include <assert.h>
#include <string.h>
#include <cspace/cspace.h>
#include <mapping.h>
#include <ut_manager/ut.h>
#include <strings.h>

#define FRAME_STATUS_UNTYPED     0
#define FRAME_STATUS_FREE        1
#define FRAME_STATUS_ALLOCATED   2

#define PAGE_SIZE (1ull<<12)

/* Frame table entry structure */
typedef struct {
    seL4_CPtr fte_cap;
    seL4_Word fte_paddr;
    seL4_Word fte_vaddr;
    seL4_ARM_PageDirectory fte_pd;
    int fte_status;
    int fte_next_free;
} frame_entry_t;

frame_entry_t *frame_table;
int first_free;

/* Keep track of initialisation status of the frame table */
static bool frame_initialised;


int frame_init(){
    /* First free should not be one */
    first_free = -1;
    frame_initialised = true;
    return 0;
}

/*
 * Retype a page
 */
static void
_alloc_n_map(const seL4_ARM_PageDirectory pd, const seL4_Word vaddr, seL4_Word *paddr, seL4_CPtr *cap) {
    /* Allocate memory */
    *paddr = ut_alloc(seL4_PageBits);

    /* Retype memory */
    cspace_err_t cspace_err = cspace_ut_retype_addr(*paddr, 
                                                    seL4_ARM_SmallPageObject, 
                                                    seL4_PageBits,
                                                    cur_cspace, 
                                                    cap);
    assert(cspace_err == CSPACE_NOERROR);

    /* Map memory */
    int err = map_page(*cap, pd, vaddr, seL4_AllRights, seL4_ARM_Default_VMAttributes);
    assert(!err);
}

int frame_alloc(seL4_Word *vaddr){
    if(first_free == -1) return 1;
    
    /* Update linked list */
    int ind = first_free;
    first_free = frame_table[ind].fte_next_free;

    *vaddr = 0x20000000 + ind * PAGE_SIZE;

    _alloc_n_map(seL4_CapInitThreadPD, *vaddr,
                &frame_table[ind].fte_paddr, &frame_table[ind].fte_cap);

    /* Zero memory */
    bzero((void *)(*vaddr), (size_t)PAGE_SIZE);

    return 0;
}

int frame_free(int id){
    /* May have concurency issues */
    
    seL4_Word paddr = frame_table[id].fte_paddr;
    seL4_CPtr frame_cap = frame_table[id].fte_cap;

    /* Unmap this frame from vaddr */
    int err = seL4_ARM_Page_Unmap(frame_cap);
    assert(!err);

    frame_table[id].fte_status = FRAME_STATUS_FREE;

    /* Start untyping the memory */
    /* Delete Cap */
    cspace_err_t cspace_err = cspace_delete_cap(cur_cspace, frame_cap);
    assert(cspace_err == CSPACE_NOERROR);

    /* Free memory back to untyped */
    ut_free(paddr, seL4_PageBits);
    
    frame_table[id].fte_status = FRAME_STATUS_UNTYPED;

    /* Update linked list */
    frame_table[id].fte_next_free = first_free;
    first_free = id;

    return 0;
}
