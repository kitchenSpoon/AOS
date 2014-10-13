#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sel4/sel4.h>

#include "tool/utility.h"
#include "proc/proc.h"
#include "vm/copyinout.h"
#include "vm/addrspace.h"
#include "vm/swap.h"

typedef struct {
    copyin_cb_t callback;
    void *token;
    seL4_Word kbuf;
    seL4_Word buf;
    size_t nbyte;
    unsigned long pos;
    addrspace_t *as;
    region_t *reg;
} copyin_cont_t;

void copyin_end(copyin_cont_t *cont, int err) {
    assert(cont != NULL);
    if (err) {
        cont->callback(cont->token, err);
        free(cont);
        return;
    }
    printf("copyin call back up\n");
    cont->callback(cont->token, 0);
    free(cont);
}

void copyin_do_copy(void* token, int err){
    printf("copyin_do_copy\n");
    copyin_cont_t* cont = (copyin_cont_t*)token;

    if (err) {
        copyin_end(cont, err);
        return;
    }

    /* Check if we need to either map the page or swap in */
    seL4_Word vaddr = cont->buf;
    if (!sos_page_is_inuse(cont->as, vaddr)) {
        err = sos_page_map(cont->as, vaddr, cont->reg->rights, copyin_do_copy, (void*)cont, false);
        if (err) {
            copyin_end(cont, err);
            return;
        }
        return;
    } else if (sos_page_is_swapped(cont->as, vaddr)) {
        err = swap_in(cont->as, cont->reg->rights, vaddr,
                false, copyin_do_copy, cont);
        if (err) {
            copyin_end(cont, err);
            return;
        }
        return;
    }

    /* Now it's guarantee that the page is in memory */

    /* Copy 1 page at a time */
    if (cont->pos < cont->nbyte) {
        seL4_Word ksrc;
        size_t cpy_sz;

        /* Get the source's kernel address */
        err = sos_get_kvaddr(cont->as, PAGE_ALIGN(cont->buf), &ksrc);
        if (err) {
            copyin_end(cont, err);
            return;
        }
        ksrc = ksrc + (cont->buf - PAGE_ALIGN(cont->buf));

        /* Copy maximum one page */
        cpy_sz = PAGE_SIZE - (ksrc & PAGE_OFFSET_MASK);
        cpy_sz = MIN(cpy_sz, cont->nbyte - cont->pos);

        memcpy((void*)cont->kbuf, (void*)ksrc, cpy_sz);

        cont->pos  += cpy_sz;
        cont->buf  += cpy_sz;
        cont->kbuf += cpy_sz;
        copyin_do_copy((void*)cont, 0);
        return;
    } else {
        copyin_end(cont, 0);
        return;
    }
}

int
copyin(seL4_Word kbuf, seL4_Word buf, size_t nbyte, copyin_cb_t callback, void *token) {
    printf("copyin called, kbuf=0x%08x, buf=0x%08x, nbyte=%u\n", kbuf, buf, nbyte);
    copyin_cont_t *cont = malloc(sizeof(copyin_cont_t));
    if (cont == NULL) {
        return ENOMEM;
    }
    cont->callback = callback;
    cont->token    = token;
    cont->kbuf     = kbuf;
    cont->buf      = buf;
    cont->nbyte    = nbyte;
    cont->pos      = 0;
    cont->as       = proc_getas();
    cont->reg      = region_probe(cont->as, buf);

    assert(cont->reg != NULL); // This address need to be valid, read precond in copyinout.h

    copyin_do_copy((void*)cont, 0);
    return 0;
}
//int
//copyin(seL4_Word kbuf, seL4_Word buf, size_t nbyte) {
//    unsigned long pos = 0;
//    while (pos < nbyte) {
//        seL4_Word ksrc;
//        size_t cpy_sz;
//        int err;
//
//        /* Get the source's kernel address */
//        err = sos_get_kvaddr(proc_getas(), PAGE_ALIGN(buf), &ksrc);
//        if (err) {
//            return err;
//        }
//        ksrc = ksrc + (buf - PAGE_ALIGN(buf));
//
//        /* Copy the data over */
//        cpy_sz = PAGE_SIZE - (ksrc & PAGE_OFFSET_MASK);
//        cpy_sz = MIN(cpy_sz, nbyte - pos);
//        memcpy((void*)kbuf, (void*)ksrc, cpy_sz);
//
//        pos  += cpy_sz;
//        buf  += cpy_sz;
//        kbuf += cpy_sz;
//    }
//
//    return 0;
//}

typedef struct {
    seL4_Word buf;
    seL4_Word kbuf;
    size_t nbyte;
    unsigned long pos;
    addrspace_t* as;
    region_t *reg;
    copyout_cb_t callback;
    void* token;

} copyout_cont_t;

