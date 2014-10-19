/*
 * Copyright 2014, NICTA
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(NICTA_BSD)
 */

#include <sel4/sel4.h>
#include <elf/elf.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <cspace/cspace.h>
#include <ut_manager/ut.h>
#include <nfs/nfs.h>

#include "vm/mapping.h"
#include "vm/addrspace.h"
#include "vm/elf.h"
#include "vm/vmem_layout.h"
#include "tool/utility.h"

#define verbose 2
#include <sys/debug.h>
#include <sys/panic.h>

extern fhandle_t mnt_point;

/*
 * Convert ELF permissions into seL4 permissions.
 */
static inline unsigned long get_sel4_rights_from_elf(unsigned long permissions) {
    seL4_Word result = 0;

    if (permissions & PF_R)
        result |= seL4_CanRead;
    if (permissions & PF_X)
        result |= seL4_CanRead;
    if (permissions & PF_W)
        result |= seL4_CanWrite;

    return result;
}

/*
 * Inject data into the given vspace.
 */


typedef void (*load_segment_cb_t)(void *token, int err);

static void load_segment_into_vspace2(void* token, int err);

typedef struct {
    addrspace_t* as;
    unsigned long file_offset;
    unsigned long segment_size;
    unsigned long file_size;
    unsigned long dst;
    unsigned long permissions;
    unsigned long pos;
    unsigned long wanna_read;
    process_t* proc;
    fhandle_t* fh;
    load_segment_cb_t callback;
    void* token;
} load_segment_cont_t;

static void
load_segment_into_vspace_end(load_segment_cont_t *cont, int err) {
    // We don't need to free anything, including as mapped page as they'll be
    // cleaned up by the caller (proc_create)
    cont->callback(cont->token, err);
    free(cont);
}

/* This function actually copy data into the user vspace */
static void
load_segment_into_vspace4(uintptr_t token, enum nfs_stat status,
                          fattr_t *fattr, int count, void* data) {
    printf("load_segment_into_vspace4 called\n");
    load_segment_cont_t* cont = (load_segment_cont_t*)token;

    if (status != NFS_OK) {
        load_segment_into_vspace_end(cont, EFAULT);
        return;
    }

    int err;
    seL4_Word kdst;
    seL4_CPtr sos_cap;

    err = sos_get_kvaddr(cont->as, cont->dst, &kdst);
    if (err) {
        load_segment_into_vspace_end(cont, err);
        return;
    }

    /* Now copy our data into the destination vspace. */
    //int nbytes = PAGESIZE - PAGE_OFFSET(cont->dst);
    if (cont->pos < cont->file_size){
        memcpy((void*)kdst, (void*)data, count);
    }

    err = sos_get_kframe_cap(cont->as, PAGE_ALIGN(cont->dst), &sos_cap);
    if (err) {
        load_segment_into_vspace_end(cont, err);
        return;
    }

    /* Not observable to I-cache yet so flush the frame */
    seL4_ARM_Page_Unify_Instruction(sos_cap, 0, PAGESIZE);

    cont->pos += count;
    cont->dst += count;

    load_segment_into_vspace2((void*)cont, err);
}

static void
load_segment_into_vspace3(void* token, int err){
    printf("load segment into vspace3\n");
    load_segment_cont_t* cont = (load_segment_cont_t*)token;
    if (err) {
        load_segment_into_vspace_end(cont, err);
        return;
    }

    /* Reset the wanna_read and read from nfs */
    cont->wanna_read = MIN(NFS_SEND_SIZE, cont->file_size - cont->pos);
    cont->wanna_read = MIN(cont->wanna_read, PAGE_SIZE - PAGE_OFFSET(cont->dst));
    printf("load_segment_into_vspace3: wanna_read = %lu\n", cont->wanna_read);

    /* Attemp to read from nfs */
    enum rpc_stat rpc_status = nfs_read(cont->fh, cont->file_offset + cont->pos,
            cont->wanna_read, load_segment_into_vspace4, (uintptr_t)cont);
    if (rpc_status != RPC_OK) {
        load_segment_into_vspace_end(cont, EFAULT);
    }
}

