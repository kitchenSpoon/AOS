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
#define NUM_FREE_SLOTS (32)
#define NUM_BITS (32)
#define SWAP_FILE_NAME  "swap"

#define NFS_SEND_SIZE   1024 //This needs to be less than UDP package size

uint32_t free_slots[NUM_FREE_SLOTS];

fhandle_t *swap_fh;

/*static void
_unset_slot(seL4_Word offset){
    int slot = offset/(NUM_FREE_SLOTS * NUM_BITS);
    free_slots[slot/NUM_FREE_SLOTS] &= 0<<(slot%NUM_FREE_SLOTS);
    return;
}

static void
_set_slot(seL4_Word offset){
    int slot = offset/(NUM_FREE_SLOTS * NUM_BITS);
    free_slots[slot/NUM_FREE_SLOTS] &= 1<<(slot%NUM_FREE_SLOTS);
    return;
}*/

static void
_unset_slot(int slot){
    free_slots[slot/NUM_FREE_SLOTS] &= ~(1<<(slot%NUM_FREE_SLOTS));
    return;
}

static void
_set_slot(int slot){
    free_slots[slot/NUM_FREE_SLOTS] |= 1<<(slot%NUM_FREE_SLOTS);
    return;
}

static int
swap_check_valid_offset(seL4_Word offset){
    return 0;
}

static int
swap_find_free_slot(void){
    for(uint32_t i = 0; i < NUM_FREE_SLOTS; i++){
        for(uint32_t j = 0; j < NUM_BITS; j++){
            //printf("i = %d, j = %d, free_slots[i] in decimal = %u\n", i, j, free_slots[i]);
            if(!(free_slots[i] & (1<<j))){
                return i*NUM_FREE_SLOTS + j;
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
    seL4_Word vaddr;
    int free_slot;
    addrspace_t *as;
    seL4_CapRights rights;
    size_t bytes_read;
} swap_in_cont_t;

void swap_in_end(void* token, int err){
    printf("swap in end entered\n");
    swap_in_cont_t *state = (swap_in_cont_t*)token;

    if(err){
        state->callback((void*)state->token, err);
        free(state);
        return;
    }

    //TODO update the as->as_pd[x][y] so that it reflects correct vaddr,
    //as->as_pd[x][y] = ((state->kvaddr)<<2) | (as->as_pd[x][y] & 3);
    //UPDATE_PAGETABLE_REG();

    //TODO set frame lock free

    //we call our continuation on the second part of vmfault that will unblock the process looking to read a page
    state->callback((void*)state->token, err);

    //set that slot in bitmap as free
    int free_slot = state->vaddr & PTE_SWAP_OFFSET;;
    _unset_slot(free_slot);

    free(state);
}

void swap_in_page_map(void* token, int err){
    printf("swap in page map entered\n");
    swap_in_cont_t *state = (swap_in_cont_t*)token;

    if(err){
        state->callback((void *)state->token, 1);
        free(state);
        return;
    }

    //map application page
    //this automaticallys sets the page as not swapped out
    sos_page_map(state->as, state->vaddr, state->rights, swap_in_end, token);
}
void swap_in_handler(uintptr_t token, enum nfs_stat status,
                                fattr_t *fattr, int count, void* data){
    printf("swap in handler entered\n");
    int err = 0;
    swap_in_cont_t *state = (swap_in_cont_t*)token;
    if(status != NFS_OK){
        swap_in_end((void*)token, EFAULT);
        return;
    }

    /* Copy page in */
    memcpy((void*)(state->kvaddr) + state->bytes_read, data, count);

    state->bytes_read += count;
    if(state->bytes_read < PAGE_SIZE){
        //enum rpc_stat status = nfs_read(swap_fh, offset + state->bytes_read, MIN(PAGE_SIZE - state->bytes_read, NFS_SEND_SIZE),
        //                     swap_in_handler, (uintptr_t)swap_cont);
        int free_slot = (state->vaddr & PTE_SWAP_OFFSET)>>2;

        printf("bytes read = %u, free_slot = %d\n",state->bytes_read, free_slot);
        enum rpc_stat status = nfs_read(swap_fh, free_slot * PAGE_SIZE + state->bytes_read, MIN(NFS_SEND_SIZE, PAGE_SIZE - state->bytes_read),
                               swap_in_handler, (uintptr_t)state);
        if (status != RPC_OK) {
            swap_in_end((void*)token, EFAULT);
            return;
        }
    } else {
        swap_in_page_map((void*)token, err);
    }
}
int swap_in(addrspace_t *as, seL4_CapRights rights, seL4_Word vaddr, seL4_Word kvaddr, swap_in_cb_t callback, void* token){
    printf("swap in entered\n");
    int err = 0;
    //check if swap file handler is initalized
    if(swap_fh == NULL){
        return EFAULT;
    }

    //use vaddr to get as->as_pd[x][y]
    //and then use it to get the free slot
    //this is currently a hack
    int free_slot = (vaddr & PTE_SWAP_OFFSET)>>2;
    err = swap_check_valid_offset(free_slot);
    if(err){
        return err;
    }

    printf("swap in checked valid\n");

    /* Set our continuations */
    swap_in_cont_t *swap_cont = malloc(sizeof(swap_in_cont_t));
    if(swap_cont == NULL){
        return EFAULT;
    }
    swap_cont->callback   = callback;
    swap_cont->token      = token;
    swap_cont->kvaddr     = kvaddr;
    swap_cont->vaddr      = vaddr;
    swap_cont->as         = as;
    swap_cont->rights     = rights;
    swap_cont->bytes_read = 0;

    printf("swap in checked valid\n");

    //TODO lock frame
    /*TODO free the slot*/

    /* Lock the slot */
    //_set_slot(free_slot);

    enum rpc_stat status = nfs_read(swap_fh, free_slot * PAGE_SIZE , PAGE_SIZE, swap_in_handler, (uintptr_t)swap_cont);
    /* if status != RPC_OK this function will return error however the swap_in_handler might still run, bugg?*/
    if(status != RPC_OK){
        err = 1;
        //printf("swap_in rpc error = %d\n", status);
    }
    return err;
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
        enum rpc_stat status = nfs_write(swap_fh, cont->free_slot * PAGE_SIZE + cont->written, MIN(NFS_SEND_SIZE, PAGE_SIZE - cont->written),
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
    int err = frame_free(cont->kvaddr);
    if(err){
        printf("frame free error in swap out\n");
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
    printf("free slot = %d, bits = %d\n", free_slot, free_slots[0]);
    cont->free_slot = free_slot;

    //TODO update the as->as_pd[x][y] so that it reflects free slot,
    //as->as_pd[x][y] = (free_slot<<2) | (as_pd[x][y] & 3);

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
    printf("swap out entered\n");

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