void copyout_end(void* token, int err){
    copyout_cont_t* cont = (copyout_cont_t*)token;

    if(err){
        cont->callback(cont->token, err);
        free(cont);
        return;
    }

    cont->callback(cont->token, 0);
    free(cont);
    return;
}

void copyout_do_copy(void* token, int err){
    printf("copyout_do_copy\n");
    if (err) {
        copyout_end(token, err);
        return;
    }

    copyout_cont_t* cont = (copyout_cont_t*)token;
    assert(cont != NULL);

    //might need to lock the frame when we have multiple process

    /* Check if we need to either map the page or swap in */
    seL4_Word vaddr = cont->buf;
    if (!sos_page_is_inuse(cont->as, vaddr)) {
        err = sos_page_map(cont->as, vaddr, cont->reg->rights, copyout_do_copy, (void*)cont, false);
        if (err) {
            copyout_end(token, err);
            return;
        }
        return;
    } else if (sos_page_is_swapped(cont->as, vaddr)) {
        err = swap_in(cont->as, cont->reg->rights, vaddr,
                false, copyout_do_copy, cont);
        if (err) {
            copyout_end(token, err);
            return;
        }
        return;
    }

    /* Now it's guarantee that the page is in memory */

    /* Copy 1 page at a time */
    if(cont->pos < cont->nbyte) {
        seL4_Word kdst;
        size_t cpy_sz;

        /* Get the user buffer's corresponding kernel address */
        err = sos_get_kvaddr(cont->as, PAGE_ALIGN(cont->buf), &kdst);
        if (err) {
            copyout_end(token, err);
            return;
        }

        kdst = kdst + (cont->buf - PAGE_ALIGN(cont->buf));

        /* Copy the data over */
        cpy_sz = PAGE_SIZE - (kdst & PAGE_OFFSET_MASK);
        cpy_sz = MIN(cpy_sz, cont->nbyte - cont->pos);
        memcpy((void*)kdst, (void*)cont->kbuf, cpy_sz);

        cont->pos  += cpy_sz;
        cont->buf  += cpy_sz;
        cont->kbuf += cpy_sz;

        copyout_do_copy(token, 0);
        return;
    } else {
        copyout_end(token, 0);
        return;
    }
}

int
copyout(seL4_Word buf, seL4_Word kbuf, size_t nbyte, copyout_cb_t callback, void* token) {
    uint32_t permissions = 0;
    printf("copyout\n");

    //printf("copyout buf = 0x%08x\n", buf);

    addrspace_t* as = proc_getas();
    assert(as != NULL);

    /* Ensure that the user buffer range is valid */
    if (!as_is_valid_memory(as, buf, nbyte, &permissions)) {
        return EINVAL;
    }

    copyout_cont_t* cont = malloc(sizeof(copyout_cont_t));
    if(cont == NULL){
        return ENOMEM;
    }

    cont->buf       = buf;
    cont->kbuf      = kbuf;
    cont->nbyte     = nbyte;
    cont->pos       = 0;
    cont->as        = as;
    cont->reg       = region_probe(as, buf);
    cont->callback  = callback;
    cont->token     = token;

    copyout_do_copy((void*)cont, 0);

    return 0;
}

//int
//copyout(seL4_Word buf, seL4_Word kbuf, size_t nbyte) {
//    unsigned long pos = 0;
//    addrspace_t* as = proc_getas();
//    uint32_t permissions = 0;
//
//    /* Ensure that the user buffer range is valid */
//    if (!as_is_valid_memory(as, buf, nbyte, &permissions)) {
//        return EINVAL;
//    }
//
//    while (pos < nbyte) {
//        seL4_Word kdst;
//        size_t cpy_sz;
//        int err;
//        if(!sos_page_is_inuse(as, PAGE_ALIGN(buf))) {
//            //TODO make this asynchronous
//            //sos_page_map(as, PAGE_ALIGN(buf), permissions);
//        }
//
//        /* Get the user buffer's corresponding kernel address */
//        err = sos_get_kvaddr(as, buf, &kdst);
//        if (err) {
//            return err;
//        }
//
//        /* Copy the data over */
//        cpy_sz = PAGE_SIZE - (kdst & PAGE_OFFSET_MASK);
//        cpy_sz = MIN(cpy_sz, nbyte - pos);
//        memcpy((void*)kdst, (void*)kbuf, cpy_sz);
//
//        pos  += cpy_sz;
//        buf  += cpy_sz;
//        kbuf += cpy_sz;
//    }
//
//    return 0;
//}
