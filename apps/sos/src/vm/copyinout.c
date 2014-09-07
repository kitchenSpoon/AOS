#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sel4/sel4.h>

#include "tool/utility.h"
#include "proc/proc.h"
#include "vm/copyinout.h"
#include "vm/addrspace.h"

int
copyin(seL4_Word kbuf, seL4_Word buf, size_t nbyte) {
    unsigned long pos = 0;
    while (pos < nbyte) {
        seL4_Word ksrc;
        size_t cpy_sz;
        int err;

        /* Get the source's kernel address */
        err = sos_get_kvaddr(proc_getas(), PAGE_ALIGN(buf), &ksrc);
        if (err) {
            return err;
        }
        ksrc = ksrc + (buf - PAGE_ALIGN(buf));

        /* Copy the data over */
        cpy_sz = PAGE_SIZE - (ksrc & PAGEMASK);
        cpy_sz = MIN(cpy_sz, nbyte - pos);
        memcpy((void*)kbuf, (void*)ksrc, cpy_sz);

        pos  += cpy_sz;
        buf  += cpy_sz;
        kbuf += cpy_sz;
    }

    return 0;
}

int
copyout(seL4_Word buf, seL4_Word kbuf, size_t nbyte) {
    unsigned long pos = 0;
    addrspace_t* as = proc_getas();
    uint32_t permissions = 0;

    /* Ensure that the user buffer range is valid */
    if (!as_is_valid_memory(as, buf, nbyte, &permissions)) {
        return EINVAL;
    }

    while (pos < nbyte) {
        seL4_Word kdst;
        size_t cpy_sz;
        int err;
        if(!sos_page_is_mapped(as, PAGE_ALIGN(buf))) {
            sos_page_map(as, PAGE_ALIGN(buf), permissions);
        }

        /* Get the user buffer's corresponding kernel address */
        err = sos_get_kvaddr(as, buf, &kdst);
        if (err) {
            return err;
        }

        /* Copy the data over */
        cpy_sz = PAGE_SIZE - (kdst & PAGEMASK);
        cpy_sz = MIN(cpy_sz, nbyte - pos);
        memcpy((void*)kdst, (void*)kbuf, cpy_sz);

        pos  += cpy_sz;
        buf  += cpy_sz;
        kbuf += cpy_sz;
    }

    return 0;
}
