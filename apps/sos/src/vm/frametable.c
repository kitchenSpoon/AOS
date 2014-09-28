#include <stdio.h>
#include <assert.h>
#include <strings.h>
#include <limits.h>
#include <sel4/sel4.h>
#include <cspace/cspace.h>
#include <ut_manager/ut.h>
#include <errno.h>

#include "vm/vm.h"
#include "vm/mapping.h"
#include "vm/vmem_layout.h"
#include "tool/utility.h"

#define NFRAMES                  (FRAME_MEMORY / PAGE_SIZE)

#define FRAME_STATUS_UNTYPED     0
#define FRAME_STATUS_FREE        1
#define FRAME_STATUS_ALLOCATED   2

#define FRAME_INVALID            (-1)

#define ID_TO_VADDR(id)     ((id)*PAGE_SIZE + FRAME_VSTART) 
#define VADDR_TO_ID(vaddr)  (((vaddr) - FRAME_VSTART) / PAGE_SIZE)

/* Frame table entry structure */
typedef struct {
    seL4_CPtr fte_cap;
    seL4_Word fte_paddr;
    seL4_Word fte_vaddr;
    seL4_ARM_PageDirectory fte_pd;
    int fte_status;
    int fte_next_free;
    bool frame_locked;
} frame_entry_t;


frame_entry_t *frametable;
int first_free;                     // Index of the first free/untyped frame
static bool frame_initialised;
size_t frametable_reserved;           // # of frames the frametable consumes

/*
 * This function allocate a frame cap and then map it to the indicated VADDR
 *
 * Return: 0 iff success
 */
static int
_map_to_sel4(const seL4_ARM_PageDirectory pd, const seL4_Word vaddr, seL4_Word *paddr, seL4_CPtr *cap) {

    /* Allocate memory */
    *paddr = ut_alloc(seL4_PageBits);
    if (*paddr == 0) {
        return ENOMEM;
    }

    /* Retype memory */
    cspace_err_t cspace_err = cspace_ut_retype_addr(*paddr, 
                                                    seL4_ARM_SmallPageObject, 
                                                    seL4_PageBits,
                                                    cur_cspace, 
                                                    cap);
    if (cspace_err != CSPACE_NOERROR) {
        ut_free(*paddr, seL4_PageBits);
        return EFAULT;
    }

    /* Map memory */
    int err = map_page(*cap, pd, vaddr, seL4_AllRights, seL4_ARM_Default_VMAttributes);
    if (err) {
        ut_free(*paddr, seL4_PageBits);
        cspace_delete_cap(cur_cspace, *cap);
        return EFAULT;
    }

    return 0;
}

int
frame_init(void){

    /* Calculate the amount of memory required for the frame table */
    size_t frametable_sz = NFRAMES * sizeof(frame_entry_t);
    frametable = (frame_entry_t*)ID_TO_VADDR(0);

    /* Allocate memory for frametable to use */
    size_t i = 0;
    for (; i*PAGE_SIZE < frametable_sz; i++) {
        seL4_Word vaddr = (seL4_Word)ID_TO_VADDR(i);
        seL4_Word tmp_paddr;
        seL4_CPtr tmp_cap;
        int err = _map_to_sel4(seL4_CapInitThreadPD, vaddr, &tmp_paddr, &tmp_cap);
        if (err) {
            return err;
        }

        frametable[i].fte_paddr     = tmp_paddr;
        frametable[i].fte_cap       = tmp_cap;
        frametable[i].fte_vaddr     = vaddr;
        frametable[i].fte_status    = FRAME_STATUS_ALLOCATED;
        frametable[i].fte_pd        = seL4_CapInitThreadPD;
        frametable[i].fte_next_free = FRAME_INVALID;
        frametable[i].frame_locked = false;
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

    return 0;
}

bool frame_has_free(void) {
    if (!frame_initialised) {
        return false;
    }
    return (first_free != FRAME_INVALID);
}

seL4_Word frame_alloc(void){

    if (!frame_initialised) {
        return 0;
    }
    if(first_free == FRAME_INVALID) {
        return 0;
    }

    int err;
    int ind = first_free;

    /* Allocate and map this frame */
    seL4_Word vaddr = (seL4_Word)ID_TO_VADDR(ind);

    err = _map_to_sel4(seL4_CapInitThreadPD, vaddr,
                          &frametable[ind].fte_paddr, &frametable[ind].fte_cap);
    if (err) {
        return 0;
    }

    frametable[ind].fte_status = FRAME_STATUS_ALLOCATED;
    frametable[ind].fte_vaddr  = vaddr;
    frametable[ind].fte_pd     = seL4_CapInitThreadPD;
    frametable[ind].frame_locked = false;

    /* Zero fill memory */
    bzero((void *)(vaddr), (size_t)PAGE_SIZE);

    /* Update free frame list */
    first_free = frametable[ind].fte_next_free;

    assert(IS_PAGESIZE_ALIGNED(vaddr));
    return vaddr;
}

int frame_free(seL4_Word vaddr){
    /* May have concurency issues */

    if (!frame_initialised) {
        /* Why is frame uninitialised? */
        return EFAULT;
    }

    int id = (int)VADDR_TO_ID(vaddr);
    if (id < frametable_reserved || id >= NFRAMES) {
        return EINVAL;
    }
    if(frametable[id].fte_status != FRAME_STATUS_ALLOCATED) {
        return EINVAL;
    }

    //frame to be freed should not be locked
    if(frametable[id].frame_locked) {
        return EFAULT;
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

    return 0;
}

int
frame_get_cap(seL4_Word vaddr, seL4_CPtr *frame_cap) {
    *frame_cap = -1;
    if (!frame_initialised) {
        return EFAULT;
    }

    int id = (int)VADDR_TO_ID(vaddr);
    if (id < frametable_reserved || id >= NFRAMES) {
        return EINVAL;
    }
    if(frametable[id].fte_status != FRAME_STATUS_ALLOCATED) {
        return EINVAL;
    }
    *frame_cap = frametable[id].fte_cap;
    return 0;
}

int frame_lock_frame(seL4_Word vaddr){
     if (!frame_initialised) {
        return EFAULT;
    }

    int id = (int)VADDR_TO_ID(vaddr);
    if (id < frametable_reserved || id >= NFRAMES) {
        return EINVAL;
    }
    if(frametable[id].fte_status != FRAME_STATUS_ALLOCATED) {
        return EINVAL;
    }

    //might need to do this atomically?
    if(frametable[id].frame_locked == true) {
        return EINVAL;
    } else {
        frametable[id].frame_locked = true;
        return 0;
    } 
}

int frame_unlock_frame(seL4_Word vaddr){
     if (!frame_initialised) {
        return EFAULT;
    }

    int id = (int)VADDR_TO_ID(vaddr);
    if (id < frametable_reserved || id >= NFRAMES) {
        return EINVAL;
    }
    if(frametable[id].fte_status != FRAME_STATUS_ALLOCATED) {
        return EINVAL;
    }

    //might need to do this atomically?
    if(frametable[id].frame_locked == false) {
        return EINVAL;
    } else {
        frametable[id].frame_locked = false;
        return 0;
    } 
}
