#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <limits.h>
#include <nfs/nfs.h>

#include "vfs/vfs.h"
#include "vm/swap.h"
#include "dev/nfs_dev.h"

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

typedef void (*swap_init_cb_t)(void *token, int err);
typedef struct {
    swap_init_cb_t callback;
    void *token;
} swap_init_cont_t;

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

int
swap_in(addrspace_t as, seL4_Word vaddr, seL4_Word free_kvaddr){
    //use as's mapping between addr and offset
    //int offset = as->mapping[addr];
    //nfs_read();
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
        enum rpc_stat status = nfs_write(swap_fh, cont->free_slot * PAGE_SIZE + cont->written, NFS_SEND_SIZE,
                            (void*)(cont->kvaddr + cont->written), swap_out_4_nfs_write_cb, (uintptr_t)cont);
        if (status != RPC_OK) {
            cont->callback(cont->token, EFAULT);
            free(cont);
            return;
        }
    }
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
