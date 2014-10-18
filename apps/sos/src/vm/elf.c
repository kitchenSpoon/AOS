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

#include "vm/mapping.h"
#include "vm/addrspace.h"
#include "vm/elf.h"
#include "vm/vmem_layout.h"
#include "tool/utility.h"

#define verbose 2
#include <sys/debug.h>
#include <sys/panic.h>

extern fhandle_t mnt_point;

static void load_segment_into_vspace2(void* token, int err);

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

typedef struct {
    addrspace_t* as;
    char* src;
    unsigned long segment_size;
    unsigned long file_size;
    unsigned long dst;
    unsigned long permissions;
    unsigned long pos;
    process_t* proc;
    load_segment_cb_t callback;
    void* token;
} load_segment_cont_t;

static void
load_segment_into_vspace3(void* token, int err){
    //printf("load segment into vspace3\n");
    load_segment_cont_t* cont = (load_segment_cont_t*)token;
    if (err) {
        cont->callback(cont->token, err);
        free(cont);
        return;
    }

    printf("load segment, incr proc size of %d\n",cont->proc->pid);
    inc_proc_size_proc(cont->proc);

    seL4_Word vpage;
    vpage = PAGE_ALIGN(cont->dst);

    seL4_Word kdst;
    seL4_CPtr sos_cap;

    err = sos_get_kvaddr(cont->as, cont->dst, &kdst);
    if (err) {
        cont->callback(cont->token, err);
        free(cont);
        return;
    }

    /* Now copy our data into the destination vspace. */
    int nbytes = PAGESIZE - (cont->dst & PAGE_OFFSET_MASK);
    if (cont->pos < cont->file_size){
        memcpy((void*)kdst, (void*)(cont->src), MIN(nbytes, cont->file_size - cont->pos));
    }

    err = sos_get_kframe_cap(cont->as, vpage, &sos_cap);
    if (err) {
        cont->callback(cont->token, err);
        free(cont);
        return;
    }

    /* Not observable to I-cache yet so flush the frame */
    seL4_ARM_Page_Unify_Instruction(sos_cap, 0, PAGESIZE);

    cont->pos += nbytes;
    cont->dst += nbytes;
    cont->src += nbytes;

    load_segment_into_vspace2(token, err);
}

static void load_segment_into_vspace2(void* token, int err){
    //printf("load segment into vspace2\n");

    load_segment_cont_t* cont = (load_segment_cont_t*)token;

    /* We work a page at a time in the destination vspace. */
    if(cont->pos < cont->segment_size){
        seL4_Word vpage;
        vpage = PAGE_ALIGN(cont->dst);

        //printf("load segment into vspace2 page map\n");
        sos_page_map(cont->as, vpage, cont->permissions, load_segment_into_vspace3, token, false);
        return;
    }

    //printf("load segment into vspace2 out\n");
    cont->callback(cont->token, 0);
    free(cont);
}


