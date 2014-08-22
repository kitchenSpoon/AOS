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

static int
_map_pagetable(pagedir_t* pd, int i) {
    return 0;
}

int
sos_page_map(addrspace_t *as, seL4_Word vaddr) {
    //TODO:
    // This is the sos_map_page() function mentioned in the Milestone note
    // We could probably follow the map_page() code
    // Except that we need to do a lot more than that
    // We need to do 3 things:
    //     Call frame_alloc to get map SOS's vaddr to seL4's frame
    //     Get the frame cap from the frametable, map the application's vaddr to the same frame
    //     Map the application's vaddr to its own pagetable (the one we implement)
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

int
sos_page_unmap(pagedir_t* pd, seL4_Word vaddr);
