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


static frame_entry_t *_frametable;
static int _first_free;                     // Index of the first free/untyped frame
static bool _frame_initialised;
static size_t _frametable_reserved;           // # of frames the _frametable consumes

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
    _frametable = (frame_entry_t*)ID_TO_KVADDR(0);

    /* Allocate memory for _frametable to use */
    size_t i = 0;
    for (; i*PAGE_SIZE < frametable_sz; i++) {
        seL4_Word kvaddr = (seL4_Word)ID_TO_KVADDR(i);
        seL4_Word tmp_paddr;
        seL4_CPtr tmp_cap;
        int err = _map_to_sel4(seL4_CapInitThreadPD, kvaddr, &tmp_paddr, &tmp_cap);
        if (err) {
            return err;
        }

        _frametable[i].fte_status        = FRAME_STATUS_ALLOCATED;
        _frametable[i].fte_paddr         = tmp_paddr;
        _frametable[i].fte_cap           = tmp_cap;
        _frametable[i].fte_kvaddr        = kvaddr;
        _frametable[i].fte_vaddr         = 0;
        _frametable[i].fte_as            = NULL;
        _frametable[i].fte_pid           = PROC_NULL;
        _frametable[i].fte_next_free     = FRAME_INVALID;
        _frametable[i].fte_locked        = false;
        _frametable[i].fte_noswap        = true;
        _frametable[i].fte_referenced    = true;
    }

    /* Mark the number of frames occupied by the _frametable */
    _frametable_reserved = i;

    /* The ith frame is the first free frame */
    _first_free = i;

    /* Initialise the remaining frames */
    for (; i<NFRAMES; i++) {
        _frametable[i].fte_status = FRAME_STATUS_UNTYPED;
        _frametable[i].fte_next_free = (i == NFRAMES-1) ? FRAME_INVALID : i+1;
    }

    //this is for rand chance swap
    srand(1);
    _frame_initialised = true;

    return 0;
}

bool frame_has_free(void) {
    if (!_frame_initialised) {
        return false;
    }
    return (_first_free != FRAME_INVALID);
}

/*
 * This function only called when there is no memory left
 * i.e. assuming that all frames are allocated
 */
static seL4_Word
_rand_swap_victim(){
    int id = rand() % NFRAMES;
    while (_frametable[id].fte_noswap) {
        id = rand() % NFRAMES;
    }

    printf("_rand_swap_victim kvaddr = 0x%08x\n", ID_TO_KVADDR(id));
    return ID_TO_KVADDR(id);
}

int victim = 0;

/*
 * This function assumes that all frames are currently allocated
 */
