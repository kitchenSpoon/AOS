#include <sel4/sel4.h>
#include <stdio.h>
#include <limits.h>
#include <assert.h>
#include <string.h>

#include "apps_vmem_layout.h"
#include "frametable.h"
#include "addrspace.h"

#define STATUS_USED     0
#define STATUS_FREE     1

#define PAGEDIR_BITS        (12)
#define PAGETABLE_BITS      (12)

#define INDEX_1_MASK        (0xffc00000)
#define INDEX_2_MASK        (0x003ff000)
#define INDEX_1(a)          ((a) & INDEX_1_MASK >> 22)
#define INDEX_2(a)          ((a) & INDEX_2_MASK >> 12)

/*
 * Allocate N frames.
 * Note:
 *      Only works for n == 1 now
 *      Assume that pagetable functions do not need to free memory!!
 *
 * Return the new address if successful, or NULL otherwise
 */
static seL4_Word
_page_malloc(int n) {
    assert(n == 1);

    /*
    static int allocated = 0;

    seL4_Word vaddr = PAGE_VSTART + allocated * PAGE_SIZE;

    frame_alloc(&vaddr);
    if (vaddr != NULL) allocated++;

    return vaddr;
    */
    seL4_Word vaddr;
    frame_alloc(&vaddr);
    return vaddr;
}

static int
_map_pagetable(pagedir_t* pd, int i) {
    return 0;
}

static int
_unmap_pagetable(pagedir_t* pd, int i) {
    return 0;
}

static int
_map_page(pagedir_t* pd, seL4_Word vaddr, uint32_t rights, uint32_t attr) {
    // allocate pagetable if there is none
    return PAGE_IS_OK;
}

static int
_umap_page(pagedir_t* pd, seL4_Word vaddr) {
    return PAGE_IS_OK;
}

pagedir_t* sos_addrspace_create(void) {
    pagedir_t* ret = (pagedir_t*)_page_malloc(1);
    return ret;
}

int sos_page_map(pagedir_t* pd, seL4_Word vaddr) {
    return 0;
    //int i = INDEX_1(vaddr);
    //int j = INDEX_2(vaddr);

    /* Attempt the mapping */
    //int err = _map_page(pd, vaddr);
    //if (err) {
    //    /* Assume the error was because we have no page table */
    //    err = _map_pagetable(pd, i);
    //    if (!err) {
    //        /* Try the mapping again */
    //        err = _map_page(pd, vaddr, rights, attr);
    //    }
    //}
    //return err;
}

int sos_page_unmap(pagedir_t* pd, seL4_Word vaddr);
