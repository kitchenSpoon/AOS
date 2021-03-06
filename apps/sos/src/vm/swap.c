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
#include "proc/proc.h"

#define verbose 0
#include <sys/debug.h>

/*
 * Bitmap use to track free slots in our swap file
 * 1 is used
 * 0 is free
 */
#define NUM_CHUNKS (1<<10)
#define NUM_BITS (32)

extern bool starting_first_process;
uint32_t free_slots[NUM_CHUNKS];

fhandle_t *swap_fh;

static void
_unset_slot(int slot){
    free_slots[slot/NUM_BITS] &= ~(1u<<(slot%NUM_BITS));
    return;
}

static void
_set_slot(int slot){
    free_slots[slot/NUM_BITS] |= 1u<<(slot%NUM_BITS);
    return;
}

static int
_swap_find_free_slot(void){
    for(uint32_t i = 0; i < NUM_CHUNKS; i++){
        if (free_slots[i] == (uint32_t)(-1)) continue;
        for(uint32_t j = 0; j < NUM_BITS; j++) {
            //dprintf(3, "i = %d, j = %d, free_slots[i] in decimal = %u\n", i, j, free_slots[i]);
            if(!(free_slots[i] & (1u<<j))){
                return i*NUM_BITS + j;
            }
        }
    }
    return -1;
}

void swap_free_slot(int slot){
    _unset_slot(slot);
}

/***********************************************************************
 * swap_init
 * This function creates the *swap* file and store its filehandler
 ***********************************************************************/

typedef void (*swap_init_cb_t)(void *token, int err);
typedef struct {
    swap_init_cb_t callback;
    void *token;
} swap_init_cont_t;

static void _swap_init_end(void *token, int err, struct vnode *vn);

static void
_swap_init(swap_init_cb_t callback, void *token){
    bzero(free_slots, sizeof(free_slots));

    swap_init_cont_t *cont = malloc(sizeof(swap_init_cont_t));
    if (cont == NULL) {
        callback(token, ENOMEM);
        return;
    }
    cont->callback = callback;
    cont->token    = token;

    vfs_open(SWAP_FILE_NAME, O_RDWR, _swap_init_end, (void*)cont);
}

static void
_swap_init_end(void *token, int err, struct vnode *vn) {
    if (token == NULL) {
        dprintf(3, "Error in _swap_init_end: data corrupted. This shouldn't happen, check it.\n");
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
        dprintf(3, "Failed to get the swap file's fhandle, will fail when swapping is needed\n");
        cont->callback(cont->token, EFAULT);
        free(cont);
        return;
    }

    cont->callback(cont->token, 0);
    free(cont);
}

/***********************************************************************
 * swap_in
 ***********************************************************************/

typedef struct {
    swap_in_cb_t callback;
    void* token;
    seL4_Word kvaddr;
    seL4_Word vpage;
    int swap_slot;
    addrspace_t *as;
    seL4_CapRights rights;
    bool is_code;
    size_t bytes_read;
    pid_t pid;
} swap_in_cont_t;

static void _swap_in_page_map_cb(void *token, int err);
static void _swap_in_nfs_read_handler(uintptr_t token, enum nfs_stat status,
                                      fattr_t *fattr, int count, void* data);
static void _swap_in_end(void* token, int err);
int
swap_in(addrspace_t *as, seL4_CapRights rights, seL4_Word vaddr,
        bool is_code, swap_in_cb_t callback, void* token){
    dprintf(3, "swap in entered, vaddr = 0x%08x\n", vaddr);

    // If swap file is not created, how can we swap in? This is a bug
    assert(swap_fh != NULL);

    seL4_Word vpage = PAGE_ALIGN(vaddr);
    int x = PT_L1_INDEX(vpage);
    int y = PT_L2_INDEX(vpage);

    // only mapped/inuse application memory are swapped out, so these need not be NULL
    assert(as != NULL && as->as_pd_regs[x] != NULL && as->as_pd_caps[x] != NULL);
    assert(as->as_pd_regs[x][y] & PTE_SWAPPED);  // only swap in pages that are swapped out

    int swap_slot = (as->as_pd_regs[x][y] & PTE_SWAP_MASK)>>PTE_SWAP_OFFSET;

    swap_in_cont_t *swap_cont = malloc(sizeof(swap_in_cont_t));
    if(swap_cont == NULL){
        return ENOMEM;
    }
    swap_cont->callback   = callback;
    swap_cont->token      = token;
    swap_cont->kvaddr     = 0;
    swap_cont->vpage      = vpage;
    swap_cont->swap_slot  = swap_slot;
    swap_cont->as         = as;
    swap_cont->rights     = rights;
    swap_cont->is_code    = is_code;
    swap_cont->bytes_read = 0;
    swap_cont->pid        = proc_get_id();

    dprintf(3, "swap_in: swap_slot = %d, pid = %d\n", swap_slot, swap_cont->pid);
    int err;
    err = sos_page_map(swap_cont->pid, as, vpage, rights, _swap_in_page_map_cb, (void*)swap_cont, false);
    if (err) {
        free(swap_cont);
        return err;
    }
    return 0;
}

