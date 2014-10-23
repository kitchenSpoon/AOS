#ifndef _LIBOS_ADDRSPACE_H_
#define _LIBOS_ADDRSPACE_H_

#include <sel4/sel4.h>
#include <cspace/cspace.h>

#define SEL4_N_PAGETABLES       (1<<12)

#define INDEX_1_MASK                (0xffc00000)
#define INDEX_2_MASK                (0x003ff000)
#define PT_L1_INDEX(a)              (((a) & INDEX_1_MASK) >> 22)
#define PT_L2_INDEX(a)              (((a) & INDEX_2_MASK) >> 12)
#define PT_ID_TO_VPAGE(id1, id2)    (((id1) << 22) | ((id2) << 12))

#define PTE_IN_USE_BIT          (1)
#define PTE_SWAPPED             (1<<1)
#define PTE_SWAP_OFFSET         (2)
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

/* sel4's pagetable link list node */
typedef struct sel4_pt_node sel4_pt_node_t;
struct sel4_pt_node {
    seL4_ARM_PageTable pt;
    seL4_Word pt_addr;
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

/***********************************************************************
 *
 * Functions in addrspace.c
 *
 **********************************************************************/

/* Find and return the region that this address is in */
region_t* region_probe(struct addrspace* as, seL4_Word addr);

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

/* SOS's handler for sys_brk system call */
seL4_Word sos_sys_brk(addrspace_t *as, seL4_Word vaddr);

/*
 * check if the given user buffer is a valid memory range
 * Note: permission can be NULL, in that case no permission is returned
 */
bool as_is_valid_memory(addrspace_t *as, seL4_Word vaddr, size_t size,
                                uint32_t* permission);

/***********************************************************************
 *
 * Functions in pagetable.c:
 *
 ***********************************************************************/

/* Callback type for their corresponding functions */
typedef void (*sos_page_map_cb_t)(void *token, int err);

/*
 * Map a page in into the shadow Pagetable
 * This is an asynchronous function and will return by calling the call back
 * function
 * Returns 0 if succesfully register callback
 */
int sos_page_map(int pid, addrspace_t *as, seL4_Word vaddr, uint32_t permissions,
                 sos_page_map_cb_t callback, void* token, bool noswap);

/*
 * Unmap a page in the pagetable.
 * Note that this does not actually free the page in user pagetable, it only
 * unmaps the page from sel4 and free the frame_cap
 * Returns 0 if successful
 */
int sos_page_unmap(addrspace_t *as, seL4_Word vaddr);

/*
 * Free a page, this will unmap the page before removing the page so that it
 * can be reused
 */
void sos_page_free(addrspace_t *as, seL4_Word vaddr);

/*
 * sos_page_is_inuse    - Check if page at address VADDR currently in use
 * sos_page_is_swapped  - Check if page at address VADDR is swapped
 * sos_page_is_locked   - Check if the underlying frame is locked
 * sos_get_kvaddr       - Get the SOS's vaddr from the given application's ADDR in AS
 * sos_get_kframe_cap   - Get the kframe_cap from the given ADDR in AS
 */
bool sos_page_is_inuse(addrspace_t *as, seL4_Word vaddr);
bool sos_page_is_swapped(addrspace_t *as, seL4_Word vaddr);
bool sos_page_is_locked(addrspace_t *as, seL4_Word vaddr);
int sos_get_kvaddr(addrspace_t *as, seL4_Word vaddr, seL4_Word *kvaddr);
int sos_get_kframe_cap(addrspace_t *as, seL4_Word vaddr, seL4_CPtr *kframe_cap);

#endif /* _LIBOS_ADDRSPACE_H_ */
