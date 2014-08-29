#ifndef _LIBOS_ADDRSPACE_H_
#define _LIBOS_ADDRSPACE_H_

#include <sel4/sel4.h>
#include <cspace/cspace.h>

#define SEL4_N_PAGETABLES       (1<<12)

#define PTE_IN_USE_BIT          (1)
#define PTE_KVADDR_MASK         (0xfffff000)

/* Pagetable related defs */
typedef seL4_Word* pagetable_t;
typedef pagetable_t* pagedir_t;

/* Region defs */
typedef struct region region_t;
struct region {
    seL4_Word vbase, vtop;  // valid addr in this region [vabase, vtop)
    uint32_t rights;        // same format as seL4's seL4_CapRights for frame caps
    region_t *next;         // link to the next region
};

/* sel4's pagetable link list nodes */
typedef struct sel4_pt_node sel4_pt_node_t;
struct sel4_pt_node {
    seL4_ARM_PageTable pt;
    sel4_pt_node_t *next;
};

/* The address space used in every user process */
typedef
struct addrspace {
    pagedir_t as_pd_caps;
    pagedir_t as_pd_regs;
    region_t *as_rhead;
    region_t *as_stack;
    region_t *as_heap;
    sel4_pt_node_t* as_pt_head;
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
 *    sos_map_page - create and map a page into indicated address for user
 *              level application
 *
 *    sos_unmap_page - unmap a page
 *    
 *    sos_get_kframe_cap - get kframe_cap
 *    
 *    sos_get_kvaddr - get kvaddr
 */

/*
 * Map a page in into the shadow Pagetable
 * @param as - The addrspace we will perform the mapping
 * @param app_sel4_pd - the sel4 page directory of the user level app
 * @param vaddr - the user level virtual address that need to be mapped
 * 
 * @Returns 0 if succesful
 */
int sos_page_map(addrspace_t *as, seL4_ARM_PageDirectory app_sel4_pd, seL4_Word vaddr, uint32_t permissions);

/*
 * Unmap a page in into the page table
 * Returns 0 if successful
 */
int sos_page_unmap(pagedir_t* pd, seL4_Word vaddr);

/* Check if page at address VADDR is mapped */
bool sos_page_is_mapped(addrspace_t *as, seL4_Word vaddr);

/* Get the kframe_cap from the given ADDR in AS */
int sos_get_kframe_cap(addrspace_t *as, seL4_Word vaddr, seL4_CPtr *kframe_cap);

/* Get the SOS's vaddr from the given application's ADDR in AS */
int sos_get_kvaddr(addrspace_t *as, seL4_Word vaddr, seL4_Word *kvaddr);

/* SOS's handler for sys_brk system call */
seL4_Word sos_sys_brk(seL4_Word vaddr, addrspace_t *as);

#endif /* _LIBOS_ADDRSPACE_H_ */
