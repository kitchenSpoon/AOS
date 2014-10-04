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
swap_free_slot(seL4_Word offset){
    int slot = offset/(NUM_FREE_SLOTS * NUM_BITS);
    free_slots[slot/NUM_FREE_SLOTS] &= 0<<(slot%NUM_FREE_SLOTS);
    return;
}

static void
swap_lock_slot(seL4_Word offset){
    int slot = offset/(NUM_FREE_SLOTS * NUM_BITS);
    free_slots[slot/NUM_FREE_SLOTS] &= 1<<(slot%NUM_FREE_SLOTS);
    return;
}*/

static void
swap_free_slot(int slot){
    free_slots[slot/NUM_FREE_SLOTS] &= 0<<(slot%NUM_FREE_SLOTS);
    return;
}

static void
swap_lock_slot(int slot){
    free_slots[slot/NUM_FREE_SLOTS] &= 1<<(slot%NUM_FREE_SLOTS);
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

void swap_in_end(uintptr_t token, int err){
    swap_in_cont_t *state = (swap_in_cont_t*)token;

    if(err){
        state->callback((uintptr_t)state->token, 1);
        free(state);
        return;
    }


    //map application page
    //this automaticallys sets the page as not swapped out
    err = sos_swap_page_map(state->as, state->vaddr, state->kvaddr, state->rights);
    if(err){
        state->callback((uintptr_t)state->token, err);
        free(state);
        return;
    }

    //set frame lock free

    //we call our continuation on the second part of vmfault that will unblock the process looking to read a page
    state->callback((uintptr_t)state->token, err);

    //set that slot in bitmap as free
    int free_slot = state->vaddr & PTE_SWAP_OFFSET;;
    swap_free_slot(free_slot);

    free(state);
}
void swap_in_handler(uintptr_t token, enum nfs_stat status,
                                fattr_t *fattr, int count, void* data){
    int err = 0;
    swap_in_cont_t *state = (swap_in_cont_t*)token;
    if(status != NFS_OK){
        swap_in_end(token, EFAULT);
        return;
    }

    /* Copy page in */
    memcpy((void*)(state->kvaddr) + state->bytes_read, data, count);

    state->bytes_read += count;
    if(state->bytes_read < PAGE_SIZE){
        //enum rpc_stat status = nfs_read(swap_fh, offset + state->bytes_read, MIN(PAGE_SIZE - state->bytes_read, NFS_SEND_SIZE),
        //                     swap_in_handler, (uintptr_t)swap_cont);

        int free_slot = state->vaddr & PTE_SWAP_OFFSET;;

        enum rpc_stat status = nfs_read(swap_fh, free_slot * PAGE_SIZE + state->bytes_read, MIN(NFS_SEND_SIZE, PAGE_SIZE - state->bytes_read),
                               swap_in_handler, (uintptr_t)state);
        if (status != RPC_OK) {
            swap_in_end(token, EFAULT);
            return;
        }
    } else {
        swap_in_end(token, err);
    }
}
int swap_in(addrspace_t *as, seL4_CapRights rights, seL4_Word vaddr, seL4_Word kvaddr, swap_in_cb_t callback, void* token){

    int err = 0;
    //check if swap file handler is initalized
    if(swap_fh == NULL){
        return EFAULT;
    }

    int free_slot = vaddr & PTE_SWAP_OFFSET;;
    err = swap_check_valid_offset(free_slot);
    if(err){
        return err;
    }

    /* Set our continuations */
    swap_in_cont_t *swap_cont = malloc(sizeof(swap_in_cont_t));
    swap_cont->callback   = callback;
    swap_cont->token      = token;
    swap_cont->kvaddr     = kvaddr;
    swap_cont->vaddr      = vaddr;
    swap_cont->as         = as;
    swap_cont->rights     = rights;
    swap_cont->bytes_read = 0;

    /* Lock the slot */
    swap_lock_slot(free_slot);

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
    swap_out_cont_t *cont = (swap_out_cont_t*)token;
    if (cont == NULL) {
        printf("NFS or swap.c is broken!!!\n");
        return;
    }

    if (status != NFS_OK || fattr == NULL || count < 0) {
        cont->callback(cont->token, EFAULT);
        free(cont);
        return;
    }
    cont->written += (size_t)count;
    if (cont->written < PAGE_SIZE) {
        enum rpc_stat status = nfs_write(swap_fh, cont->free_slot * PAGE_SIZE + cont->written, MIN(NFS_SEND_SIZE, PAGE_SIZE - cont->written),
                            (void*)(cont->kvaddr + cont->written), swap_out_4_nfs_write_cb, (uintptr_t)cont);
        if (status != RPC_OK) {
            cont->callback(cont->token, EFAULT);
            free(cont);
            return;
        }
    }
    cont->callback(cont->token, 0);
    free(cont);
}

static void
swap_out_3(swap_out_cont_t *cont) {
    assert(cont != NULL); //Kernel code is buggy if this happens

    //TODO: lock down the frame before writing
    enum rpc_stat status = nfs_write(swap_fh, cont->free_slot * PAGE_SIZE, NFS_SEND_SIZE,
                        (void*)cont->kvaddr, swap_out_4_nfs_write_cb, (uintptr_t)cont);
    if (status != RPC_OK) {
        cont->callback(cont->token, EFAULT);
        free(cont);
        return;
    }
}

static void
swap_out_2_init_callback(void *token, int err) {
    assert(token != NULL); // Should not happen

    swap_out_cont_t *cont = (swap_out_cont_t*)token;

    if (err) {
        cont->callback(cont->token, err);
        free(cont);
        return;
    }

    swap_out_3(cont);
}

void
swap_out(seL4_Word kvaddr, swap_out_cb_t callback, void *token) {
    int free_slot = swap_find_free_slot();
    if(free_slot < 0){
        callback(token, EFAULT);
        return;
    }

    /* Create the continuation here to be used in subsequent functions */
    swap_out_cont_t *cont = malloc(sizeof(swap_out_cont_t));
    if (cont == NULL) {
        callback(token, ENOMEM);
        return;
    }
    cont->callback  = callback;
    cont->token     = token;
    cont->kvaddr    = kvaddr;
    cont->free_slot = free_slot;
    cont->written   = 0;

    /* Initialise the swap file if it hasn't been there */
    if (swap_fh == NULL) {
        swap_init(swap_out_2_init_callback, (void*)cont);
        return;
    }
    swap_out_3(cont);
}
