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

#define RW_BIT    (1<<11)
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
sos_VMFaultHandler(seL4_Word fault_addr, seL4_Word fsr){
    if (fault_addr == 0) {
        /* Derefenrecing NULL? */
        return EINVAL;
    }

    addrspace_t *as = proc_getas();
    if (as == NULL) {
        /* Kernel is probably failed when bootstraping */
        return EFAULT;
    }

    if (sos_page_is_mapped(as, fault_addr)) {
        /* This must be a readonly fault */
        return EACCES;
    }

    int err;
    bool fault_when_write = (bool)(fsr & RW_BIT);
    bool fault_when_read = !fault_when_write;

    /* Check if the fault address is in a valid region */
    region_t* reg = _region_probe(as, fault_addr);
    if(reg != NULL){
        if (fault_when_write && !(reg->rights & seL4_CanWrite)) {
            return EACCES;
        }
        if (fault_when_read && !(reg->rights & seL4_CanRead)) {
            return EACCES;
        }
        err = sos_page_map(as, proc_getvroot(), fault_addr, reg->rights);
        if (err) {
            return err;
        }

        return 0;
    }

    return EFAULT;
}
