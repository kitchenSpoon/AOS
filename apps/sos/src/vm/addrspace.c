#include <errno.h>
#include <stdio.h>
#include <strings.h>
#include <limits.h>

#include "vm/vm.h"
#include "vm/addrspace.h"
#include "tool/utility.h"

#define N_PAGETABLES       (1024)
#define DIVROUNDUP(a,b) (((a)+(b)-1)/(b))

typedef struct {
    addrspace_t *as;
    as_create_cb_t callback;
    void *token;
} as_create_cont_t;

static void
as_create_end(as_create_cont_t *cont, int err) {
    printf("as create end\n");
    if (!err) {
        printf("as create end success\n");
        cont->callback(cont->token, cont->as);
        free(cont);
        return;
    }

    printf("as create end err = %d\n",err);
    /* Clean up as needed */
    if (cont->as) {
        if (cont->as->as_pd_caps != NULL) {
            //frame_free((seL4_Word)(cont->as->as_pd_caps));
        }
        if (cont->as->as_pd_regs != NULL) {
            //frame_free((seL4_Word)(cont->as->as_pd_regs));
        }
    }
    cont->callback(cont->token, NULL);
    return;
}

static void
as_create_pagedir_regs_allocated(void *token, seL4_Word kvaddr) {
    printf("as create pagedir_reg_allocated\n");
    as_create_cont_t *cont = (as_create_cont_t*)token;
    if (cont == NULL) {
        printf("as_create_pagedir_regs_allocated: There is something wrong with the memory\n");
        return;
    }

    if (kvaddr == 0) {
        as_create_end(cont, ENOMEM);
        return;
    }
    cont->as->as_pd_regs = (pagedir_t)kvaddr;
    bzero((void*)(cont->as->as_pd_regs), PAGE_SIZE);

    as_create_end(cont, 0);
    return;
}

static void
as_create_pagedir_caps_allocated(void *token, seL4_Word kvaddr) {
    printf("as create pagedir_cap_allocated\n");
    int err;
    as_create_cont_t *cont = (as_create_cont_t*)token;
    if (cont == NULL) {
        printf("as_create_pagedir_caps_allocated: There is something wrong with the memory\n");
        return;
    }

    if (kvaddr == 0) {
        as_create_end(cont, ENOMEM);
        return;
    }
    cont->as->as_pd_caps = (pagedir_t)kvaddr;
    bzero((void*)(cont->as->as_pd_caps), PAGE_SIZE);

    err = frame_alloc(as_create_pagedir_regs_allocated, (void*)cont);
    if (err) {
        as_create_end(cont, err);
        return;
    }
}

int
as_create(seL4_ARM_PageDirectory sel4_pd, as_create_cb_t callback, void *token) {
    printf("as create\n");
    int err;
    addrspace_t* as = malloc(sizeof(addrspace_t));
    if (as == NULL) {
        return ENOMEM;
    }
    as->as_pd_caps = NULL;
    as->as_pd_regs = NULL;
    as->as_rhead   = NULL;
    as->as_stack   = NULL;
    as->as_heap    = NULL;
    as->as_sel4_pd = sel4_pd;
    as->as_pt_head = NULL;

    printf("as create\n");
    as_create_cont_t *cont = malloc(sizeof(as_create_cont_t));
    if (cont == NULL) {
        free(as);
        return ENOMEM;
    }
    cont->callback = callback;
    cont->token    = token;
    cont->as       = as;

    printf("as create\n");
    err = frame_alloc(as_create_pagedir_caps_allocated, (void*)cont);
    if (err) {
        printf("as create err\n");
        free(as);
        free(cont);
        return err;
    }
    printf("as create\n");
    return 0;
}

void
as_destroy(addrspace_t *as) {
    //TODO: clean up page table and also seL4's caps related to this as
    (void)as;
    if(as == NULL){
        return;
    }

    //Free page directory
    if(as->as_pd_regs != NULL){
        //TODO actually free them
        for(int i = 0; i < N_PAGETABLES; i++){
            if(as->as_pd_regs[i] != NULL){
                frame_free((seL4_Word)as->as_pd_regs[i]);
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
    printf("as region init\n");
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
        printf("as region init loop, region base = 0x%08x, region top = 0x%08x\n", r->vbase,r->vtop);
        if (_region_overlap(nregion, r)) {
            return EINVAL;
        }
    }
    return 0;
}

int
as_define_region(addrspace_t *as, seL4_Word vaddr, size_t sz, int32_t rights) {
    printf("as define region\n");
    assert(as != NULL);

    region_t* nregion = malloc(sizeof(region_t));
    if (nregion == NULL) {
        printf("as define region no mem\n");
        return ENOMEM;
    }

    /* Page align the page and the size */
    vaddr = PAGE_ALIGN(vaddr);
    sz = DIVROUNDUP(sz, PAGE_SIZE) * PAGE_SIZE;

    /* Check region overlap */
    int err = _region_init(as, vaddr, sz, rights, nregion);
    if (err) {
        printf("as define region init failed\n");
        return err;
    }

    /* Add the new region to addrspace's region list */
    nregion->next = as->as_rhead;
    as->as_rhead = nregion;

    printf("as define region end\n");
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
