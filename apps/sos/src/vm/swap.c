#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <nfs/nfs.h>

#include "vfs/vfs.h"
#include "vm/swap.h"
#include "dev/nfs_dev.h"

#define NUM_BITS (32)
#define SWAP_FILE_NAME  "swap"

fhandle_t *swap_fh;

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

int swap_in(addrspace_t as, seL4_Word vaddr, seL4_Word free_kvaddr){
    //use as's mapping between addr and offset
    //int offset = as->mapping[addr];
    //nfs_read();
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
