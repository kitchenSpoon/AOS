#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <limits.h>
#include <sel4/sel4.h>
#include <cspace/cspace.h>
#include <ut_manager/ut.h>
#include <strings.h>
#include <errno.h>

#include "vm.h"
#include "mapping.h"
#include "vmem_layout.h"
#include "addrspace.h"
#include "proc.h"

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

    int err;

    /* Check if the fault address is in a valid region */
    region_t* reg = _region_probe(as, fault_addr);
    if(reg != NULL){
        /* If yes, map a page for this address to use */
        seL4_Word kvaddr;
        err = sos_page_map(as, proc_getvroot(), fault_addr, &kvaddr);
        if (err) {
            return err;
        }

        return 0;
    }

    return EFAULT;
}
