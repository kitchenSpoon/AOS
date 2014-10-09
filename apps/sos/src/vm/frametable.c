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
#include "vm/swap.h"
#include "tool/utility.h"

#define NFRAMES                  (FRAME_MEMORY / PAGE_SIZE)

#define FRAME_STATUS_UNTYPED     0
#define FRAME_STATUS_FREE        1
#define FRAME_STATUS_ALLOCATED   2

#define FRAME_INVALID            (-1)

#define ID_TO_KVADDR(id)     ((id)*PAGE_SIZE + FRAME_VSTART)
#define KVADDR_TO_ID(kvaddr)  (((kvaddr) - FRAME_VSTART) / PAGE_SIZE)

/* Frame table entry structure */
typedef struct {
    seL4_CPtr fte_cap;
    seL4_Word fte_paddr;
    seL4_Word fte_kvaddr;
    seL4_ARM_PageDirectory fte_pd; // This represents an ASID
    int fte_status;
    int fte_next_free;
    bool fte_locked;
    //TODO: add an unswappable field
} frame_entry_t;


frame_entry_t *frametable;
int first_free;                     // Index of the first free/untyped frame
static bool frame_initialised;
size_t frametable_reserved;           // # of frames the frametable consumes

/*
 * This function allocate a frame cap and then map it to the indicated KVADDR
 *
 * Return: 0 iff success
 */
