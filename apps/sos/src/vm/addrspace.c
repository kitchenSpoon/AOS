#include <errno.h>
#include <stdio.h>
#include <strings.h>
#include <limits.h>
#include <sel4/sel4.h>
#include <ut_manager/ut.h>

#include "vm/vm.h"
#include "vm/swap.h"
#include "vm/addrspace.h"
#include "tool/utility.h"

#define N_PAGETABLES             (1024)
#define N_PAGETABLES_ENTRIES     (1024)
#define DIVROUNDUP(a,b) (((a)+(b)-1)/(b))

#define verbose 0
#include <sys/debug.h>

/***********************************************************************
 * as_create
 ***********************************************************************/
typedef struct {
    addrspace_t *as;
    as_create_cb_t callback;
    void *token;
} as_create_cont_t;

static void _as_create_pagedir_caps_allocated(void *token, seL4_Word kvaddr);
static void _as_create_pagedir_regs_allocated(void *token, seL4_Word kvaddr);
static void _as_create_end(as_create_cont_t *cont, int err);

int
as_create(seL4_ARM_PageDirectory sel4_pd, as_create_cb_t callback, void *token) {
    dprintf(3, "as_create called\n");
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

    as_create_cont_t *cont = malloc(sizeof(as_create_cont_t));
    if (cont == NULL) {
        free(as);
        return ENOMEM;
    }
    cont->callback = callback;
    cont->token    = token;
    cont->as       = as;

    err = frame_alloc(0, NULL, PROC_NULL, true, _as_create_pagedir_caps_allocated, (void*)cont);
    if (err) {
        free(as);
        free(cont);
        return err;
    }
    return 0;
}

static void
_as_create_pagedir_caps_allocated(void *token, seL4_Word kvaddr) {
    dprintf(3, "as create pagedir_cap_allocated\n");
    int err;
    as_create_cont_t *cont = (as_create_cont_t*)token;
    if (cont == NULL) {
        dprintf(3, "_as_create_pagedir_caps_allocated: There is something wrong with the memory\n");
        return;
    }

    if (kvaddr == 0) {
        dprintf(3, "as create err 1\n");
        _as_create_end(cont, ENOMEM);
        return;
    }
    cont->as->as_pd_caps = (pagedir_t)kvaddr;
    bzero((void*)(cont->as->as_pd_caps), PAGE_SIZE);

    err = frame_alloc(0, NULL, PROC_NULL, true, _as_create_pagedir_regs_allocated, (void*)cont);
    if (err) {
        dprintf(3, "as create err 2\n");
        _as_create_end(cont, err);
        return;
    }
}

static void
_as_create_pagedir_regs_allocated(void *token, seL4_Word kvaddr) {
    dprintf(3, "as create pagedir_reg_allocated\n");
    as_create_cont_t *cont = (as_create_cont_t*)token;
    if (cont == NULL) {
        dprintf(3, "_as_create_pagedir_regs_allocated: There is something wrong with the memory\n");
        return;
    }

    if (kvaddr == 0) {
        _as_create_end(cont, ENOMEM);
        return;
    }
    cont->as->as_pd_regs = (pagedir_t)kvaddr;
    bzero((void*)(cont->as->as_pd_regs), PAGE_SIZE);

    _as_create_end(cont, 0);
    return;
}

static void
_as_create_end(as_create_cont_t *cont, int err) {
    dprintf(3, "as create end\n");
    if (!err) {
        dprintf(3, "as create end success\n");
        cont->callback(cont->token, cont->as);
        free(cont);
        return;
    }

    dprintf(3, "as create end err = %d\n",err);
    /* Clean up as needed */
    if (cont->as) {
        if (cont->as->as_pd_caps != NULL) {
            frame_free((seL4_Word)(cont->as->as_pd_caps));
        }
        if (cont->as->as_pd_regs != NULL) {
            frame_free((seL4_Word)(cont->as->as_pd_regs));
        }
    }
    cont->callback(cont->token, NULL);
    return;
}

/***********************************************************************
 * as_destroy
 ***********************************************************************/
