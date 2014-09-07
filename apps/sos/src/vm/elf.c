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
#include "vm/vmem_layout.h"
#include "tool/utility.h"

#define verbose 2
#include <sys/debug.h>
#include <sys/panic.h>

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
static int load_segment_into_vspace(addrspace_t *as, char *src,
                                    unsigned long segment_size,
                                    unsigned long file_size, unsigned long dst,
                                    unsigned long permissions) {
    assert(file_size <= segment_size);
    unsigned long pos;

    /* We work a page at a time in the destination vspace. */
    pos = 0;
    while(pos < segment_size) {
        seL4_Word vpage;
        seL4_Word kdst;
        seL4_CPtr sos_cap;
        int nbytes;
        int err;

        vpage = PAGE_ALIGN(dst);
        err = sos_page_map(as, vpage, permissions);
        if (err) {
            return err;
        }
        err = sos_get_kvaddr(as, dst, &kdst);
        if (err) {
            return err;
        }

        /* Now copy our data into the destination vspace. */
        nbytes = PAGESIZE - (dst & PAGEMASK);
        if (pos < file_size){
            memcpy((void*)kdst, (void*)src, MIN(nbytes, file_size - pos));
        }

        err = sos_get_kframe_cap(as, vpage, &sos_cap);
        if (err) {
            return err;
        }
        /* Not observable to I-cache yet so flush the frame */
        seL4_ARM_Page_Unify_Instruction(sos_cap, 0, PAGESIZE);

        pos += nbytes;
        dst += nbytes;
        src += nbytes;
    }
    return 0;
}

int elf_load(addrspace_t* as, char *elf_file) {

    int num_headers;
    int err;
    int i;

    /* Ensure that the ELF file looks sane. */
    if (elf_checkFile(elf_file)){
        return seL4_InvalidArgument;
    }

    printf("Starting elf_load...\n");
    num_headers = elf_getNumProgramHeaders(elf_file);
    for (i = 0; i < num_headers; i++) {
        char *source_addr;
        unsigned long flags, file_size, segment_size, vaddr, rights;

        /* Skip non-loadable segments (such as debugging data). */
        if (elf_getProgramHeaderType(elf_file, i) != PT_LOAD)
            continue;

        /* Fetch information about this segment. */
        source_addr = elf_file + elf_getProgramHeaderOffset(elf_file, i);
        file_size = elf_getProgramHeaderFileSize(elf_file, i);
        segment_size = elf_getProgramHeaderMemorySize(elf_file, i);
        vaddr = elf_getProgramHeaderVaddr(elf_file, i);
        flags = elf_getProgramHeaderFlags(elf_file, i);
        rights = get_sel4_rights_from_elf(flags) & seL4_AllRights;

        /* Define the region */
        dprintf(1, " * Loading segment %08x-->%08x\n", (int)vaddr, (int)(vaddr + segment_size));

        err = as_define_region(as, vaddr, segment_size, rights);
        conditional_panic(err, "Elf loading failed to define region\n");

        /* Copy it across into the vspace. */
        err = load_segment_into_vspace(as, source_addr, segment_size, file_size,
                                       vaddr, rights);
        if (err) {
            return EFAULT;
        }
    }
    printf("Finished elf_load\n");

    return 0;
}
