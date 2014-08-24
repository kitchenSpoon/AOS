#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <limits.h>
#include <sel4/sel4.h>
#include <cspace/cspace.h>
#include <ut_manager/ut.h>
#include <strings.h>

#include "vm.h"
#include "mapping.h"
#include "vmem_layout.h"
#include "addrspace.h"
#include "proc.h"

#define NFRAMES                  (FRAME_MEMORY / PAGE_SIZE)

#define FRAME_STATUS_UNTYPED     0
#define FRAME_STATUS_FREE        1
#define FRAME_STATUS_ALLOCATED   2

#define FRAME_INVALID            (-1)

#define ID_TO_VADDR(id)     ((id)*PAGE_SIZE + FRAME_VSTART) 
#define VADDR_TO_ID(vaddr)  (((vaddr) - FRAME_VSTART) / PAGE_SIZE)

#define PAGEMASK              ((PAGESIZE) - 1)
#define PAGE_ALIGN(addr)      ((addr) & ~(PAGEMASK))
#define IS_PAGESIZE_ALIGNED(addr) !((addr) &  (PAGEMASK))

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

seL4_Word frame_alloc(void){

    if (!frame_initialised) {
        return 0;
    }
    if(first_free == FRAME_INVALID) {
        return 0;
    }

    int result;
    int ind = first_free;

    /* Allocate and map this frame */
    seL4_Word vaddr = (seL4_Word)ID_TO_VADDR(ind);

    result = _map_to_sel4(seL4_CapInitThreadPD, vaddr,
                          &frametable[ind].fte_paddr, &frametable[ind].fte_cap);
    if (result != FRAME_IS_OK) {
        return 0;
    }

    frametable[ind].fte_status = FRAME_STATUS_ALLOCATED;
    frametable[ind].fte_vaddr  = vaddr;
    frametable[ind].fte_pd     = seL4_CapInitThreadPD;

    /* Zero fill memory */
    bzero((void *)(vaddr), (size_t)PAGE_SIZE);

    /* Update free frame list */
    first_free = frametable[ind].fte_next_free;

    assert(IS_PAGESIZE_ALIGNED(vaddr));
    /* Now update the vaddr */
    return vaddr;
}

int frame_free(seL4_Word vaddr){
    /* May have concurency issues */
    
    if (!frame_initialised) {
        return FRAME_IS_UNINT;
    }

    int id = (int)VADDR_TO_ID(vaddr);
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
frame_get_cap(seL4_Word vaddr) {
    if (!frame_initialised) {
        return FRAME_IS_UNINT;
    }

    int id = (int)VADDR_TO_ID(vaddr);
    if (id < frametable_reserved || id >= NFRAMES) {
        return FRAME_IS_FAIL;
    }
    if(frametable[id].fte_status != FRAME_STATUS_ALLOCATED) {
        return FRAME_IS_FAIL;
    }
    return frametable[id].fte_cap;
}

static
region_t*
_region_probe(struct addrspace* as, seL4_Word addr) {
    assert(as != NULL);
    assert(addr != 0);

    if(as->as_stack != NULL && as->as_stack->vbase <= addr && addr < as->as_stack->vtop)
        return as->as_stack;

    if(as->as_heap != NULL && as->as_heap->vbase <= addr && addr < as->as_heap->vtop)
        return as->as_heap;

    for (region_t *r = as->as_rhead; r != NULL; r = r->next) {
        if (r->vbase <= addr && addr < r->vtop) {
            return r;
        }
    }
    return NULL;
}

int
sos_VMFaultHandler(seL4_Word fault_addr, int fault_type){
    if (fault_addr == 0) {
        /* Derefenrecing NULL? */
        return EINVAL;
    }

    addrspace_t *as = proc_getas();
    if (as == NULL) {
        /* Kernel is probably failed when bootstraping */
        return EFAULT;
    }

    int result;

    /* Check if the fault address is in a valid region */
    region_t* reg = _region_probe(as, fault_addr);
    if(reg != NULL){
        /* If yes, map a page for this address to use */
        seL4_Word kvaddr;
        result = sos_page_map(as, proc_getvroot(), fault_addr, &kvaddr);
        if (result) {
            return result;
        }

        return 0;
    }

    return EFAULT;
}
