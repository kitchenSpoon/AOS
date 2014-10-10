#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <strings.h>
#include <limits.h>

#include <errno.h>
#include <sel4/sel4.h>
#include <nfs/nfs.h>

#include "tool/utility.h"
#include "vfs/vfs.h"
#include "vm/swap.h"
#include "dev/nfs_dev.h"
#include "vm/addrspace.h"
#include "vm/vm.h"

/*
 * Bitmap use to track free slots in our swap file
 * 1 is used
 * 0 is free
 */
#define NUM_CHUNKS (32)
#define NUM_BITS (32)
#define SWAP_FILE_NAME  "swap"

#define NFS_SEND_SIZE   1024 //This needs to be less than UDP package size


#define INDEX_1_MASK        (0xffc00000)
#define INDEX_2_MASK        (0x003ff000)
#define PT_L1_INDEX(a)      (((a) & INDEX_1_MASK) >> 22)
#define PT_L2_INDEX(a)      (((a) & INDEX_2_MASK) >> 12)

uint32_t free_slots[NUM_CHUNKS];

fhandle_t *swap_fh;

static void
_unset_slot(int slot){
    free_slots[slot/NUM_CHUNKS] &= ~(1u<<(slot%NUM_CHUNKS));
    return;
}

static void
_set_slot(int slot){
    free_slots[slot/NUM_CHUNKS] |= 1u<<(slot%NUM_CHUNKS);
    return;
}

static int
swap_find_free_slot(void){
    for(uint32_t i = 0; i < NUM_CHUNKS; i++){
        for(uint32_t j = 0; j < NUM_BITS; j++){
            //printf("i = %d, j = %d, free_slots[i] in decimal = %u\n", i, j, free_slots[i]);
            if(!(free_slots[i] & (1u<<j))){
                return i*NUM_CHUNKS + j;
            }
        }
    }

    return -1;
}

typedef void (*swap_init_cb_t)(void *token, int err);
typedef struct {
    swap_init_cb_t callback;
    void *token;
} swap_init_cont_t;

static void
swap_init_end(void *token, int err, struct vnode *vn) {
    if (token == NULL) {
        printf("Error in swap_init_end: data corrupted. This shouldn't happen, check it.\n");
        return;
    }

    swap_init_cont_t *cont = (swap_init_cont_t*)token;

    if (err) {
        cont->callback(cont->token, err);
        free(cont);
        return;
    }

    err = nfs_dev_get_fhandle(vn, &swap_fh);
    if (err) {
        printf("Failed to get the swap file's fhandle, will fail when swapping is needed\n");
        cont->callback(cont->token, EFAULT);
        free(cont);
        return;
    }

    cont->callback(cont->token, 0);
    free(cont);
}

static void
swap_init(swap_init_cb_t callback, void *token){
    bzero(free_slots, sizeof(free_slots));

    swap_init_cont_t *cont = malloc(sizeof(swap_init_cont_t));
    if (cont == NULL) {
        callback(token, ENOMEM);
        return;
    }
    cont->callback = callback;
    cont->token    = token;

    vfs_open(SWAP_FILE_NAME, O_RDWR, swap_init_end, (void*)cont);
}

typedef struct {
    swap_in_cb_t callback;
    void* token;
    seL4_Word kvaddr;
    seL4_Word vpage;
    int swap_slot;
    addrspace_t *as;
    seL4_CapRights rights;
    size_t bytes_read;
} swap_in_cont_t;

static void
swap_in_end(void* token, int err){
    printf("swap in end entered\n");
    swap_in_cont_t *state = (swap_in_cont_t*)token;

    if(err){
        state->callback((void*)state->token, err);
        free(state);
        return;
    }

    _unset_slot(state->swap_slot);

    //TODO set frame lock free

    /* Map the page into user addrspace */

    seL4_CPtr kframe_cap, frame_cap;

    err = frame_get_cap(state->kvaddr, &kframe_cap);
    assert(!err); // This kvaddr is ready to use, there should be no error

    /* Copy the frame cap as we need to map it into 2 address spaces */
    frame_cap = cspace_copy_cap(cur_cspace, cur_cspace, kframe_cap, state->rights);
    if (frame_cap == CSPACE_NULL) {
        printf("swap_in_end: failed copying frame cap\n");
        state->callback((void*)state->token, EFAULT);
        free(state);
        return;
    }

    err = seL4_ARM_Page_Map(frame_cap, state->as->as_sel4_pd, PAGE_ALIGN(state->vpage),
                            state->rights, seL4_ARM_Default_VMAttributes);
    if(err == seL4_FailedLookup){
        /* Assume the error was because we have no page table
         * And at this point, page table should already be mapped in.
         * So this is an error */
        printf("swap_in_end: failed mapping application frame to sel4\n");
        state->callback((void*)state->token, EFAULT);
        free(state);
        return;
    }

    int x = PT_L1_INDEX(state->vpage);
    int y = PT_L2_INDEX(state->vpage);

    // Update the pagetable to reflects page is swapped back in
    state->as->as_pd_regs[x][y] = (state->kvaddr | PTE_IN_USE_BIT) & (~PTE_SWAPPED);
    state->as->as_pd_caps[x][y] = frame_cap;

    /* Not observable to I-cache yet so flush the frame */
    //TODO: only flush on pages in text segment
    seL4_ARM_Page_Unify_Instruction(kframe_cap, 0, PAGESIZE);

    printf("swap_in_end: calling back up\n");
    //we call our continuation on the second part of vmfault that will unblock the process looking to read a page
    state->callback((void*)state->token, 0);
    free(state);
}