static int
_map_to_sel4(const seL4_ARM_PageDirectory pd, const seL4_Word kvaddr, seL4_Word *paddr, seL4_CPtr *cap) {

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
    int err = map_page(*cap, pd, kvaddr, seL4_AllRights, seL4_ARM_Default_VMAttributes);
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
    frametable = (frame_entry_t*)ID_TO_KVADDR(0);

    /* Allocate memory for frametable to use */
    size_t i = 0;
    for (; i*PAGE_SIZE < frametable_sz; i++) {
        seL4_Word kvaddr = (seL4_Word)ID_TO_KVADDR(i);
        seL4_Word tmp_paddr;
        seL4_CPtr tmp_cap;
        int err = _map_to_sel4(seL4_CapInitThreadPD, kvaddr, &tmp_paddr, &tmp_cap);
        if (err) {
            return err;
        }

        frametable[i].fte_paddr     = tmp_paddr;
        frametable[i].fte_cap       = tmp_cap;
        frametable[i].fte_kvaddr    = kvaddr;
        frametable[i].fte_status    = FRAME_STATUS_ALLOCATED;
        frametable[i].fte_pd        = seL4_CapInitThreadPD;
        frametable[i].fte_next_free = FRAME_INVALID;
        frametable[i].fte_locked    = false;
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

    //this is for rand chance swap, TODO remove me when we upgrade
    //to a better page replacement algorithm
    srand(1);
    frame_initialised = true;

    return 0;
}

bool frame_has_free(void) {
    if (!frame_initialised) {
        return false;
    }
    return (first_free != FRAME_INVALID);
}

static seL4_Word
rand_swap_victim(){
    int id = rand() % NFRAMES;
    //TODO: check if frame is unswappable and not locked
    return ID_TO_KVADDR(id);
}

typedef struct {
   frame_alloc_cb_t callback;
   void* token;
   int victim_id;
} frame_alloc_cont_t;

static void
frame_alloc_end(void* token, int err){
    printf("frame alloc end\n");

    if (token == NULL) {
        printf("error: frame_alloc_end: shit happened\n");
        return;
    }
    frame_alloc_cont_t* cont = (frame_alloc_cont_t*)token;
    if(err){
        cont->callback(cont->token, 0);
        free(cont);
        return;
    }

    /* Make sure that we have free frame */
    if (first_free == FRAME_INVALID) {
        // This should not happen though because of swapping
        printf("warning: frame_alloc_end: failed in getting a free frame\n");
        cont->callback(cont->token, 0);
        free(cont);
        return;
    }

    int ind = first_free;
    printf("first free = %d\n",ind);
    seL4_Word kvaddr = (seL4_Word)ID_TO_KVADDR(ind);

    //If this assert fails then our first_free is buggy
    assert(frametable[ind].fte_status != FRAME_STATUS_ALLOCATED);

    if(frametable[ind].fte_status == FRAME_STATUS_FREE){
        /* If the frame has been typed, we simple allocated it */

        frametable[ind].fte_status = FRAME_STATUS_ALLOCATED;
        frametable[ind].fte_kvaddr = kvaddr;
        frametable[ind].fte_pd     = seL4_CapInitThreadPD;
        frametable[ind].fte_locked = false;
    } else {
        /* Else we have to typed it */

        /* Allocate and map this frame */
        err = _map_to_sel4(seL4_CapInitThreadPD, kvaddr,
                           &frametable[ind].fte_paddr, &frametable[ind].fte_cap);
        if (err) {
            cont->callback(cont->token, 0);
            free(cont);
            return;
        }

        frametable[ind].fte_status = FRAME_STATUS_ALLOCATED;
        frametable[ind].fte_kvaddr = kvaddr;
        frametable[ind].fte_pd     = seL4_CapInitThreadPD;
        frametable[ind].fte_locked = false;
    }

    /* Zero fill memory */
    bzero((void *)(kvaddr), (size_t)PAGE_SIZE);

    /* Update free frame list */
    first_free = frametable[ind].fte_next_free;

    printf("new first free = %d\n",first_free);
    cont->callback(cont->token, kvaddr);
    free(cont);
}

static void
frame_alloc_swap_out_cb(void *token, int err) {
    printf("frame alloc swapout cb\n");
    assert(token != NULL); // if this is NULL, memory is corrupted
    frame_alloc_cont_t* cont = (frame_alloc_cont_t*)token;
    if(err){
        printf("frame alloc cb err\n");
        cont->callback(cont->token, 0);
        free(cont);
        return;
    }

    /* Update free frame list */
    //swap out already calls frame free which update the free frame list
    printf("first free after swap out = %d\n",first_free);
    //frametable[cont->victim_id].fte_next_free = first_free;
    //first_free = cont->victim_id;

    frame_alloc_end((void*)cont, 0);
}

int frame_alloc(frame_alloc_cb_t callback, void* token){
    printf("frame alloc\n");

    if (!frame_initialised) {
        callback(token, 0);
        return EFAULT;
    }

    frame_alloc_cont_t *cont = malloc(sizeof(frame_alloc_cb_t));
    if(cont == NULL){
        callback(token, 0);
        return EFAULT;
    }
    cont->callback = callback;
    cont->token = token;

    /* If we do not have enough memory, start swapping frames out */
    if(first_free == FRAME_INVALID) {
        printf("frame alloc no memory\n");
        seL4_Word kvaddr = rand_swap_victim();
        // the frame returned is not locked

        int ind = (int)KVADDR_TO_ID(kvaddr);
        cont->victim_id = ind;
        swap_out(kvaddr, frame_alloc_swap_out_cb, (void*)cont); 
        return 0;
    }

    /* Else we have free frames to allocate */
    frame_alloc_end((void*)cont, 0);
    return 0;
}

int frame_free(seL4_Word kvaddr){
    /* May have concurency issues */
    printf("frame free\n");

    if (!frame_initialised) {
        /* Why is frame uninitialised? */
        return EFAULT;
    }

    int id = (int)KVADDR_TO_ID(kvaddr);
    if (id < frametable_reserved || id >= NFRAMES) {
    printf("frame free err1\n");
        return EINVAL;
    }
    if(frametable[id].fte_status != FRAME_STATUS_ALLOCATED) {
    printf("frame free err2\n");
        return EINVAL;
    }

    //frame to be freed should not be locked
    if(frametable[id].fte_locked) {
    printf("frame free err3\n");
        //dont crash sos
        return EFAULT;
    }


    /* Optimization: we only actually untype the memory sometimes
     * Other times, we only reset the status of the frame to FREE */
    //TODO update this condition/heuristic
    if (true) {
        /* We dont actually free the frame */
        frametable[id].fte_status    = FRAME_STATUS_FREE;
    } else {
        seL4_Word paddr = frametable[id].fte_paddr;
        seL4_CPtr frame_cap = frametable[id].fte_cap;

        /* "Freeing" the frame */
        int err = seL4_ARM_Page_Unmap(frame_cap);
        if (err) {
            return EFAULT;
        }

        // set here so if fails after, the frame is still usable
        frametable[id].fte_status = FRAME_STATUS_UNTYPED;

        cspace_err_t cspace_err = cspace_delete_cap(cur_cspace, frame_cap);
        if (cspace_err != CSPACE_NOERROR) {
            return EFAULT;
        }

        ut_free(paddr, seL4_PageBits);
    }

    /* Clear other fields */
    frametable[id].fte_locked = false;
    frametable[id].fte_pd     = 0;

    /* Update free frame list */
    frametable[id].fte_next_free = first_free;
    first_free = id;

    return 0;
}

int
frame_get_cap(seL4_Word kvaddr, seL4_CPtr *frame_cap) {
    *frame_cap = -1;
    if (!frame_initialised) {
        return EFAULT;
    }

    int id = (int)KVADDR_TO_ID(kvaddr);
    if (id < frametable_reserved || id >= NFRAMES) {
        return EINVAL;
    }
    if(frametable[id].fte_status != FRAME_STATUS_ALLOCATED) {
        return EINVAL;
    }
    *frame_cap = frametable[id].fte_cap;
    return 0;
}

int
frame_is_locked(seL4_Word kvaddr, bool *is_locked) {
     if (!frame_initialised) {
        return EFAULT;
    }

    int id = (int)KVADDR_TO_ID(kvaddr);
    if (id < frametable_reserved || id >= NFRAMES) {
        return EINVAL;
    }
    if(frametable[id].fte_status != FRAME_STATUS_ALLOCATED) {
        return EINVAL;
    }
    *is_locked = frametable[id].fte_locked;
    return 0;
}

int
frame_lock_frame(seL4_Word kvaddr){
     if (!frame_initialised) {
        return EFAULT;
    }

    int id = (int)KVADDR_TO_ID(kvaddr);
    if (id < frametable_reserved || id >= NFRAMES) {
        return EINVAL;
    }
    if(frametable[id].fte_status != FRAME_STATUS_ALLOCATED) {
        return EINVAL;
    }

    //might need to do this atomically?
    if(frametable[id].fte_locked == true) {
        return EINVAL;
    } else {
        frametable[id].fte_locked = true;
        return 0;
    }
}

int
frame_unlock_frame(seL4_Word kvaddr){
     if (!frame_initialised) {
        return EFAULT;
    }

    int id = (int)KVADDR_TO_ID(kvaddr);
    if (id < frametable_reserved || id >= NFRAMES) {
        return EINVAL;
    }
    if(frametable[id].fte_status != FRAME_STATUS_ALLOCATED) {
        return EINVAL;
    }

    //might need to do this atomically?
    if(frametable[id].fte_locked == false) {
        return EINVAL;
    } else {
        frametable[id].fte_locked = false;
        return 0;
    }
}

seL4_Word get_free_frame_kvaddr(){
    if(first_free != FRAME_INVALID){
        return (seL4_Word)ID_TO_KVADDR(first_free);
    }
    return FRAME_INVALID;
}
