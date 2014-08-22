#include "addrspace.h"

addrspace_t
*as_create(void) {
    // TODO: set up the first level pagetable for now

    return NULL;
}

void
as_destroy(addrspace_t *as) {
    //TODO: clean up page table and also seL4's caps related to this as
    (void)as;
}

int
as_define_region(addrspace_t *as, seL4_Word vaddr, size_t sz, int32_t rights);

int
as_define_stack(addrspace_t *as, seL4_Word *initstackptr);

int
as_define_heap(addrspace_t *as);