void swap_in_nfs_read_handler(uintptr_t token, enum nfs_stat status,
                                fattr_t *fattr, int count, void* data){
    printf("swap in handler entered\n");
    if(status != NFS_OK){
        swap_in_end((void*)token, EFAULT);
        return;
    }

    swap_in_cont_t *state = (swap_in_cont_t*)token;

    /* Copy page in */
    memcpy((void*)(state->kvaddr) + state->bytes_read, data, count);

    state->bytes_read += count;
    if(state->bytes_read < PAGE_SIZE){
        printf("bytes read = %u, swap_slot = %d\n", state->bytes_read, state->swap_slot);
        enum rpc_stat status = nfs_read(swap_fh, state->swap_slot * PAGE_SIZE + state->bytes_read,
                                        MIN(NFS_SEND_SIZE, PAGE_SIZE - state->bytes_read),
                                        swap_in_nfs_read_handler, (uintptr_t)state);
        if (status != RPC_OK) {
            swap_in_end((void*)token, EFAULT);
            return;
        }
    } else {
        //sos_page_map(state->as, state->vpage, state->rights, swap_in_end, token, false);
        swap_in_end((void*)token, 0);
        return;
    }
}

int swap_in(addrspace_t *as, seL4_CapRights rights, seL4_Word vaddr, seL4_Word kvaddr, swap_in_cb_t callback, void* token){
    printf("swap in entered\n");
    printf("swap in this kvaddr -> 0x%08x, this vaddr -> 0x%08x\n", kvaddr, vaddr);

    // If swap file is not created, how can we swap in? This is a bug
    assert(swap_fh != NULL);

    kvaddr = PAGE_ALIGN(kvaddr);
    seL4_Word vpage = PAGE_ALIGN(vaddr);
    int x = PT_L1_INDEX(vpage);
    int y = PT_L2_INDEX(vpage);

    // only mapped/inuse application memory are swapped out, so these need not be NULL
    assert(as != NULL && as->as_pd_regs[x] != NULL && as->as_pd_caps[x] != NULL);

    assert(as->as_pd_regs[x][y] & PTE_SWAPPED);  // only swap in pages that are swapped out

    int swap_slot = (as->as_pd_regs[x][y] & PTE_SWAP_MASK)>>PTE_SWAP_OFFSET;

    /* Set our continuations */
    swap_in_cont_t *swap_cont = malloc(sizeof(swap_in_cont_t));
    if(swap_cont == NULL){
        return ENOMEM;
    }
    swap_cont->callback   = callback;
    swap_cont->token      = token;
    swap_cont->kvaddr     = kvaddr;
    swap_cont->vpage      = vpage;
    swap_cont->swap_slot  = swap_slot;
    swap_cont->as         = as;
    swap_cont->rights     = rights;
    swap_cont->bytes_read = 0;

    //TODO lock frame
    /*TODO free the slot*/

    /* Lock the slot */

    printf("swap in start reading\n");

    enum rpc_stat status = nfs_read(swap_fh, swap_slot * PAGE_SIZE, MIN(PAGE_SIZE, NFS_SEND_SIZE),
            swap_in_nfs_read_handler, (uintptr_t)swap_cont);
    if(status != RPC_OK){
        //printf("swap_in rpc error = %d\n", status);
        free(swap_cont);
        return EFAULT;
    }
    return 0;
}


typedef void (*swap_out_cb_t)(void *token, int err);
typedef struct {
    swap_out_cb_t callback;
    void *token;
    seL4_Word kvaddr;
    int free_slot;
    size_t written;
} swap_out_cont_t;