static void
_swap_in_page_map_cb(void *token, int err) {
    swap_in_cont_t *cont = (swap_in_cont_t*)token;

    dprintf(3, "_swap_in_page_map_cb called\n");
    if (err) {
        _swap_in_end(cont, err);
        return;
    }

    int x = PT_L1_INDEX(cont->vpage);
    int y = PT_L2_INDEX(cont->vpage);
    cont->kvaddr = cont->as->as_pd_regs[x][y] & PTE_KVADDR_MASK;

    dprintf(3, "_swap_in_page_map_cb lockframe\n");
    err = frame_lock_frame(cont->kvaddr);
    if (err) {
        dprintf(3, "swap_in failed to lock frame\n");
        _swap_in_end(cont, err);
        return;
    }

    dprintf(3, "swap in start reading\n");
    enum rpc_stat status = nfs_read(swap_fh, cont->swap_slot * PAGE_SIZE,
            MIN(PAGE_SIZE, NFS_SEND_SIZE), _swap_in_nfs_read_handler,
            (uintptr_t)cont);
    if(status != RPC_OK){
        _swap_in_end(cont, EFAULT);
    } else {
        //cont->pid = proc_get_id(); //we don't need to set it here
    }
    return;
}

static void
_swap_in_nfs_read_handler(uintptr_t token, enum nfs_stat status,
                          fattr_t *fattr, int count, void* data){
    dprintf(3, "swap in handler entered\n");
    swap_in_cont_t *cont = (swap_in_cont_t*)token;

    if(status != NFS_OK || count < 0){
        _swap_in_end((void*)token, EFAULT);
        return;
    }
    if (!starting_first_process && !is_proc_alive(frame_get_pid(cont->kvaddr))) {
        dprintf(3, "_swap_in_nfs_read_handler: process is killed\n");
        frame_unlock_frame(cont->kvaddr);
        //sos_page_free(cont->as, cont->vpage);
        frame_free(cont->kvaddr);
        _unset_slot(cont->swap_slot);
        cont->callback((void*)cont->token, EFAULT);
        free(cont);
        return;
    }
    set_cur_proc(cont->pid);


    /* Copy data in */
    memcpy((void*)(cont->kvaddr) + cont->bytes_read, data, count);

    cont->bytes_read += (size_t)count;

    //dprintf(3, "bytes read = %u, swap_slot = %d\n", cont->bytes_read, cont->swap_slot);
    /* Check if we need to read more */
    if(cont->bytes_read < PAGE_SIZE){
        enum rpc_stat status = nfs_read(swap_fh, cont->swap_slot * PAGE_SIZE + cont->bytes_read,
                                        MIN(NFS_SEND_SIZE, PAGE_SIZE - cont->bytes_read),
                                        _swap_in_nfs_read_handler, (uintptr_t)cont);
        if (status != RPC_OK) {
            _swap_in_end((void*)token, EFAULT);
        } else {
            //cont->pid = proc_get_id(); // we don't need to do this
        }
        return;
    } else {
        _swap_in_end((void*)token, 0);
    }
}

static void
_swap_in_end(void* token, int err){
    dprintf(3, "swap in end entered\n");
    swap_in_cont_t *cont = (swap_in_cont_t*)token;
    dprintf(3, "swap_slot = %d, pid = %d\n", cont->swap_slot, cont->pid);

    if(err){
        frame_unlock_frame(cont->kvaddr);
        if (cont->kvaddr) {
            sos_page_free(cont->as, cont->vpage);
        }
        cont->callback((void*)cont->token, err);
        free(cont);
        return;
    }

    /* Flush I-cache if we just swapped in an instruction page */
    seL4_CPtr kframe_cap;
    err = frame_get_cap(cont->kvaddr, &kframe_cap);
    assert(!err); // frame is locked, there should be no error

    if (cont->is_code) {
        seL4_ARM_Page_Unify_Instruction(kframe_cap, 0, PAGESIZE);
    }

    frame_unlock_frame(cont->kvaddr);
    _unset_slot(cont->swap_slot);

    /* We don't need to reset PTE as sos_page_map already done that for us */

    dprintf(3, "_swap_in_end: calling back up\n");
    cont->callback((void*)cont->token, 0);
    free(cont);
}

/***********************************************************************
 * swap_out
 ***********************************************************************/

typedef struct {
    swap_out_cb_t callback;
    void *token;
    seL4_Word kvaddr;
    int free_slot;
    size_t written;
    pid_t pid;
} swap_out_cont_t;

static void _swap_out_2_init_callback(void *token, int err);
static void _swap_out_3(swap_out_cont_t *cont);
static void _swap_out_4_nfs_write_cb(uintptr_t token, enum nfs_stat status,
                                     fattr_t *fattr, int count);
static void _swap_out_end(swap_out_cont_t *cont, int err);