static void load_segment_into_vspace2(void* token, int err){
    printf("load segment into vspace2\n");

    load_segment_cont_t* cont = (load_segment_cont_t*)token;

    /* We copy chunk by chunk to the destination vspace. */
    if(cont->pos < cont->file_size){
        seL4_Word vpage;
        vpage = PAGE_ALIGN(cont->dst);

        //printf("load segment into vspace2 page map\n");
        if (!sos_page_is_inuse(cont->as, vpage)) {
            sos_page_map(cont->as, vpage, cont->permissions, load_segment_into_vspace3, token, false);
            inc_proc_size_proc(cont->proc);
        } else {
            load_segment_into_vspace3(token, 0);
        }
        return;
    }

    //printf("load segment into vspace2 out\n");
    load_segment_into_vspace_end(cont, 0);
}

static int load_segment_into_vspace(addrspace_t *as,
                                    unsigned long file_offset,
                                    unsigned long segment_size,
                                    unsigned long file_size, unsigned long dst,
                                    unsigned long permissions,
                                    process_t* proc, fhandle_t *fh,
                                    load_segment_cb_t callback, void* token) {
    printf("load segment into vspace\n");
    assert(file_size <= segment_size);

    load_segment_cont_t* cont = malloc(sizeof(load_segment_cont_t));
    if(cont == NULL){
        return ENOMEM;
    }
    cont->as = as;
    cont->file_offset = file_offset;
    cont->segment_size = segment_size;
    cont->file_size = file_size;
    cont->dst = dst;
    cont->permissions = permissions;
    cont->pos = 0;
    cont->wanna_read = 0;
    cont->proc = proc;
    cont->fh = fh;
    cont->callback = callback;
    cont->token = token;

    load_segment_into_vspace2((void*)cont, 0);

    return 0;
}


typedef struct {
    int i;
    addrspace_t* as;
    //char* elf_file;
    char* file_name;
    process_t* proc;
    fhandle_t *fh;
    seL4_Word elf_entry;
    struct Elf32_Phdr *prog_hdrs;
    int ph_num;     /* Program header number */
    int ph_size;    /* Program header size */
    elf_load_cb_t callback;
    void* token;
} elf_load_cont_t;

static void
elf_load_end(void *token, int err) {
    printf("elf_load_end called\n");
    elf_load_cont_t *cont = (elf_load_cont_t*)token;

    if (cont->fh != NULL) {
        free(cont->fh);
    }
    if (cont->prog_hdrs != NULL) {
        free(cont->prog_hdrs);
    }
    cont->callback(cont->token, err, (err) ? 0 : cont->elf_entry);
    free(cont);
}

static void
elf_load_part2(void* token, int err){
    printf("elf_load... part 2\n");

    elf_load_cont_t* cont = (elf_load_cont_t*)token;
    if (err) {
        elf_load_end((void*)cont, err);
        return;
    }

    if(cont->i < cont->ph_num) {
        printf("cont->i = %d, cont->ph_num = %d\n", cont->i,cont->ph_num);
        unsigned long flags, file_offset, file_size, segment_size, vaddr, rights;

        /* Skip non-loadable segments (such as debugging data). */
        if (cont->prog_hdrs[cont->i].p_type != PT_LOAD){
            cont->i++;
            elf_load_part2(token, 0);
            return;
        }

        /* Fetch information about this segment. */
        file_offset     = cont->prog_hdrs[cont->i].p_offset;
        file_size       = cont->prog_hdrs[cont->i].p_filesz;
        segment_size    = cont->prog_hdrs[cont->i].p_memsz;
        vaddr           = cont->prog_hdrs[cont->i].p_vaddr;
        flags           = cont->prog_hdrs[cont->i].p_flags;
        rights          = get_sel4_rights_from_elf(flags) & seL4_AllRights;

        /* Define the region */
        dprintf(1, " * Loading segment %08x-->%08x\n", (int)vaddr, (int)(vaddr + segment_size));

        err = as_define_region(cont->as, vaddr, segment_size, rights);
        if (err) {
            elf_load_end((void*)cont, err);
            return;
        }
        int i = cont->i;
        cont->i++;

        /* Copy it across into the vspace. */
        err = load_segment_into_vspace(cont->as, file_offset, segment_size, file_size,
                                       vaddr, rights, cont->proc, cont->fh, elf_load_part2, (void*)cont);
        if (err) {
            elf_load_end((void*)cont, err);
            return;
        }

        return;
    }

    elf_load_end((void*)cont, 0);
}