void
as_destroy(addrspace_t *as) {
    dprintf(3, "as destroy called\n");
    if(as == NULL){
        return;
    }

    //Free page directory
    assert(as->as_pd_regs != NULL && as->as_pd_caps != NULL);
    for(int i = 0; i < N_PAGETABLES; i++){
        if (as->as_pd_regs[i] == NULL) {
            assert(as->as_pd_caps[i] == NULL);
            continue;
        }

        assert(as->as_pd_caps != NULL);
        for(int j = 0; j < N_PAGETABLES_ENTRIES; j++){
            sos_page_free(as, PT_ID_TO_VPAGE(i, j));
        }
        frame_free((seL4_Word)as->as_pd_caps[i]);
        frame_free((seL4_Word)as->as_pd_regs[i]);
    }

    frame_free((seL4_Word)as->as_pd_regs);
    frame_free((seL4_Word)as->as_pd_caps);

    //Free heap
    free(as->as_heap);

    //Free stack
    free(as->as_stack);

    //Free regions
    region_t* cur_r = as->as_rhead;
    region_t* prev_r = cur_r;
    while(cur_r != NULL){
        cur_r = cur_r->next;
        free(prev_r);
        prev_r = cur_r;
    }

    //Free sel4 page tables
    sel4_pt_node_t* cur_pt  = as->as_pt_head;
    sel4_pt_node_t* prev_pt = cur_pt;
    while(cur_pt != NULL){
        cur_pt = cur_pt->next;
        seL4_ARM_PageTable_Unmap(prev_pt->pt);
        ut_free(prev_pt->pt_addr, seL4_PageTableBits);
        cspace_delete_cap(cur_cspace, prev_pt->pt);
        free(prev_pt);
        prev_pt = cur_pt;
    }

    //free sel4 page dir
    //this is deleted in proc?
    //cspace_delete_cap(cur_cspace, as_sel4_pd);

    free(as);
}

/**********************************************************************
 * Region related functions
 * - region_probe
 * - as_define_region
 * - as_define_stack
 * - as_define_heap
 * - sos_sys_brk
 **********************************************************************/

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
    dprintf(3, "as region init\n");
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
        dprintf(3, "as region init loop, region base = 0x%08x, region top = 0x%08x\n", r->vbase,r->vtop);
        if (_region_overlap(nregion, r)) {
            return EINVAL;
        }
    }
    return 0;
}

region_t*
region_probe(struct addrspace* as, seL4_Word addr) {
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
as_define_region(addrspace_t *as, seL4_Word vaddr, size_t sz, int32_t rights) {
    dprintf(3, "as define region\n");
    assert(as != NULL);

    region_t* nregion = malloc(sizeof(region_t));
    if (nregion == NULL) {
        dprintf(3, "as define region no mem\n");
        return ENOMEM;
    }

    /* Page align the page and the size */
    vaddr = PAGE_ALIGN(vaddr);
    sz = DIVROUNDUP(sz, PAGE_SIZE) * PAGE_SIZE;

    /* Check region overlap */
    int err = _region_init(as, vaddr, sz, rights, nregion);
    if (err) {
        dprintf(3, "as define region init failed\n");
        return err;
    }

    /* Add the new region to addrspace's region list */
    nregion->next = as->as_rhead;
    as->as_rhead = nregion;

    dprintf(3, "as define region end\n");
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

seL4_Word
sos_sys_brk(addrspace_t *as, seL4_Word vaddr){
    if(as == NULL || as->as_heap == NULL) return 0;

    dprintf(3, "sos_sysbrk, vaddr = %p\n", (void*)vaddr);
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
    dprintf(3, "sos_sysbrk ended, vaddr = %p\n", (void*)vaddr);
    return vaddr;
}

/***********************************************************************
 * Simple getter and setter functions for addrspace
 **********************************************************************/
bool
as_is_valid_memory(addrspace_t *as, seL4_Word vaddr, size_t size,
                   uint32_t* permission) {
    region_t *reg = region_probe(as, vaddr);
    if (reg != NULL) {
        if (permission != NULL) {
            *permission = reg->rights;
        }
        return vaddr + size < reg->vtop;
    }
    return false;
}
