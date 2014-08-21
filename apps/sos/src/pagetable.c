#include <sel4/sel4.h>
#include <stdio.h>
#include <limits.h>
#include <assert.h>
#include <string.h>

#include "pagetable.h"
#include "frametable.h"
#include "apps_vmem_layout.h"

#define STATUS_USED     0
#define STATUS_FREE     1

typedef struct _pagetable_entry{
    int status;             // Either USED or FREE
    seL4_CPtr frame_cap;    // frame cap of the allocated frame or NULL
    int frame_id;           // index returned by frametable
} pagetable_entry_t;


/* Global variables come here */
static bool _initialised = false;

/* Map a pagetable into slot i */
static int
_map_pagetable(pagedir_t* spd, int i) {
    return 0;
}

/* Map a pagetable at slot i */
static int
_unmap_pagetable(pagedir_t* spd, int i) {
    return 0;
}

/*
 * Memory management function to manage memory used by pagetable
 * Assume that pagetable functions do not need to free memory!!
 *
 * Return PAGE_IS_OK iff successful
 */
static int
_kmalloc(seL4_Word* vaddr) {
    static int n_allocated = 0;
    int result;

    *vaddr = 0;  // in case we fail

    seL4_Word tmp_vaddr = PAGE_VSTART + n_allocated * PAGE_SIZE;
    result = frame_alloc(&tmp_vaddr);
    if (result != FRAME_IS_OK)
        return PAGE_IS_FAIL;

    *vaddr = tmp_vaddr;
    return PAGE_IS_OK;
}

/*
 * Lets assume that there is only 1 process running on our OS
 */
int pagetable_init(void) {
    //first, init page dir

    // isn't page table per applications?
    // How do we store application's address space?
    // an array of address spaces relating to process structs?
    // Do we have an process/application struct?
    // Currently, do we use ASID at all in seL4? I can't find any with cscope

    _initialised = true;
    return PAGE_IS_OK;
}

int sos_page_map(pagedir_t* spd, seL4_Word* vaddr);

int sos_page_unmap(pagedir_t* spd, seL4_Word* vaddr);
