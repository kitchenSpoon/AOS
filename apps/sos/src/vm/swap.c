#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <strings.h>
#include <nfs/nfs.h>
#include <limits.h>
#include <sel4/sel4.h>
#include <strings.h>
#include <nfs/nfs.h>

#include "vfs/vfs.h"
#include "vm/swap.h"
#include "dev/nfs_dev.h"
#include "vm/addrspace.h"

#define NUM_BITS (32)
#define SWAP_FILE_NAME  "swap"

fhandle_t *swap_fh;

void swap_free_slot(seL4_Word offset){
    int slot = offset/(NUM_FREE_SLOTS * NUM_BITS);
    free_slots[slot/NUM_FREE_SLOTS] &= 0<<(slot%NUM_FREE_SLOTS); 
    return;
}

void swap_lock_slot(seL4_Word offset){
    int slot = offset/(NUM_FREE_SLOTS * NUM_BITS);
    free_slots[slot/NUM_FREE_SLOTS] &= 1<<(slot%NUM_FREE_SLOTS); 
    return;
}

int swap_check_valid_offset(seL4_Word offset){
    return 0;
}

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

void swap_init(swap_init_cb_t callback, void *token){
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

int swap_find_free_slot(void){
    for(uint32_t i = 0; i < NUM_FREE_SLOTS; i++){
        for(uint32_t j = 0; j < NUM_BITS; j++){
            if(!(free_slots[i] & (1<<j))){
                return i*NUM_FREE_SLOTS + j;
            }
        }
    }

    return -1;
}

typedef struct {
    swap_in_cb_t callback;
    void* token;
    seL4_Word kvaddr;
    seL4_Word vaddr;
    addrspace_t *as;
    seL4_CapRights rights;
} swap_in_cont_t;

void swap_in_handler(uintptr_t token, enum nfs_stat status,
                                fattr_t *fattr, int count, void* data){
    int err = 0;
    swap_in_cont_t *state = (swap_in_cont_t*)token;

    if(status != NFS_OK){
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

    //write the content into application's page through sos
    //char* src = (char*)data;
    //char* des = state->kvaddr;
    //*des = *src;
    memcpy((void*)(state->kvaddr), data, PAGE_SIZE);

    //set frame lock free

    //we call our continuation on the second part of vmfault that will unblock the process looking to read a page
    state->callback((uintptr_t)state->token, err);

    //set that slot in bitmap as free
    swap_free_slot(state->vaddr & PTE_SWAP_OFFSET);

    free(state);
}

int swap_in(addrspace_t *as, seL4_CapRights rights, seL4_Word vaddr, seL4_Word kvaddr, swap_in_cb_t callback, void* token){

    int err = 0;
    //check if swap file handler is initalized
    if(swap_fh == NULL){
        return err;
    }

    int offset = vaddr & PTE_SWAP_OFFSET;
    err = swap_check_valid_offset(offset);
    if(err){
        return err;
    }

    /* Set our continuations */
    swap_in_cont_t *swap_cont = malloc(sizeof(swap_in_cont_t));
    swap_cont->callback = callback;
    swap_cont->token    = token;
    swap_cont->kvaddr   = kvaddr;
    swap_cont->vaddr    = vaddr;
    swap_cont->as       = as;
    swap_cont->rights   = rights;

    enum rpc_stat status = nfs_read(swap_fh, offset, PAGE_SIZE, swap_in_handler, (uintptr_t)swap_cont);
    if(status != RPC_OK){
        err = 1;
        //printf("swap_in rpc error = %d\n", status);
    }
    return err;
}


int swap_out(seL4_Word kvaddr){
    int free_slot = swap_find_free_slot();
    if(free_slot == -1){
        return -1;
    }

//    nfs_write(
//typedef void (*nfs_write_cb_t)(uintptr_t token, enum nfs_stat status, 
//                               fattr_t *fattr, int count);
//enum rpc_stat nfs_write(const fhandle_t *fh, int offset, int count, 
//                        const void *data,
//                        nfs_write_cb_t callback, uintptr_t token);
//    nfs_write();
    return 0;
}

