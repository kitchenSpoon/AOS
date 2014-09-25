#include <errno.h>
#include <stdio.h>
#include <strings.h>
#include <limits.h>

#include "vm/vm.h"
#include "vm/addrspace.h"
#include "tool/utility.h"

#define N_PAGETABLES       (1024)
#define DIVROUNDUP(a,b) (((a)+(b)-1)/(b))

addrspace_t
*as_create(seL4_ARM_PageDirectory sel4_pd) {
    addrspace_t* as = malloc(sizeof(addrspace_t));
    if (as == NULL)
        return as;

    /* Initialise page directories */
    seL4_Word vaddr = frame_alloc();
    if (vaddr == 0) {
        free(as);
        return NULL;
    }
    as->as_pd_caps = (pagedir_t)vaddr;

    vaddr = frame_alloc();
    if (vaddr == 0) {
        free(as);
        return NULL;
    }
    as->as_pd_regs = (pagedir_t)vaddr;

    bzero((void*)as->as_pd_caps, PAGE_SIZE);
    bzero((void*)as->as_pd_regs, PAGE_SIZE);

    /* Initialise the remaining values */
    as->as_rhead   = NULL;
    as->as_stack   = NULL;
    as->as_heap    = NULL;
    as->as_sel4_pd = sel4_pd;
    as->as_pt_head = NULL;

    return as;
}

void
as_destroy(addrspace_t *as) {
    //TODO: clean up page table and also seL4's caps related to this as
    (void)as;
    if(as == NULL){
        return;
    }

//    //Free page directory
//    if(as->as_pd_regs != NULL){
//        //TODO actually free them
//        for(int i = 0; i < N_PAGETABLES; i++){
//            if(as->as_pd_regs[i] != NULL){
//                frame_free((seL4_Word)as->as_pd_regs[i]);
//            }
//        }
//    }

    //Free heap
    //Free stack
    //Free regionhead

}

static int
_region_overlap(region_t* r1, region_t* r2) {
    assert(r1 != NULL && r2 != NULL);

    bool r1_left_r2 = (r1->vbase < r2->vbase && r1->vtop <= r2->vbase);
    bool r1_right_r2 = (r1->vbase >= r2->vtop && r1->vtop >= r2->vtop);
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

    /* Page align the page and the size */
    vaddr = PAGE_ALIGN(vaddr);
    sz = DIVROUNDUP(sz, PAGE_SIZE) * PAGE_SIZE;

    /* Check region overlap */
    int err = _region_init(as, vaddr, sz, rights, nregion);
    if (err) {
        return err;
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

    assert(IS_PAGESIZE_ALIGNED(stack_top));
    size = DIVROUNDUP(size, PAGE_SIZE) * PAGE_SIZE;

    region_t *stack, *page_guard;
    int stack_base;
    int err;
    stack = malloc(sizeof(region_t));
    if (stack == NULL) {
        return ENOMEM;
    }
    page_guard = malloc(sizeof(region_t));
    if (page_guard == NULL) {
        free(stack);
        return ENOMEM;
    }

    stack_base = stack_top - size;
    err = _region_init(as, stack_base, size, seL4_AllRights, stack);
    if (err) {
        free(stack);
        free(page_guard);
        return err;
    }

    err = _region_init(as, stack_base - PAGE_SIZE, PAGE_SIZE, 0, page_guard);
    if (err) {
        free(stack);
        free(page_guard);
        return err;
    }

    as->as_stack = stack;

    page_guard->next = as->as_rhead;
    as->as_rhead = page_guard->next;

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

    int err = _region_init(as, heap_base, 0, seL4_AllRights, heap);
    if (err) {
        free(heap);
        return err;
    }

    as->as_heap = heap;

    return 0;
}

seL4_Word sos_sys_brk(addrspace_t *as, seL4_Word vaddr){
    if(as == NULL || as->as_heap == NULL) return 0;

    printf("sos_sysbrk, vaddr = %p\n", (void*)vaddr);
    if(vaddr == 0){
        return as->as_heap->vtop;
    }
    if (vaddr < as->as_heap->vbase) {
        return as->as_heap->vtop = as->as_heap->vbase;
    }

    seL4_Word oldtop = as->as_heap->vtop;
    as->as_heap->vtop = vaddr;

    if(as->as_stack != NULL && _region_overlap(as->as_heap, as->as_stack)){
        as->as_heap->vtop = oldtop;
        return 0;
    }

    for (region_t *r = as->as_rhead; r != NULL; r = r->next) {
        if (_region_overlap(as->as_heap, r)) {
            as->as_heap->vtop = oldtop;
            return 0;
        }
    }
    return vaddr;
}

bool as_is_valid_memory(addrspace_t *as, seL4_Word vaddr, size_t size,
                        uint32_t* permission) {
    seL4_Word range_start = vaddr;
    seL4_Word range_end   = vaddr + (seL4_Word)size;
    for (region_t *r = as->as_rhead; r != NULL; r = r->next) {
        if (r->vbase <= range_start && range_end <= r->vtop) {
            *permission = r->rights;
            return true;
        }
    }

    if (as->as_stack->vbase <= range_start && range_end <= as->as_stack->vtop) {
        *permission = as->as_stack->rights;
        return true;
    }
    if (as->as_heap->vbase <= range_start && range_end <= as->as_heap->vtop) {
        *permission = as->as_heap->rights;
        return true;
    }

    return false;
}
