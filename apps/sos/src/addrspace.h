#ifndef _LIBOS_ADDRSPACE_H_
#define _LIBOS_ADDRSPACE_H_

#include <sel4/sel4.h>
#include <cspace/cspace.h>

#define PAGE_IS_OK         (0)
#define PAGE_IS_FAIL       (-1)

typedef
struct _pagetable_entry{
    int status;             // Either USED or FREE
    seL4_CPtr kframe_cap;    // frame cap of the allocated frame or NULL
    seL4_CPtr frame_cap;    // frame cap of the allocated frame or NULL
    seL4_Word kvaddr;
} pagetable_entry_t;

typedef pagetable_entry_t** pagetable_t;
typedef pagetable_t* pagedir_t;


#define AS_REGION_R     (1)
#define AS_REGION_W     (2)
#define AS_REGION_ALL   (3)

typedef struct region region_t;
struct region {
    seL4_Word vbase, vtop; /* valid addr in this region [vabase, vtop) */
    uint32_t rights;
    region_t *next;
};
    
typedef
struct addrspace {
    pagedir_t as_pd;
    region_t *as_rhead;
    region_t *as_stack;
    region_t *as_heap;
    int as_loading;        // to ignore readonly permission on loading
} addrspace_t;

/*
 * Functions in addrspace.c:
 *
 *    as_create - create a new empty address space. You need to make
 *                sure this gets called in all the right places. You
 *                may find you want to change the argument list. May
 *                return NULL on out-of-memory error.
 *
 *    as_destroy - dispose of an address space. You may need to change
 *                the way this works if implementing user-level threads.
 *
 *    as_define_region - set up a region of memory within the address
 *                space.
 *
 *    as_define_stack - set up the stack region in the address space.
 *                (Normally called *after* as_complete_load().) Hands
 *                back the initial stack pointer for the new process.
 *
 *    as_define_heap - set up the heap region in the address space.
 */
addrspace_t      *as_create(void);
void              as_destroy(addrspace_t *as);
int               as_define_region(addrspace_t *as,
                                   seL4_Word vaddr,
                                   size_t sz,
                                   int32_t rights);
int               as_define_stack(addrspace_t *as, seL4_Word stack_top, int size);
int               as_define_heap(addrspace_t *as);

/*
 * Functions in elf.c
 *    elf_load - load an ELF user program executable into the current
 *               address space. (i.e. the only one address space )
 */
int elf_load(addrspace_t *as, seL4_ARM_PageDirectory dest_pd, char* elf_file);

/*
 * Functions in pagetable.c:
 *
 *    sos_map_page - create and map a page
 *
 *    sos_unmap_page - unmap a page
 */

/*
 * Map a page in into the page table
 * Returns PAGE_IS_OK if succesful
 */
int sos_page_map(addrspace_t *as,
                 seL4_ARM_PageDirectory app_sel4_pd, cspace_t *app_cspace,
                 seL4_Word vaddr, seL4_Word* kvaddr);
/*
 * Unmap a page in into the page table
 * Returns PAGE_IS_OK if successful
 */
int sos_page_unmap(pagedir_t* pd, seL4_Word vaddr);
#endif /* _LIBOS_ADDRSPACE_H_ */
