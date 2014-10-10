#ifndef _LIBOS_ADDRSPACE_H_
#define _LIBOS_ADDRSPACE_H_

#include <sel4/sel4.h>
#include <cspace/cspace.h>

#define SEL4_N_PAGETABLES       (1<<12)

#define PTE_IN_USE_BIT          (1)
#define PTE_SWAPPED             (1<<1)
#define PTE_SWAP_OFFSET         (2)
#define PTE_KVADDR_OFFSET       (2)
#define PTE_SWAP_MASK           (0xfffffffc)
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
    seL4_ARM_PageDirectory as_sel4_pd;
    sel4_pt_node_t* as_pt_head;
} addrspace_t;

/*
 * Functions in addrspace.c
 *
 */

/* Callback for as_create function, to be called when as_create finished and
 * want to reply to the caller */
typedef void (*as_create_cb_t)(void *token, addrspace_t *as);

/*
 * Create a new empty address space.
 * This function is asynchronous.
 * RETURN: if the immediate return value is 0, new addrspace will be returned via the callback
 *         if the immediate return is not 0, it fails and callback will not be called
 */
int as_create(seL4_ARM_PageDirectory sel4_pd, as_create_cb_t callback, void *token);

/*
 * Dispose of an address space.
 */
void as_destroy(addrspace_t *as);

/*
 * set up a region of memory within the address space.
 */
int as_define_region(addrspace_t *as, seL4_Word vaddr,
                              size_t sz, int32_t rights);

/*
 * set up the stack region in the address space.
 * Hands back the initial stack pointer for the new process.
 */
int as_define_stack(addrspace_t *as, seL4_Word stack_top, int size);

/*
 * set up the heap region in the address space.
 */
int as_define_heap(addrspace_t *as);

/*
 * check if the given user buffer is a valid memory range
 */
bool as_is_valid_memory(addrspace_t *as, seL4_Word vaddr, size_t size,
                                uint32_t* permission);

/*
 * Functions in elf.c
 *    elf_load - load an ELF user program executable into the current
 *               address space. (i.e. the only one address space )
 */
typedef void (*elf_load_cb_t)(void *token, int err);
void elf_load(addrspace_t *as, char* elf_file, elf_load_cb_t callback, void* token);

/*
 * Functions in pagetable.c:
 *
 */

/*
 * Map a page in into the shadow Pagetable
 * @param as - The addrspace we will perform the mapping
 * @param app_sel4_pd - the sel4 page directory of the user level app
 * @param vaddr - the user level virtual address that need to be mapped
 *
 * @Returns 0 if succesful
 */
typedef void (*sos_page_map_cb_t)(void *token, int err);
int sos_page_map(addrspace_t *as, seL4_Word vaddr, uint32_t permissions,
        sos_page_map_cb_t callback, void* token, bool noswap);

/*
 * Unmap a page in into the page table
 * Returns 0 if successful
 */
int sos_page_unmap(addrspace_t *as, seL4_Word vaddr);

/* Check if page at address VADDR is swapped */
bool sos_page_is_swapped(addrspace_t *as, seL4_Word vaddr);

/* Check if page at address VADDR is mapped */
bool sos_page_is_mapped(addrspace_t *as, seL4_Word vaddr);

/* Get the kframe_cap from the given ADDR in AS */
int sos_get_kframe_cap(addrspace_t *as, seL4_Word vaddr, seL4_CPtr *kframe_cap);

/* Get the SOS's vaddr from the given application's ADDR in AS */
int sos_get_kvaddr(addrspace_t *as, seL4_Word vaddr, seL4_Word *kvaddr);

/* SOS's handler for sys_brk system call */
seL4_Word sos_sys_brk(addrspace_t *as, seL4_Word vaddr);

#endif /* _LIBOS_ADDRSPACE_H_ */
