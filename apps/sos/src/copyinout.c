#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sel4/sel4.h>

#include "utility.h"
#include "proc.h"
#include "copyinout.h"
#include "addrspace.h"

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
    while (pos < nbyte) {
        seL4_Word kdst;
        size_t cpy_sz;
        int err;

        /* Get the user buffer's corresponding kernel address */
        err = sos_get_kvaddr(proc_getas(), PAGE_ALIGN(buf), &kdst);
        if (err) {
            return err;
        }
        kdst = kdst + (buf - PAGE_ALIGN(buf));

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