void
swap_out(seL4_Word kvaddr, swap_out_cb_t callback, void *token) {
    dprintf(3, "swap_out entered, kvaddr = 0x%08x\n", kvaddr);
    kvaddr = PAGE_ALIGN(kvaddr);

    seL4_Word vaddr = frame_get_vaddr(kvaddr);
    assert(vaddr != 0);
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
    cont->pid       = proc_get_id();

    dprintf(3, "swap out this kvaddr -> 0x%08x, this vaddr -> 0x%08x, pid = %d\n",kvaddr,vaddr, cont->pid);

    frame_lock_frame(kvaddr);

    /* Initialise the swap file if it hasn't been there */
    if (swap_fh == NULL) {
        _swap_init(_swap_out_2_init_callback, (void*)cont);
        return;
    }

    _swap_out_3(cont);
}

static void
_swap_out_2_init_callback(void *token, int err) {
    dprintf(3, "swap out 2 init entered\n");
    assert(token != NULL); // Should not happen

    swap_out_cont_t *cont = (swap_out_cont_t*)token;
    if (err) {
        _swap_out_end(cont, EFAULT);
        return;
    }

    _swap_out_3(cont);
}

static void
_swap_out_3(swap_out_cont_t *cont) {
    dprintf(3, "swap out 3 entered\n");
    assert(cont != NULL); //Kernel code is buggy if this happens

    int free_slot = _swap_find_free_slot();
    dprintf(3, "swap_out free slot = %d, pid = %d, kvaddr = 0x%08x \n", free_slot, cont->pid, cont->kvaddr);
    if(free_slot < 0){
        _swap_out_end(cont, EFAULT);
        return;
    }
    _set_slot(free_slot);
    cont->free_slot = free_slot;

    enum rpc_stat status = nfs_write(swap_fh, cont->free_slot * PAGE_SIZE,
                        MIN(NFS_SEND_SIZE, PAGE_SIZE), (void*)cont->kvaddr,
                        _swap_out_4_nfs_write_cb, (uintptr_t)cont);
    if (status != RPC_OK) {
        dprintf(3, "swapout 3 err\n");
        _swap_out_end(cont, EFAULT);
        return;
    }
}

static void
_swap_out_4_nfs_write_cb(uintptr_t token, enum nfs_stat status, fattr_t *fattr, int count) {
    dprintf(3, "swap out 4 entered\n");
    swap_out_cont_t *cont = (swap_out_cont_t*)token;
    assert(cont != NULL);

    if (status != NFS_OK || fattr == NULL || count < 0) {
        _swap_out_end(cont, EFAULT);
        return;
    }

    if (!starting_first_process && !is_proc_alive(frame_get_pid(cont->kvaddr))) {
        dprintf(3, "_swap_out_4_nfs_write_cb: process is killed\n");
        _unset_slot(cont->free_slot);
        frame_unlock_frame(cont->kvaddr);
        frame_free(cont->kvaddr);
        cont->callback(cont->token, EFAULT);
        free(cont);
        return;
    }
    set_cur_proc(cont->pid);

    cont->written += (size_t)count;
    //dprintf(3, "swap out 4 written = %u, free_slot = %d\n", cont->written, cont->free_slot);
    /* Check if we have written the whole page */
    if (cont->written < PAGE_SIZE) {
        enum rpc_stat status = nfs_write(swap_fh, cont->free_slot * PAGE_SIZE + cont->written,
                MIN(NFS_SEND_SIZE, PAGE_SIZE - cont->written),
                (void*)(cont->kvaddr + cont->written), _swap_out_4_nfs_write_cb, (uintptr_t)cont);
        if (status != RPC_OK) {
            _swap_out_end(cont, EFAULT);
            return;
        }
    } else {
        _swap_out_end(cont, 0);
    }
}

static void
_swap_out_end(swap_out_cont_t *cont, int err) {
    dprintf(3, "swap out end: free_slot = %d, pid = %d\n", cont->free_slot, cont->pid);
    if (err) {
        dprintf(3, "_swap_out_end err\n");
        _unset_slot(cont->free_slot);
        frame_unlock_frame(cont->kvaddr);
        cont->callback(cont->token, EFAULT);
        free(cont);
        return;
    }

    /* Update the page table entries */
    addrspace_t *as = frame_get_as(cont->kvaddr);
    assert(as != NULL);

    seL4_Word vpage = PAGE_ALIGN(frame_get_vaddr(cont->kvaddr));
    assert(vpage != 0);
    int x = PT_L1_INDEX(vpage);
    int y = PT_L2_INDEX(vpage);

    as->as_pd_regs[x][y] = ((cont->free_slot)<<PTE_SWAP_OFFSET) | PTE_IN_USE_BIT | PTE_SWAPPED;

    /* Update frametable data */
    frame_unlock_frame(cont->kvaddr);

    seL4_CPtr kframe_cap;
    err = frame_get_cap(cont->kvaddr, &kframe_cap);
    seL4_ARM_Page_Unify_Instruction(kframe_cap, 0, PAGESIZE);

    err = frame_free(cont->kvaddr);
    if(err){
        dprintf(3, "frame free error in swap out\n");
    }

    cont->callback(cont->token, 0);
    free(cont);
}
