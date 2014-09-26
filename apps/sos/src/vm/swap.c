#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <strings.h>
#include <nfs/nfs.h>
#include <limits.h>
#include <sel4/sel4.h>

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

int swap_init(){
    bzero(free_slots, sizeof(free_slots));
    return 0;
}

int swap_find_free_slots(){
    //loops 32 times
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

/*
int swap_out(){
    int slot = swap_find_free_slots();
    if(slot == -1){
        return -1;
    }

}
*/
