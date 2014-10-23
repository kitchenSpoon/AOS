#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sel4/sel4.h>

#include "tool/utility.h"
#include "proc/proc.h"
#include "vm/copyinout.h"
#include "vm/addrspace.h"
#include "vm/swap.h"

#define verbose 0
#include <sys/debug.h>

/***********************************************************************
 * Copyin
 **********************************************************************/
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

static void _copyin_do_copy(void *token, int err);
static void _copyin_end(copyin_cont_t *cont, int err);

int
copyin(seL4_Word kbuf, seL4_Word buf, size_t nbyte, copyin_cb_t callback, void *token) {
    dprintf(3, "copyin called, kbuf=0x%08x, buf=0x%08x, nbyte=%u\n", kbuf, buf, nbyte);
    uint32_t permissions = 0;

    addrspace_t *as = proc_getas();
    /* Ensure that the user buffer range is valid */
    if (!as_is_valid_memory(as, buf, nbyte, &permissions)) {
        return EINVAL;
    }

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
    cont->as       = as;
    cont->reg      = region_probe(cont->as, buf);

    assert(cont->reg != NULL); // This address need to be valid, read precond in copyinout.h

    _copyin_do_copy((void*)cont, 0);
    return 0;
}

static void
_copyin_do_copy(void *token, int err){
    dprintf(3, "_copyin_do_copy\n");
    copyin_cont_t *cont = (copyin_cont_t*)token;

    if (err) {
        _copyin_end(cont, err);
        return;
    }

    /* Check if we need to either map the page or swap in */
    seL4_Word vpage = PAGE_ALIGN(cont->buf);
    if (!sos_page_is_inuse(cont->as, vpage)) {
        dprintf(3, "_copyin_do_copy: mapping page in\n");
        err = sos_page_map(proc_get_id(), cont->as, vpage, cont->reg->rights, _copyin_do_copy, (void*)cont, false);
        if (err) {
            _copyin_end(cont, err);
            return;
        }
        inc_proc_size_proc(cur_proc());
        return;
    } else if (sos_page_is_swapped(cont->as, vpage)) {
        dprintf(3, "_copyin_do_copy: swapping page in\n");
        err = swap_in(cont->as, cont->reg->rights, vpage,
                false, _copyin_do_copy, cont);
        if (err) {
            _copyin_end(cont, err);
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
        err = sos_get_kvaddr(cont->as, cont->buf, &ksrc);
        if (err) {
            _copyin_end(cont, err);
            return;
        }

        /* Copy maximum one page */
        cpy_sz = PAGE_SIZE - PAGE_OFFSET(ksrc);
        cpy_sz = MIN(cpy_sz, cont->nbyte - cont->pos);
        dprintf(3, "cpy_sz = %u\n", cpy_sz);

        memcpy((void*)cont->kbuf, (void*)ksrc, cpy_sz);

        dprintf(3, "copyin from (ubuf=0x%08x, ksrc=0x%08x) to (kbuf=0x%08x)\n", cont->buf, ksrc, cont->kbuf);
        cont->pos  += cpy_sz;
        cont->buf  += cpy_sz;
        cont->kbuf += cpy_sz;
        _copyin_do_copy((void*)cont, 0);
        return;
    } else {
        _copyin_end(cont, 0);
        return;
    }
}

static void
_copyin_end(copyin_cont_t *cont, int err) {
    assert(cont != NULL);
    if (err) {
        cont->callback(cont->token, err);
        free(cont);
        return;
    }
    dprintf(3, "copyin call back up\n");
    cont->callback(cont->token, 0);
    free(cont);
}

/***********************************************************************
 * Copyout
 **********************************************************************/

typedef struct {
    seL4_Word buf;
    seL4_Word kbuf;
    size_t nbyte;
    unsigned long pos;
    addrspace_t *as;
    region_t *reg;
    copyout_cb_t callback;
    void *token;
} copyout_cont_t;

static void _copyout_end(void *token, int err);
static void _copyout_do_copy(void *token, int err);

int
copyout(seL4_Word buf, seL4_Word kbuf, size_t nbyte, copyout_cb_t callback, void *token) {
    uint32_t permissions = 0;
    dprintf(3, "copyout called, kbuf=0x%08x, buf=0x%08x, nbyte=%u\n", kbuf, buf, nbyte);

    //dprintf(3, "copyout buf = 0x%08x\n", buf);

    addrspace_t *as = proc_getas();
    assert(as != NULL);

    /* Ensure that the user buffer range is valid */
    if (!as_is_valid_memory(as, buf, nbyte, &permissions)) {
        return EINVAL;
    }

    copyout_cont_t *cont = malloc(sizeof(copyout_cont_t));
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

    _copyout_do_copy((void*)cont, 0);

    return 0;
}

static void
_copyout_do_copy(void *token, int err){
    dprintf(3, "_copyout_do_copy\n");
    if (err) {
        _copyout_end(token, err);
        return;
    }

    copyout_cont_t *cont = (copyout_cont_t*)token;
    assert(cont != NULL);

    /* Check if we need to either map the page or swap in */
    seL4_Word vpage = PAGE_ALIGN(cont->buf);
    if (!sos_page_is_inuse(cont->as, vpage)) {
        dprintf(3, "_copyout_do_copy: mapping page in\n");
        err = sos_page_map(proc_get_id(), cont->as, vpage, cont->reg->rights, _copyout_do_copy, (void*)cont, false);
        if (err) {
            _copyout_end(token, err);
            return;
        }
        inc_proc_size_proc(cur_proc());
        return;
    } else if (sos_page_is_swapped(cont->as, vpage)) {
        dprintf(3, "_copyout_do_copy: swapping page in\n");
        err = swap_in(cont->as, cont->reg->rights, vpage,
                false, _copyout_do_copy, cont);
        if (err) {
            _copyout_end(token, err);
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
        err = sos_get_kvaddr(cont->as, cont->buf, &kdst);
        if (err) {
            _copyout_end(token, err);
            return;
        }

        /* Copy the data over */
        cpy_sz = PAGE_SIZE - PAGE_OFFSET(kdst);
        cpy_sz = MIN(cpy_sz, cont->nbyte - cont->pos);
        dprintf(3, "cpy_sz = %u\n", cpy_sz);
        memcpy((void*)kdst, (void*)cont->kbuf, cpy_sz);
        dprintf(3, "copyout from kbuf=0x%08x to (ubuf=0x%08x, kdst=0x%08x)\n", cont->kbuf, cont->buf, kdst);

        cont->pos  += cpy_sz;
        cont->buf  += cpy_sz;
        cont->kbuf += cpy_sz;

        _copyout_do_copy(token, 0);
        return;
    } else {
        _copyout_end(token, 0);
        return;
    }
}

static void
_copyout_end(void *token, int err){
    copyout_cont_t *cont = (copyout_cont_t*)token;

    if(err){
        cont->callback(cont->token, err);
        free(cont);
        return;
    }

    dprintf(3, "copyout calls back up\n");
    cont->callback(cont->token, 0);
    free(cont);
    return;
}