static seL4_Word
_second_chance_swap_victim(){
    bool found = false;
    int cnt = 0;
    while(!found && cnt < 3*NFRAMES){
    //while(!found){
        cnt++;
        if(_frametable[victim].fte_noswap || _frametable[victim].fte_locked){
            victim++;
            victim = victim % NFRAMES;
        } else if(_frametable[victim].fte_referenced){
            addrspace_t* as = _frametable[victim].fte_as;
    printf("assssssssssssssssssss2 as->as_pd_regs = %p\n", as->as_pd_regs);
            seL4_Word vaddr = _frametable[victim].fte_vaddr;
            int err = sos_page_unmap(as, vaddr);
            if (err) {
                printf("second_chance_swap failed to unmap page\n");
            }

            _frametable[victim].fte_referenced = false;
            printf("second chance unmap this kvaddr -> 0x%08x, vaddr = 0x%08x\n",
                    _frametable[victim].fte_kvaddr, vaddr);
            victim++;
            victim = victim % NFRAMES;
            continue;
        } else {
            //you are killed
            //I may need to increase victim here after returning
            int id = victim;
            victim++;
            victim = victim % NFRAMES;
            seL4_Word vaddr = _frametable[id].fte_vaddr;
            printf("_second_chance_swap_victim kvaddr = 0x%08x, vaddr = 0x%08x\n", ID_TO_KVADDR(id), vaddr);
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

static void _frame_alloc_end(void* token, int err);

int
frame_alloc(seL4_Word vaddr, addrspace_t* as, pid_t pid, bool noswap,
                frame_alloc_cb_t callback, void* token){
    printf("frame_alloc called, noswap = %d\n", (int)noswap);

    if (!_frame_initialised) {
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
    if(_first_free == FRAME_INVALID) {
        printf("frame alloc no memory\n");
        //seL4_Word kvaddr = _rand_swap_victim();
        seL4_Word kvaddr = _second_chance_swap_victim();
        // the frame returned is not locked
        if (kvaddr == 0) {
            free(cont);
            return ENOMEM;
        }

        swap_out(kvaddr, _frame_alloc_end, (void*)cont);
        return 0;
    }

    /* Else we have free frames to allocate */
    _frame_alloc_end((void*)cont, 0);
    return 0;
}

static void
_frame_alloc_end(void* token, int err){
    printf("frame_alloc end\n");
    assert(token != NULL);
    frame_alloc_cont_t* cont = (frame_alloc_cont_t*)token;
    if(err){
        cont->callback(cont->token, 0);
        free(cont);
        return;
    }

    /* Make sure that we have free frame */
    if (_first_free == FRAME_INVALID) {
        // This should not happen though because of swapping
        printf("warning: _frame_alloc_end: failed in getting a free frame\n");
        cont->callback(cont->token, 0);
        free(cont);
        return;
    }

    int ind = _first_free;
    seL4_Word kvaddr = (seL4_Word)ID_TO_KVADDR(ind);
    printf("frame_alloc memory = 0x%08x\n", kvaddr);

    //If this assert fails then our _first_free is buggy
    assert(_frametable[ind].fte_status != FRAME_STATUS_ALLOCATED);

    if(_frametable[ind].fte_status == FRAME_STATUS_UNTYPED){
        /* Allocate and map this frame */
        err = _map_to_sel4(seL4_CapInitThreadPD, kvaddr,
                           &_frametable[ind].fte_paddr, &_frametable[ind].fte_cap);
        if (err) {
            cont->callback(cont->token, 0);
            free(cont);
            return;
        }
    }
    _frametable[ind].fte_status     = FRAME_STATUS_ALLOCATED;
    _frametable[ind].fte_kvaddr     = kvaddr;
    _frametable[ind].fte_vaddr      = cont->vaddr;
    _frametable[ind].fte_as         = cont->as;
    _frametable[ind].fte_pid        = cont->pid;
    _frametable[ind].fte_noswap     = cont->noswap;
    _frametable[ind].fte_referenced = true;
    _frametable[ind].fte_locked     = false;

    /* Zero fill memory */
    bzero((void *)(kvaddr), (size_t)PAGE_SIZE);

    printf("_first_free = %d, new_first_free = %d\n", _first_free, _frametable[ind].fte_next_free);

    /* Update free frame list */
    _first_free = _frametable[ind].fte_next_free;

    cont->callback(cont->token, kvaddr);
    free(cont);
}

int frame_free(seL4_Word kvaddr){
    /* May have concurency issues */
    printf("frame_free\n");

    if (!_frame_initialised) {
        /* Why is frame uninitialised? */
        return EFAULT;
    }

    int id = (int)KVADDR_TO_ID(kvaddr);
    if (id < _frametable_reserved || id >= NFRAMES) {
        printf("frame_free err1\n");
        return EINVAL;
    }
    if(_frametable[id].fte_status != FRAME_STATUS_ALLOCATED) {
        printf("frame_free err2\n");
        return EINVAL;
    }

    //frame to be freed should not be locked
    if(_frametable[id].fte_locked) {
        printf("frame_free err3\n");
        //dont crash sos
        return EFAULT;
    }


    /* Optimization: we only actually untype the memory sometimes
     * Other times, we only reset the status of the frame to FREE */
    //TODO update this condition/heuristic
    if (true) {
        /* We dont actually free the frame */
        _frametable[id].fte_status = FRAME_STATUS_FREE;
    } else {
        seL4_Word paddr = _frametable[id].fte_paddr;
        seL4_CPtr frame_cap = _frametable[id].fte_cap;

        /* "Freeing" the frame */
        int err = seL4_ARM_Page_Unmap(frame_cap);
        if (err) {
            return EFAULT;
        }

        // set here so if fails after, the frame is still usable
        _frametable[id].fte_status = FRAME_STATUS_UNTYPED;

        cspace_err_t cspace_err = cspace_delete_cap(cur_cspace, frame_cap);
        if (cspace_err != CSPACE_NOERROR) {
            return EFAULT;
        }

        ut_free(paddr, seL4_PageBits);
    }

    /* Clear other fields */
    _frametable[id].fte_locked = false;

    /* Update free frame list */
    _frametable[id].fte_next_free = _first_free;
    _first_free = id;

    return 0;
}

int
frame_get_cap(seL4_Word kvaddr, seL4_CPtr *frame_cap) {
    *frame_cap = -1;
    if (!_frame_initialised) {
        printf("frame get cap 1\n");
        return EFAULT;
    }

    int id = (int)KVADDR_TO_ID(kvaddr);
    if (id < _frametable_reserved || id >= NFRAMES) {
        printf("frame get cap 2\n");
        return EINVAL;
    }
    if(_frametable[id].fte_status != FRAME_STATUS_ALLOCATED) {
        printf("frame get cap 3\n");
        return EINVAL;
    }
    *frame_cap = _frametable[id].fte_cap;
    return 0;
}

int
frame_is_locked(seL4_Word kvaddr, bool *is_locked) {
     if (!_frame_initialised) {
        return EFAULT;
    }

    int id = (int)KVADDR_TO_ID(kvaddr);
    if (id < _frametable_reserved || id >= NFRAMES) {
        return EINVAL;
    }
    if(_frametable[id].fte_status != FRAME_STATUS_ALLOCATED) {
        return EINVAL;
    }
    *is_locked = _frametable[id].fte_locked;
    return 0;
}

int
frame_lock_frame(seL4_Word kvaddr){
     if (!_frame_initialised) {
        return EFAULT;
    }

    int id = (int)KVADDR_TO_ID(kvaddr);
    if (id < _frametable_reserved || id >= NFRAMES) {
        return EINVAL;
    }
    if(_frametable[id].fte_status != FRAME_STATUS_ALLOCATED) {
        return EINVAL;
    }

    //might need to do this atomically?
    if(_frametable[id].fte_locked == true) {
        return EINVAL;
    } else {
        _frametable[id].fte_locked = true;
        return 0;
    }
}

int
frame_unlock_frame(seL4_Word kvaddr){
     if (!_frame_initialised) {
        return EFAULT;
    }

    int id = (int)KVADDR_TO_ID(kvaddr);
    if (id < _frametable_reserved || id >= NFRAMES) {
        return EINVAL;
    }
    if(_frametable[id].fte_status != FRAME_STATUS_ALLOCATED) {
        return EINVAL;
    }

    //might need to do this atomically?
    if(_frametable[id].fte_locked == false) {
        return EINVAL;
    } else {
        _frametable[id].fte_locked = false;
        return 0;
    }
}

addrspace_t* frame_get_as(seL4_Word kvaddr){
    int id = (int)KVADDR_TO_ID(kvaddr);
    if (id < _frametable_reserved || id >= NFRAMES) {
        return NULL;
    }
    if(_frametable[id].fte_status != FRAME_STATUS_ALLOCATED) {
        return NULL;
    }
    return _frametable[id].fte_as;
}

pid_t frame_get_pid(seL4_Word kvaddr) {
    int id = (int)KVADDR_TO_ID(kvaddr);
    if (id < _frametable_reserved || id >= NFRAMES) {
        return PROC_NULL;
    }
    if(_frametable[id].fte_status != FRAME_STATUS_ALLOCATED) {
        return PROC_NULL;
    }
    return _frametable[id].fte_pid;
}

seL4_Word frame_get_vaddr(seL4_Word kvaddr){
    int id = (int)KVADDR_TO_ID(kvaddr);
    if (id < _frametable_reserved || id >= NFRAMES) {
        return 0;
    }
    if(_frametable[id].fte_status != FRAME_STATUS_ALLOCATED) {
        return 0;
    }
    return _frametable[id].fte_vaddr;
}

int frame_set_referenced(seL4_Word kvaddr){
    int id = (int)KVADDR_TO_ID(kvaddr);
    if (id < _frametable_reserved || id >= NFRAMES) {
        return EINVAL;
    }

    _frametable[id].fte_referenced = true;
    return 0;
}

bool is_frame_referenced(seL4_Word kvaddr){
    int id = (int)KVADDR_TO_ID(kvaddr);
    if (id < _frametable_reserved || id >= NFRAMES) {
        return EINVAL;
    }

    return _frametable[id].fte_referenced;
}