static int load_segment_into_vspace(addrspace_t *as, char *src,
                                    unsigned long segment_size,
                                    unsigned long file_size, unsigned long dst,
                                    unsigned long permissions, process_t* proc, load_segment_cb_t callback, void* token) {
    //printf("load segment into vspace\n");
    assert(file_size <= segment_size);

    load_segment_cont_t* cont = malloc(sizeof(load_segment_cont_t));
    if(cont == NULL){
        return ENOMEM;
    }
    cont->as = as;
    cont->src = src;
    cont->segment_size = segment_size;
    cont->file_size = file_size;
    cont->dst = dst;
    cont->permissions = permissions;
    cont->pos = 0;
    cont->proc = proc;
    cont->callback = callback;
    cont->token = token;

    load_segment_into_vspace2((void*)cont, 0);

    return 0;
}
//static int load_segment_into_vspace(addrspace_t *as, char *src,
//                                    unsigned long segment_size,
//                                    unsigned long file_size, unsigned long dst,
//                                    unsigned long permissions) {
//    assert(file_size <= segment_size);
//    unsigned long pos;
//
//    /* We work a page at a time in the destination vspace. */
//    pos = 0;
//    while(pos < segment_size) {
//        seL4_Word vpage;
//        seL4_Word kdst;
//        seL4_CPtr sos_cap;
//        int nbytes;
//        int err;
//
//        vpage = PAGE_ALIGN(dst);
//        err = sos_page_map(as, vpage, permissions);
//        if (err) {
//            return err;
//        }
//        err = sos_get_kvaddr(as, dst, &kdst);
//        if (err) {
//            return err;
//        }
//
//        /* Now copy our data into the destination vspace. */
//        nbytes = PAGESIZE - (dst & PAGE_OFFSET_MASK);
//        if (pos < file_size){
//            memcpy((void*)kdst, (void*)src, MIN(nbytes, file_size - pos));
//        }
//
//        err = sos_get_kframe_cap(as, vpage, &sos_cap);
//        if (err) {
//            return err;
//        }
//        /* Not observable to I-cache yet so flush the frame */
//        seL4_ARM_Page_Unify_Instruction(sos_cap, 0, PAGESIZE);
//
//        pos += nbytes;
//        dst += nbytes;
//        src += nbytes;
//    }
//    return 0;
//}


typedef struct {
    int i;
    addrspace_t* as;
    //char* elf_file;
    char* file_name;
    process_t* proc;
    filehandle_t *fh;
    Elf32_Phdr_t *prog_hdrs;
    int ph_num;     /* Program header number */
    int ph_size;    /* Program header size */
    elf_load_cb_t callback;
    void* token;
} elf_load_cont_t;

static void
elf_load_end(void *token, int err) {
    elf_load_cont_t *cont = (elf_load_cont_t*)token;

    if (err) {
        //TODO:

        return;
    }
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
        printf("cont->i = %d, cont->num_headers = %d\n", cont->i,cont->num_headers);
        char *source_offset;
        unsigned long flags, file_size, segment_size, vaddr, rights;

        /* Skip non-loadable segments (such as debugging data). */
        if (elf_getProgramHeaderType(cont->elf_file, cont->i) != PT_LOAD){
            cont->i++;
            elf_load_part2(token, 0);
            return;
        }

        /* Fetch information about this segment. */
//        source_addr = cont->elf_file + elf_getProgramHeaderOffset(cont->elf_file, cont->i);
//        file_size = elf_getProgramHeaderFileSize(cont->elf_file, cont->i);
//        segment_size = elf_getProgramHeaderMemorySize(cont->elf_file, cont->i);
//        vaddr = elf_getProgramHeaderVaddr(cont->elf_file, cont->i);
//        flags = elf_getProgramHeaderFlags(cont->elf_file, cont->i);
//        rights = get_sel4_rights_from_elf(flags) & seL4_AllRights;

        source_offset   = cont->prog_hdrs[cont->i].p_offset;
        file_size       = cont->prog_hdrs[cont->i].p_filesz;
        segment_size    = cont->prog_hdrs[cont->i].p_memsz;
        vaddr           = cont->prog_hdrs[cont->i].p_vaddr;
        flags           = cont->prog_hdrs[cont->i].p_flags;
        rights          = get_sel4_rights_from_elf(flags) & seL4_AllRights;
        /* Define the region */
        dprintf(1, " * Loading segment %08x-->%08x\n", (int)vaddr, (int)(vaddr + segment_size));

        printf("elf_load... part 2.2\n");
        err = as_define_region(cont->as, vaddr, segment_size, rights);
        if (err) {
            printf("elf_load... part 2 err2 \n");
            elf_load_end((void*)cont, err);
            return;
        }
        int i = cont->i;
        cont->i++;

        printf("elf_load... part 2.3\n");
        /* Copy it across into the vspace. */
        err = load_segment_into_vspace(cont->as, source_offset, segment_size, file_size,
                                       vaddr, rights, cont->proc, elf_load_part2, (void*)cont);
        printf("elf_load... part 2.4, i = %d, ori i = %d\n\n\n\n",cont->i, i);
        if (err) {
            printf("elf_load... part 2 err load segment\n");
            elf_load_end((void*)cont, err);
            return;
        }

        return;
    }

    printf("elf_load... calling back up\n");
    elf_load_end((void*)cont, 0);
}