static void
swap_out_4_nfs_write_cb(uintptr_t token, enum nfs_stat status, fattr_t *fattr, int count) {
    printf("swap out 4 entered\n");
    swap_out_cont_t *cont = (swap_out_cont_t*)token;
    if (cont == NULL) {
        printf("NFS or swap.c is broken!!!\n");
        return;
    }

    if (status != NFS_OK || fattr == NULL || count < 0) {
        //TODO unlock frame
        cont->callback(cont->token, EFAULT);
        free(cont);
        return;
    }
    cont->written += (size_t)count;
    if (cont->written < PAGE_SIZE) {
        printf("swap out 4 reading more\n");
        enum rpc_stat status = nfs_write(swap_fh, cont->free_slot * PAGE_SIZE + cont->written,
                MIN(NFS_SEND_SIZE, PAGE_SIZE - cont->written),
                (void*)(cont->kvaddr + cont->written), swap_out_4_nfs_write_cb, (uintptr_t)cont);
        if (status != RPC_OK) {
            //TODO unlock frame
            cont->callback(cont->token, EFAULT);
            free(cont);
            return;
        }
        return;
    }
    printf("swap out 4 calling back up\n");

    //TODO unlock frame
    /* Use frame_free to mark the frame as free */
    int err = frame_free(cont->kvaddr);
    if(err){
        printf("frame free error in swap out\n");
    }


    //set swap out bit true
    addrspace_t *as = frame_get_as(cont->kvaddr);
    assert(as != NULL);

    seL4_Word vaddr = frame_get_vaddr(cont->kvaddr);
    seL4_Word vpage = PAGE_ALIGN(vaddr);
    int x = PT_L1_INDEX(vpage);
    int y = PT_L2_INDEX(vpage);

    //update with free slot
    as->as_pd_regs[x][y] = (cont->free_slot)<<PTE_SWAP_OFFSET | PTE_IN_USE_BIT | PTE_SWAPPED;

    //unmap the page, so that vmf will occurr
    err = sos_page_unmap(as, vaddr);
    if(err){
        printf("swapout4, sos_page_unmap error\n");
    }

    err = sos_page_unmap(as, vaddr);
    if (err) {
        printf("warning: swap_out_4: sos_page_unmap failed\n");
    }

    cont->callback(cont->token, 0);

    free(cont);
}

static void
swap_out_3(swap_out_cont_t *cont) {
    printf("swap out 3 entered\n");
    assert(cont != NULL); //Kernel code is buggy if this happens

    int free_slot = swap_find_free_slot();
    printf("swapout free slot = %d\n", free_slot);
    if(free_slot < 0){
        //TODO unlock frame
        cont->callback(cont->token, EFAULT);
        free(cont);
        return;
    }

    _set_slot(free_slot);
    printf("free slot = %d, bits = 0x%08x\n", free_slot, free_slots[0]);
    cont->free_slot = free_slot;

    //TODO update the as->as_pd[x][y] so that it reflects free slot,
    //as->as_pd[x][y] = (free_slot<<PTE_SWAP_OFFSET) | (as_pd[x][y] & 3);

    //TODO: lock down the frame before writing
    enum rpc_stat status = nfs_write(swap_fh, cont->free_slot * PAGE_SIZE, NFS_SEND_SIZE,
                        (void*)cont->kvaddr, swap_out_4_nfs_write_cb, (uintptr_t)cont);
    if (status != RPC_OK) {
        //TODO unlock frame
        printf("swapout 3 err\n");
        cont->callback(cont->token, EFAULT);
        free(cont);
        return;
    }
}

static void
swap_out_2_init_callback(void *token, int err) {
    printf("swap out 2 init entered\n");
    assert(token != NULL); // Should not happen

    swap_out_cont_t *cont = (swap_out_cont_t*)token;

    if (err) {
        //TODO unlock frame
        cont->callback(cont->token, err);
        free(cont);
        return;
    }

    swap_out_3(cont);
}

void
swap_out(seL4_Word kvaddr, swap_out_cb_t callback, void *token) {
    printf("swap_out entered, kvaddr = 0x%08x\n", kvaddr);

    seL4_Word vaddr = frame_get_vaddr(kvaddr);
    printf("swap out this kvaddr -> 0x%08x, this vaddr -> 0x%08x\n",kvaddr,vaddr);
    //TODO lock frame

    /* Create the continuation here to be used in subsequent functions */
    swap_out_cont_t *cont = malloc(sizeof(swap_out_cont_t));
    if (cont == NULL) {
        callback(token, ENOMEM);
        return;
    }
    cont->callback  = callback;
    cont->token     = token;
    cont->kvaddr    = kvaddr;
    cont->written   = 0;

    /* Initialise the swap file if it hasn't been there */
    if (swap_fh == NULL) {
        swap_init(swap_out_2_init_callback, (void*)cont);
        return;
    }

    swap_out_3(cont);
}
