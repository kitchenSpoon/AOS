#include <assert.h>
#include <string.h>
#include <cspace/cspace.h>
#include <mapping.h>
#include <ut_manager/ut.h>

#define FRAME_STATUS_UNTYPED     0
#define FRAME_STATUS_FREE        1
#define FRAME_STATUS_ALLOCATED   2

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

/*
 * Find index of the next free frame in the frame table
 * 
 * Returns index of the next free frame
 */
static int
_next_free(void) {
    /* Check if we have any free frames */
    /* TODO:Change this to use a linked list */
    for(int i = 0; i < 10; i++){
        if(frame_table[i].fte_status == FRAME_STATUS_UNTYPED) return i;
    }

    /* No Free Frames */
    return -1;
}


int frame_init(){
    /* First free should not be one */
    first_free = -1;
    frame_initialised = true;
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
    
    return 0;
}