static void
elf_load_read_progheaders_cb(uintptr_t token, enum nfs_stat status,
                              fattr_t *fattr, int count, void* data) {
    elf_load_cont_t* cont = (elf_load_cont_t*)token;

    if (status != NFS_OK || count != sizeof(Elf32_Header_t)) {
        elf_load_end((void*)cont, EFAULT);
        return;
    }

    cont->prog_hdrs = malloc(cont->ph_num * cont->ph_size);
    if (cont->prog_hdrs == NULL) {
        elf_load_end((void*)cont, ENOMEM);
        return;
    }
    memcpy(cont->prog_hdrs, data, cont->ph_num * cont->ph_size);

    elf_load_part2((void*)cont, 0);
}

static void
elf_load_read_elfheader_cb(uintptr_t token, enum nfs_stat status,
                              fattr_t *fattr, int count, void* data) {
    elf_load_cont_t* cont = (elf_load_cont_t*)token;

    if (status != NFS_OK || count != sizeof(Elf32_Header_t)) {
        elf_load_end((void*)cont, EFAULT);
        return;
    }

    Elf32_Header_t *elf_hdr = (Elf32_Header_t*)data;
    int ph_off      = elf_hdr.e_phoff;
    cont->ph_num    = elf_hdr.ephnum;
    cont->ph_size   = elf_hdr.e_phentsize;

    int wanna_read = cont->ph_size * cont->ph_num;
    enum rpc_stat status = nfs_read(cont->fh, ph_off,
            wanna_read, elf_load_read_progheaders_cb, (uintptr_t)cont);
    if (status != RPC_OK) {
        elf_load_end((void*)cont, EFAULT);
    }
}

static void
elf_load_lookup_cb(uintptr_t token, enum nfs_stat status, fhandle_t* fh, fattr_t* fattr) {
    elf_load_cont_t* cont = (elf_load_cont_t*)token;

    if (status != NFS_OK) {
        elf_load_end((void*)cont, EFAULT);
        return;
    }

    cont->fh = malloc(sizeof(fhandle_t));
    if(cont->fh == NULL){
        elf_load_end((void*)cont, ENOMEM);
        return;
    }
    memcpy(cont->fh->data, fh->data, sizeof(fh->data));

    enum rpc_stat status = nfs_read(cont->fh, 0, sizeof(Elf32_Header_t),
            elf_load_read_elfheader_cb, (uintptr_t)cont);
    if (status != RPC_OK) {
        elf_load_end((void*)cont, EFAULT);
    }
}

void elf_load(addrspace_t* as, char *file_name, process_t* proc, elf_load_cb_t callback, void* token) {

    int num_headers;

    /* Ensure that the ELF file looks sane. */
    //TODO: check header instead
//    if (elf_checkFile(elf_file)){
//        //callback(token, seL4_InvalidArgument);
//        callback(token, EINVAL);
//        return;
//    }

    printf("Starting elf_load...\n");
//    num_headers = elf_getNumProgramHeaders(elf_file);

    elf_load_cont_t* cont = malloc(sizeof(elf_load_cont_t));
    if (cont == NULL) {
        callback(token, ENOMEM);
        return;
    }

    cont->i = 0;
    cont->as = as;
    //cont->elf_file = elf_file;
    cont->file_name = file_name;
    cont->num_headers = 0;
    //cont->num_headers = num_headers;
    cont->proc = proc;
    cont->callback = callback;
    cont->token = token;
    //TODO: init everything

    enum rpc_stat status = nfs_lookup(&mnt_point, file_name, elf_load_lookup_cb, (uintptr_t)cont);
    if (status != RPC_OK) {
        elf_load_end((void*)cont, EFAULT);
        return;
    }

}
