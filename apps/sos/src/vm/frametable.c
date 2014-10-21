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

#define NFRAMES                  ((FRAME_MEMORY) / (PAGE_SIZE))

#define FRAME_STATUS_UNTYPED     (0)
#define FRAME_STATUS_FREE        (1)
#define FRAME_STATUS_ALLOCATED   (2)

#define FRAME_INVALID            (-1)

#define ID_TO_KVADDR(id)     ((id)*PAGE_SIZE + FRAME_VSTART)
#define KVADDR_TO_ID(kvaddr)  (((kvaddr) - FRAME_VSTART) / PAGE_SIZE)

/* Frame table entry structure */
typedef struct {
    int fte_status;
    seL4_CPtr fte_cap;
    seL4_Word fte_paddr;
    seL4_Word fte_kvaddr;
    seL4_Word fte_vaddr;
    addrspace_t *fte_as;  //Addrspace, we may change this to use ASID instead
    pid_t fte_pid;
    //TODO: fuse all these fields into 1 var
    int fte_next_free;
    bool fte_locked;
    bool fte_noswap;
    bool fte_referenced;
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

        frametable[i].fte_status        = FRAME_STATUS_ALLOCATED;
        frametable[i].fte_paddr         = tmp_paddr;
        frametable[i].fte_cap           = tmp_cap;
        frametable[i].fte_kvaddr        = kvaddr;
        frametable[i].fte_vaddr         = 0;
        frametable[i].fte_as            = NULL;
        frametable[i].fte_pid           = PROC_NULL;
        frametable[i].fte_next_free     = FRAME_INVALID;
        frametable[i].fte_locked        = false;
        frametable[i].fte_noswap        = true;
        frametable[i].fte_referenced    = true;
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

    //this is for rand chance swap
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

/*
 * This function only called when there is no memory left
 * i.e. assuming that all frames are allocated
 */
static seL4_Word
rand_swap_victim(){
    int id = rand() % NFRAMES;
    while (frametable[id].fte_noswap) {
        id = rand() % NFRAMES;
    }

    printf("rand_swap_victim kvaddr = 0x%08x\n", ID_TO_KVADDR(id));
    return ID_TO_KVADDR(id);
}

int victim = 0;

/*
 * This function assumes that all frames are currently allocated
 */
static seL4_Word
second_chance_swap_victim(){
    bool found = false;
    int cnt = 0;
    while(!found && cnt < 3*NFRAMES){
    //while(!found){
        cnt++;
        if(frametable[victim].fte_noswap || frametable[victim].fte_locked){
            victim++;
            victim = victim % NFRAMES;
        } else if(frametable[victim].fte_referenced){
            addrspace_t* as = frametable[victim].fte_as;
    printf("assssssssssssssssssss2 as->as_pd_regs = %p\n", as->as_pd_regs);
            seL4_Word vaddr = frametable[victim].fte_vaddr;
            int err = sos_page_unmap(as, vaddr);
            if (err) {
                printf("second_chance_swap failed to unmap page\n");
            }

            frametable[victim].fte_referenced = false;
            printf("second chance unmap this kvaddr -> 0x%08x, vaddr = 0x%08x\n",
                    frametable[victim].fte_kvaddr, vaddr);
            victim++;
            victim = victim % NFRAMES;
            continue;
        } else {
            //you are killed
            //I may need to increase victim here after returning
            int id = victim;
            victim++;
            victim = victim % NFRAMES;
            seL4_Word vaddr = frametable[id].fte_vaddr;
            printf("second_chance_swap_victim kvaddr = 0x%08x, vaddr = 0x%08x\n", ID_TO_KVADDR(id), vaddr);
            return ID_TO_KVADDR(id);
        }
    }

    printf("second_chance_swap cannot find a victim to swap out\n");
    return 0;
}

typedef struct {
   frame_alloc_cb_t callback;
   void* token;
   addrspace_t* as;
   seL4_Word vaddr;
   pid_t pid;
   bool noswap;
} frame_alloc_cont_t;

static void
frame_alloc_end(void* token, int err){
    printf("frame_alloc end\n");
    assert(token != NULL);
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
    seL4_Word kvaddr = (seL4_Word)ID_TO_KVADDR(ind);
    printf("frame_alloc memory = 0x%08x\n", kvaddr);

    //If this assert fails then our first_free is buggy
    assert(frametable[ind].fte_status != FRAME_STATUS_ALLOCATED);

    if(frametable[ind].fte_status == FRAME_STATUS_UNTYPED){
        /* Allocate and map this frame */
        err = _map_to_sel4(seL4_CapInitThreadPD, kvaddr,
                           &frametable[ind].fte_paddr, &frametable[ind].fte_cap);
        if (err) {
            cont->callback(cont->token, 0);
            free(cont);
            return;
        }
    }
    frametable[ind].fte_status     = FRAME_STATUS_ALLOCATED;
    frametable[ind].fte_kvaddr     = kvaddr;
    frametable[ind].fte_vaddr      = cont->vaddr;
    frametable[ind].fte_as         = cont->as;
    frametable[ind].fte_pid        = cont->pid;
    frametable[ind].fte_noswap     = cont->noswap;
    frametable[ind].fte_referenced = true;
    frametable[ind].fte_locked     = false;

    /* Zero fill memory */
    bzero((void *)(kvaddr), (size_t)PAGE_SIZE);

    printf("first_free = %d, new_first_free = %d\n", first_free, frametable[ind].fte_next_free);

    /* Update free frame list */
    first_free = frametable[ind].fte_next_free;

    cont->callback(cont->token, kvaddr);
    free(cont);
}

int
frame_alloc(seL4_Word vaddr, addrspace_t* as, pid_t pid, bool noswap,
                frame_alloc_cb_t callback, void* token){
    printf("frame_alloc called, noswap = %d\n", (int)noswap);

    if (!frame_initialised) {
        return EFAULT;
    }

    //if as == NULL, it means the kernel is calling frame_alloc for itself
    if(as != NULL && vaddr == 0){
        return EINVAL;
    }

    frame_alloc_cont_t *cont = malloc(sizeof(frame_alloc_cont_t));
    if(cont == NULL){
        return ENOMEM;
    }
    cont->callback = callback;
    cont->token = token;
    cont->vaddr = PAGE_ALIGN(vaddr);
    cont->as = as;
    cont->pid = pid;
    cont->noswap = noswap;

    /* If we do not have enough memory, start swapping frames out */
    if(first_free == FRAME_INVALID) {
        printf("frame alloc no memory\n");
        //seL4_Word kvaddr = rand_swap_victim();
        seL4_Word kvaddr = second_chance_swap_victim();
        // the frame returned is not locked
        if (kvaddr == 0) {
            free(cont);
            return ENOMEM;
        }

        swap_out(kvaddr, frame_alloc_end, (void*)cont);
        return 0;
    }

    /* Else we have free frames to allocate */
    frame_alloc_end((void*)cont, 0);
    return 0;
}

int frame_free(seL4_Word kvaddr){
    /* May have concurency issues */
    printf("frame_free\n");

    if (!frame_initialised) {
        /* Why is frame uninitialised? */
        return EFAULT;
    }

    int id = (int)KVADDR_TO_ID(kvaddr);
    if (id < frametable_reserved || id >= NFRAMES) {
        printf("frame_free err1\n");
        return EINVAL;
    }
    if(frametable[id].fte_status != FRAME_STATUS_ALLOCATED) {
        printf("frame_free err2\n");
        return EINVAL;
    }

    //frame to be freed should not be locked
    if(frametable[id].fte_locked) {
        printf("frame_free err3\n");
        //dont crash sos
        return EFAULT;
    }


    /* Optimization: we only actually untype the memory sometimes
     * Other times, we only reset the status of the frame to FREE */
    //TODO update this condition/heuristic
    if (true) {
        /* We dont actually free the frame */
        frametable[id].fte_status = FRAME_STATUS_FREE;
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

    /* Update free frame list */
    frametable[id].fte_next_free = first_free;
    first_free = id;

    return 0;
}

int
frame_get_cap(seL4_Word kvaddr, seL4_CPtr *frame_cap) {
    *frame_cap = -1;
    if (!frame_initialised) {
        printf("frame get cap 1\n");
        return EFAULT;
    }

    int id = (int)KVADDR_TO_ID(kvaddr);
    if (id < frametable_reserved || id >= NFRAMES) {
        printf("frame get cap 2\n");
        return EINVAL;
    }
    if(frametable[id].fte_status != FRAME_STATUS_ALLOCATED) {
        printf("frame get cap 3\n");
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

addrspace_t* frame_get_as(seL4_Word kvaddr){
    int id = (int)KVADDR_TO_ID(kvaddr);
    if (id < frametable_reserved || id >= NFRAMES) {
        return NULL;
    }
    if(frametable[id].fte_status != FRAME_STATUS_ALLOCATED) {
        return NULL;
    }
    return frametable[id].fte_as;
}

pid_t frame_get_pid(seL4_Word kvaddr) {
    int id = (int)KVADDR_TO_ID(kvaddr);
    if (id < frametable_reserved || id >= NFRAMES) {
        return PROC_NULL;
    }
    if(frametable[id].fte_status != FRAME_STATUS_ALLOCATED) {
        return PROC_NULL;
    }
    return frametable[id].fte_pid;
}

seL4_Word frame_get_vaddr(seL4_Word kvaddr){
    int id = (int)KVADDR_TO_ID(kvaddr);
    if (id < frametable_reserved || id >= NFRAMES) {
        return 0;
    }
    if(frametable[id].fte_status != FRAME_STATUS_ALLOCATED) {
        return 0;
    }
    return frametable[id].fte_vaddr;
}

int set_frame_referenced(seL4_Word kvaddr){
    int id = (int)KVADDR_TO_ID(kvaddr);
    if (id < frametable_reserved || id >= NFRAMES) {
        return EINVAL;
    }

    frametable[id].fte_referenced = true;
    return 0;
}

bool is_frame_referenced(seL4_Word kvaddr){
    int id = (int)KVADDR_TO_ID(kvaddr);
    if (id < frametable_reserved || id >= NFRAMES) {
        return EINVAL;
    }

    return frametable[id].fte_referenced;
}
