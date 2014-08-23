#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <limits.h>
#include <sel4/sel4.h>
#include <cspace/cspace.h>
#include <ut_manager/ut.h>
#include <strings.h>

#include "frametable.h"
#include "mapping.h"
#include "vmem_layout.h"

#define NFRAMES                  (FRAME_MEMORY / PAGE_SIZE)

#define FRAME_STATUS_UNTYPED     0
#define FRAME_STATUS_FREE        1
#define FRAME_STATUS_ALLOCATED   2

#define FRAME_INVALID            (-1)

/* Frame table entry structure */
typedef struct {
    seL4_CPtr fte_cap;
    seL4_Word fte_paddr;
    seL4_Word fte_vaddr;
    seL4_ARM_PageDirectory fte_pd;
    int fte_status;
    int fte_next_free;
} frame_entry_t;


frame_entry_t *frametable;
int first_free;                     // Index of the first free/untyped frame
static bool frame_initialised;
size_t frametable_reserved;           // # of frames the frametable consumes

/* Convert from an id in frametable to the corresponding vaddr */
static seL4_Word
id_to_vaddr(int i) {
    return (i*PAGE_SIZE + FRAME_VSTART);
}

/*
 * This function allocate a frame cap and then map it to the indicated VADDR
 *
 * Return: FRAME_IS_OK iff success
 */
static int
_map_to_sel4(const seL4_ARM_PageDirectory pd, const seL4_Word vaddr, seL4_Word *paddr, seL4_CPtr *cap) {

    /* Allocate memory */
    *paddr = ut_alloc(seL4_PageBits);
    if (*paddr == 0) {
        return FRAME_IS_FAIL;
    }

    /* Retype memory */
    cspace_err_t cspace_err = cspace_ut_retype_addr(*paddr, 
                                                    seL4_ARM_SmallPageObject, 
                                                    seL4_PageBits,
                                                    cur_cspace, 
                                                    cap);
    if (cspace_err != CSPACE_NOERROR) {
        ut_free(*paddr, seL4_PageBits);
        return FRAME_IS_FAIL;
    }

    /* Map memory */
    int err = map_page(*cap, pd, vaddr, seL4_AllRights, seL4_ARM_Default_VMAttributes);
    if (err) {
        ut_free(*paddr, seL4_PageBits);
        cspace_delete_cap(cur_cspace, *cap);
        return FRAME_IS_FAIL;
    }

    return FRAME_IS_OK;
}

int frame_init(){

    /* Calculate the amount of memory required for the frame table */
    size_t frametable_sz = NFRAMES * sizeof(frame_entry_t);
    printf("frametable_sz = %u\n", frametable_sz);
    printf("FRAME_VSTART = 0x%08x, FRAME_VEND = 0x%08x\n", (int)FRAME_VSTART, (int)FRAME_VEND);
    frametable = (frame_entry_t*)id_to_vaddr(0);

    /* Allocate memory for frametable to use */
    size_t i = 0;
    for (; i*PAGE_SIZE < frametable_sz; i++) {
        seL4_Word vaddr = id_to_vaddr(i);
        seL4_Word tmp_paddr;
        seL4_CPtr tmp_cap;
        int result = _map_to_sel4(seL4_CapInitThreadPD, vaddr, &tmp_paddr, &tmp_cap);
        assert(result == FRAME_IS_OK);

        frametable[i].fte_paddr     = tmp_paddr;
        frametable[i].fte_cap       = tmp_cap;
        frametable[i].fte_vaddr     = vaddr;
        frametable[i].fte_status    = FRAME_STATUS_ALLOCATED;
        frametable[i].fte_pd        = seL4_CapInitThreadPD;
        frametable[i].fte_next_free = FRAME_INVALID;
    }

    /* Mark the number of frames occupied by the frametable */
    frametable_reserved = i;

    /* The ith frame is the first free frame */
    first_free = i;

    /* Initialise the remaining frames */
    for (; i<NFRAMES; i++) {
        frametable[i].fte_status = FRAME_STATUS_UNTYPED;
        frametable[i].fte_next_free = (i == NFRAMES-1) ? FRAME_INVALID : i+1;
    }

    frame_initialised = true;

    return FRAME_IS_OK;
}

int frame_alloc(seL4_Word *vaddr){

    /* In case we fail early */
    *vaddr = 0;

    if (!frame_initialised) {
        return FRAME_IS_UNINT;
    }
    if(first_free == FRAME_INVALID) {
        return FRAME_IS_FAIL;
    }

    int result;
    int ind = first_free;

    /* Allocate and map this frame */
    seL4_Word tmp_vaddr = id_to_vaddr(ind);

    result = _map_to_sel4(seL4_CapInitThreadPD, tmp_vaddr,
                          &frametable[ind].fte_paddr, &frametable[ind].fte_cap);
    if (result != FRAME_IS_OK) {
        return result;
    }

    frametable[ind].fte_status = FRAME_STATUS_ALLOCATED;
    frametable[ind].fte_vaddr  = tmp_vaddr;
    frametable[ind].fte_pd     = seL4_CapInitThreadPD;

    /* Zero fill memory */
    bzero((void *)(tmp_vaddr), (size_t)PAGE_SIZE);

    /* Update free frame list */
    first_free = frametable[ind].fte_next_free;

    /* Now update the vaddr */
    *vaddr = tmp_vaddr;
    return ind;
}

int frame_free(int id){
    /* May have concurency issues */
    
    if (!frame_initialised) {
        return FRAME_IS_UNINT;
    }
    if (id < frametable_reserved || id >= NFRAMES) {
        return FRAME_IS_FAIL;
    }
    if(frametable[id].fte_status != FRAME_STATUS_ALLOCATED) {
        return FRAME_IS_FAIL;
    }

    seL4_Word paddr = frametable[id].fte_paddr;
    seL4_CPtr frame_cap = frametable[id].fte_cap;

    /* "Freeing" the frame */
    int err = seL4_ARM_Page_Unmap(frame_cap);
    assert(!err);
    frametable[id].fte_status = FRAME_STATUS_FREE;

    /* Untyping the frame */
    cspace_err_t cspace_err = cspace_delete_cap(cur_cspace, frame_cap);
    assert(cspace_err == CSPACE_NOERROR);
    ut_free(paddr, seL4_PageBits);
    frametable[id].fte_status = FRAME_STATUS_UNTYPED;

    /* Update free frame list */
    frametable[id].fte_next_free = first_free;
    first_free = id;

    return FRAME_IS_OK;
}

seL4_CPtr
frame_get_cap(int id) {
    if (!frame_initialised) {
        return FRAME_IS_UNINT;
    }
    if (id < frametable_reserved || id >= NFRAMES) {
        return FRAME_IS_FAIL;
    }
    if(frametable[id].fte_status != FRAME_STATUS_ALLOCATED) {
        return FRAME_IS_FAIL;
    }
    return frametable[id].fte_cap;
}


/*
 * Note: Only works for n == 1 now
 */
seL4_Word
alloc_kpages(int n) {
    assert(n == 1);

    seL4_Word vaddr;
    frame_alloc(&vaddr);
    return vaddr;
}

/*
 * TODO
 */
int
free_kpages(int n){
    assert(n == 1);
    return 1; 
}
