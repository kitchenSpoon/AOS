#include <errno.h>
#include "addrspace.h"
#include "frametable.h"

#define N_PAGETABLES       (1024)
addrspace_t
*as_create(void);

int
as_init(addrspace_t *as) {
    as->as_pd = (pagedir_t)alloc_kpages(1);
    if (as->as_pd == NULL) {
        return ENOMEM;
    }
    for (int i=0; i<N_PAGETABLES; i++) {
        as->as_pd = NULL;
    }

    return 0;
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
