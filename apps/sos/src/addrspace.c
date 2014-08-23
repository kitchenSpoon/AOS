#include <errno.h>
#include <limits.h>
#include "addrspace.h"
#include "vm.h"

#define N_PAGETABLES       (1024)
#define DIVROUNDUP(a,b) (((a)+(b)-1)/(b))

addrspace_t
*as_create(void) {
    addrspace_t* as = malloc(sizeof(addrspace_t));
    if (as == NULL)
        return as;
    
    /* Initialise page directory */
    seL4_Word vaddr = frame_alloc();
    as->as_pd = (pagedir_t)vaddr;
    if (as->as_pd == NULL) {
        free(as);
        return NULL;
    }

    for (int i=0; i<N_PAGETABLES; i++) {
        as->as_pd[i] = NULL;
    }

    /* Initialise the remaining values */
    as->as_rhead   = NULL;
    as->as_stack   = NULL;
    as->as_heap    = NULL;
    as->as_loading = false;

    return as;
}

void
as_destroy(addrspace_t *as) {
    //TODO: clean up page table and also seL4's caps related to this as
    (void)as;
    if(as == NULL){
        return;
    }

    //if(as_loading){
        //do something??
    //}

    //Free page directory
    if(as->as_pd != NULL){
        //TODO actually free them
        for(int i = 0; i < N_PAGETABLES; i++){
            if(as->as_pd[i] != NULL){
                frame_free((seL4_Word)as->as_pd[i]);
            }
        }
    }

    //Free heap
    //Free stack
    //Free regionhead

}

static int
_region_overlap(region_t* r1, region_t* r2) {
    assert(r1 != NULL && r2 != NULL);

    bool r1_left_r2 = (r1->vbase < r2->vbase && r1->vtop <= r2->vbase);
    bool r1_right_r2 = (r1->vbase >= r2->vtop && r1->vtop > r2->vtop);
    return !(r1_left_r2 || r1_right_r2);
}

/*
 * Initialise the new reigon and make sure it does not overlap with other
 * regions
 */
static int 
_region_init(addrspace_t *as, seL4_Word vaddr, size_t sz,
        int rights, struct region* nregion)
{
    assert(as != NULL);

    nregion->vbase = vaddr;
    nregion->vtop = vaddr + sz;
    nregion->rights = rights;
    nregion->next = NULL;

    /*
     * Since we always assume that the heap is the last region to be define
     * we do not check if any region overlaps with the heap
     */
    if(as->as_stack != NULL && _region_overlap(nregion, as->as_stack)){
        return EINVAL;
    }

    for (region_t *r = as->as_rhead; r != NULL; r = r->next) {
        if (_region_overlap(nregion, r)) {
            return EINVAL;
        }
    }
    return 0;
}

int
as_define_region(addrspace_t *as, seL4_Word vaddr, size_t sz, int32_t rights) {
    assert(as != NULL);

    region_t* nregion = malloc(sizeof(region_t));
    if (nregion == NULL) {
        return ENOMEM;
    }
    int result = _region_init(as, vaddr, sz, rights, nregion);
    if (result) {
        return result;
    }

    /* Add the new region to addrspace's region list */
    nregion->next = as->as_rhead;
    as->as_rhead = nregion;

    return 0;
}

int
as_define_stack(addrspace_t *as, seL4_Word stack_top, int size) {
    if (as == NULL)
        return EINVAL;
    region_t *stack = malloc(sizeof(region_t));
    if (stack == NULL) {
        return ENOMEM;
    }
    int result = _region_init(as, stack_top, size, AS_REGION_ALL, stack);
    if (result) {
        free(stack);
        return result;
    }

    as->as_stack = stack;
    return 0;
}

int
as_define_heap(addrspace_t *as) {
    if (as == NULL) {
        return EINVAL;
    }
    /* Find a location for the heap base */
    seL4_Word heap_base = 1*PAGE_SIZE;	
    for (region_t* r = as->as_rhead; r != NULL; r = r->next) {
        if (r->vtop > heap_base) {
            heap_base = r->vtop;
        }
    }

    /* Align the heap_base */
    heap_base = DIVROUNDUP(heap_base, PAGE_SIZE) * PAGE_SIZE;
    if (heap_base > as->as_stack->vbase) {
        /* Heap base overlap with stack */
        return EFAULT;
    }

    region_t* heap = malloc(sizeof(region_t));
    if (heap == NULL) {
        return ENOMEM;
    }

    int result = _region_init(as, heap_base, 0, AS_REGION_ALL, heap);
    if (result) {
        free(heap);
        return result;
    }

    as->as_heap = heap;

    return 0;
}