static void
elf_load_read_progheaders_cb(uintptr_t token, enum nfs_stat status,
                              fattr_t *fattr, int count, void* data) {
    printf("elf_load_read_progheaders_cb called\n");
    elf_load_cont_t* cont = (elf_load_cont_t*)token;

    if (status != NFS_OK || count != cont->ph_size * cont->ph_num) {
        printf("elf_load_read_progheaders_cb: err\n");
        elf_load_end((void*)cont, EFAULT);
        return;
    }

    cont->prog_hdrs = malloc(cont->ph_num * cont->ph_size);
    if (cont->prog_hdrs == NULL) {
        printf("elf_load_read_progheaders_cb: no memory for prog_hdrs\n");
        elf_load_end((void*)cont, ENOMEM);
        return;
    }
    memcpy(cont->prog_hdrs, data, cont->ph_num * cont->ph_size);

    elf_load_part2((void*)cont, 0);
}

static void
elf_load_read_elfheader_cb(uintptr_t token, enum nfs_stat status,
                              fattr_t *fattr, int count, void* data) {
    printf("elf_load_read_elfheader_cb called\n");
    elf_load_cont_t* cont = (elf_load_cont_t*)token;

    if (status != NFS_OK || count != sizeof(struct Elf32_Header)) {
        printf("elf_load_read_elfheader_cb: err\n");
        elf_load_end((void*)cont, EFAULT);
        return;
    }

    struct Elf32_Header *elf_hdr = (struct Elf32_Header*)data;
    if (elf_checkFile(elf_hdr)) {
        printf("elf_load_read_elfheader_cb: elf_check failed\n");
        elf_load_end((void*)cont, EINVAL);
        return;
    }
    int ph_off      = elf_hdr->e_phoff;
    cont->ph_num    = elf_hdr->e_phnum;
    cont->ph_size   = elf_hdr->e_phentsize;
    cont->elf_entry = elf_hdr->e_entry;

    printf("ph_num = %d\n", cont->ph_num);
    printf("ph_size = %d\n", cont->ph_size);
    printf("elf_entry = %d\n", cont->elf_entry);

    int wanna_read = cont->ph_size * cont->ph_num;
    enum rpc_stat rpc_status = nfs_read(cont->fh, ph_off,
            wanna_read, elf_load_read_progheaders_cb, (uintptr_t)cont);
    if (rpc_status != RPC_OK) {
        printf("elf_load_read_elfheader_cb: nfs_read failed\n");
        elf_load_end((void*)cont, EFAULT);
    }
}

static void
elf_load_lookup_cb(uintptr_t token, enum nfs_stat status, fhandle_t* fh, fattr_t* fattr) {
    printf("elf_load_lookup_cb called\n");
    elf_load_cont_t* cont = (elf_load_cont_t*)token;

    if (status != NFS_OK) {
        printf("elf_load_lookup_cb: nfs_lookup status != NFS_OK\n");
        elf_load_end((void*)cont, EFAULT);
        return;
    }

    cont->fh = malloc(sizeof(fhandle_t));
    if(cont->fh == NULL){
        printf("elf_load_lookup_cb: no mem for fh\n");
        elf_load_end((void*)cont, ENOMEM);
        return;
    }
    memcpy(cont->fh->data, fh->data, sizeof(fh->data));

    enum rpc_stat rpc_status = nfs_read(cont->fh, 0, sizeof(struct Elf32_Header),
            elf_load_read_elfheader_cb, (uintptr_t)cont);
    if (rpc_status != RPC_OK) {
        printf("elf_load_lookup_cb: nfs_read failed\n");
        elf_load_end((void*)cont, EFAULT);
    }
}

void elf_load(addrspace_t* as, char *file_name, process_t* proc, elf_load_cb_t callback, void* token) {

    printf("Starting elf_load...\n");

    elf_load_cont_t* cont = malloc(sizeof(elf_load_cont_t));
    if (cont == NULL) {
        callback(token, ENOMEM, 0);
        return;
    }

    cont->i             = 0;
    cont->as            = as;
    cont->file_name     = file_name;
    cont->proc          = proc;
    cont->fh            = NULL;
    cont->elf_entry     = 0;
    cont->prog_hdrs     = NULL;
    cont->ph_num        = 0;
    cont->ph_size       = 0;
    cont->callback      = callback;
    cont->token         = token;

    enum rpc_stat status = nfs_lookup(&mnt_point, file_name, elf_load_lookup_cb, (uintptr_t)cont);
    if (status != RPC_OK) {
        printf("elf_load nfs_lookup err\n");
        elf_load_end((void*)cont, EFAULT);
    }
}
