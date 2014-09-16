#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <string.h>

#include <nfs/nfs.h>
#include <sel4/sel4.h>
#include <cspace/cspace.h>
#include <fcntl.h>
#include <unistd.h>

#include "dev/nfs_dev.h"

#include "tool/utility.h"
#include "vfs/vnode.h"
#include "vm/copyinout.h"
#include "dev/clock.h"
#include "syscall/syscall.h"

#define MAX_IO_BUF 0x1000
#define MAX_SERIAL_SEND 100

extern fhandle_t mnt_point;


static int nfs_dev_eachopen(struct vnode *file, int flags);
static int nfs_dev_eachclose(struct vnode *file, uint32_t flags);
static int nfs_dev_lastclose(struct vnode *file);
static int nfs_dev_read(struct vnode *file, char* buf, size_t nbytes, seL4_CPtr reply_cap);
static int nfs_dev_write(struct vnode *file, const char* buf, size_t nbytes, size_t *len);
static void nfs_dev_getdirent(struct vnode *dir, char *buf, size_t nbyte,
                      int pos, serv_sys_getdirent_cb_t callback, void *token);

typedef struct nfs_open_state{
    struct vnode *file;
    vfs_open_cb_t callback;
    void *vfs_open_token;
    sattr_t sattr;
} nfs_open_state_t;

struct nfs_data{
    //TODO: change these to non-pointer types
    fhandle_t *fh;
    fattr_t   *fattr;
};

typedef struct nfs_write_state{
    seL4_CPtr reply_cap;
    struct openfile *openfile;
} nfs_write_state;

typedef struct nfs_read_state{
    seL4_CPtr reply_cap;
    char* app_buf;
    struct openfile *openfile;
} nfs_read_state;

typedef struct nfs_getdirent_state{
    int pos;
    size_t nbyte;
    char* app_buf;
    nfscookie_t cookie;
    serv_sys_getdirent_cb_t callback;
    void* token;
} nfs_getdirent_state;

/*
 * Common function between nfs_dev_init & nfs_dev_init_mntpoint
 */
static int
init_helper(struct vnode *vn, fhandle_t *fh, fattr_t *fattr) {
    struct nfs_data *data = malloc(sizeof(struct nfs_data));
    if (data == NULL) {
        return ENOMEM;
    }
    data->fh = fh;
    data->fattr = fattr;

    vn->vn_data = data;

    vn->vn_ops->vop_eachopen  = nfs_dev_eachopen;
    vn->vn_ops->vop_eachclose = nfs_dev_eachclose;
    vn->vn_ops->vop_lastclose = nfs_dev_lastclose;
    vn->vn_ops->vop_read      = nfs_dev_read;
    vn->vn_ops->vop_write     = nfs_dev_write;
    vn->vn_ops->vop_getdirent = nfs_dev_getdirent;

    vn->vn_opencount  = 0;
    vn->initialised = true;
    return 0;
}

static void
nfs_dev_create_handler(uintptr_t token, enum nfs_stat status, fhandle_t *fh, fattr_t *fattr){
    nfs_open_state_t *state = (nfs_open_state_t*)token;
    nfs_open_state_t local_state = *state;
    free(state);
    if(status == NFS_OK){
        int err = 0;

        /* Copy data to our vnode*/
        fhandle_t *our_fh = malloc(sizeof(fhandle_t));
        if(our_fh == NULL){
            local_state.callback(local_state.vfs_open_token, ENOMEM);
            return;
        }
        memcpy(our_fh->data, fh->data, sizeof(fh->data));

        fattr_t *our_fattr = malloc(sizeof(fattr_t));
        if(our_fattr == NULL){
            free(our_fh);
            local_state.callback(local_state.vfs_open_token, ENOMEM);
            return;
        }
        *our_fattr = *fattr;

        /* place fhandle_t into vnode and add vnode into mapping*/
        err = init_helper(state->file, our_fh, our_fattr);
        if (err) {
            free(our_fh);
            free(our_fattr);
            local_state.callback(local_state.vfs_open_token, err);
            return;
        }

        local_state.callback(local_state.vfs_open_token, 0);
        return;
    } else {
        //error, nfs fail to create file
        local_state.callback(local_state.vfs_open_token, EFAULT);
        return;
    }
}

static void
nfs_dev_create(nfs_open_state_t *state){
    state->sattr.mode  = S_IRUSR | S_IWUSR;
    state->sattr.uid   = 0;
    state->sattr.gid   = 0;
    state->sattr.size  = 0;
    //TODO: fix this
    state->sattr.atime.seconds = 0;
    state->sattr.atime.useconds = 0;
    state->sattr.mtime.seconds = 0;
    state->sattr.mtime.useconds = 0;
    nfs_create(&mnt_point, state->file->vn_name, &state->sattr, nfs_dev_create_handler, (uintptr_t)state);
}

static void
nfs_dev_lookup_handler(uintptr_t token, enum nfs_stat status, fhandle_t *fh, fattr_t *fattr){
    int err;

    nfs_open_state_t *state = (nfs_open_state_t*)token;

    if(status == NFS_OK){
        nfs_open_state_t local_state = *state;
        free(state);

        err = init_helper(local_state.file, fh, fattr);
        local_state.callback(local_state.vfs_open_token, err);
        return;
    }

    nfs_dev_create(state);
}

void
nfs_dev_init(struct vnode* vn, vfs_open_cb_t callback, void *vfs_open_token) {
    nfs_open_state_t *state = malloc(sizeof(nfs_open_state_t));
    if (state == NULL) {
        callback(vfs_open_token, ENOMEM);
        return;
    }

    state->file            = vn;
    state->callback        = callback;
    state->vfs_open_token  = vfs_open_token;

    nfs_lookup(&mnt_point, vn->vn_name, nfs_dev_lookup_handler, (uintptr_t)state);
}

int
nfs_dev_init_mntpoint(struct vnode* vn, fhandle_t *mnt_point) {
    int err;
    err = init_helper(vn, mnt_point, NULL);
    return err;
}

static int
nfs_dev_eachopen(struct vnode *file, int flags){
    (void)file;
    (void)flags;
    return 0;
}

static int nfs_dev_eachclose(struct vnode *file, uint32_t flags){
//    (void)file;
//    (void)flags;
    return 0;
}

static int nfs_dev_lastclose(struct vnode *vn) {
    struct nfs_data *data = (struct nfs_data*)vn->vn_data;

    free(data->fh);
    free(data->fattr);
    return 0;
}

static void nfs_dev_write_handler(uintptr_t token, enum nfs_stat status, fattr_t *fattr, int count){
//    int err = 0;
//    if(status == NFS_OK){
//        /* Cast for convience */
//        nfs_write_state *state = (nfs_write_state*) token;
//
//        // can we write more data than the file can hold?
//        /* Update openfile */
//        state->openfile->offset += count;
//
//    } else {
//        //error
//        err = 1;
//    }
//
//    /* reply sosh*/
//    seL4_CPtr reply_cap = state->reply_cap;
//    seL4_MessageInfo_t reply = seL4_MessageInfo_new(err, 0, 0, 1);
//    seL4_SetMR(0, (seL4_Word)count);
//    seL4_Send(reply_cap, reply);
//    cspace_free_slot(cur_cspace, reply_cap);
//
//    free(state);
}

//we do not need len anymore since it will be invalid when our callack finishes, please remove or not use it
static int nfs_dev_write(struct vnode *file, const char* buf, size_t nbytes, size_t *len) {

//    nfs_write_state state = malloc(sizeof(nfs_write_state));
//    state->reply = reply_cap;
//    state->openfile = openfile;
//    rpc_stat status = nfs_write(mnt_point, offset, nbytes, buf, nfs_dev_write_handler, state);
    return 0;
}

static void nfs_dev_read_handler(uintptr_t token, enum nfs_stat status, fattr_t *fattr, int count, void* data){
//    int err = 0;
//    if(status == NFS_OK){
//        /* Cast for convience */
//        nfs_read_state *state = (nfs_read_state*) token;
//
//        /* Needs to copy the data to sosh space */
//        int err = copyout(state->app_buf, data, count);
//
//        /* Update openfile */
//        if(!err){
//            openfile->offset += count;
//        }
//    } else {
//        //error
//        err = 1;
//    }
//
//
//    /* reply sosh*/
//    seL4_CPtr reply_cap = state->reply_cap;
//    seL4_MessageInfo_t reply = seL4_MessageInfo_new(err, 0, 0, 1);
//    seL4_SetMR(0, (seL4_Word)count);
//    seL4_Send(reply_cap, reply);
//    cspace_free_slot(cur_cspace, reply_cap);
//
//    free(state);
}
//static int nfs_dev_read(struct vnode *file, char* buf, size_t nbytes, int offset, seL4_CPtr reply_cap){
static int nfs_dev_read(struct vnode *file, char* buf, size_t nbytes, seL4_CPtr reply_cap){
//    nfs_read_state *state = malloc(sizeof(nfs_read_state));
//    state->reply_cap = reply_cap;
//    state->app_buf = buf;
//    state->openfile = openfile;
//    nfs_read(file->vn_data->fh, offset, nbytes, nfs_dev_read_handler, state);
//
    return 0;
}

static void nfs_dev_getdirent_handler(uintptr_t token, enum nfs_stat status, int num_files, char* file_names[], nfscookie_t nfscookie){
    int err = 0;
    size_t size = 0;
    bool finish = false;
    nfs_getdirent_state *state = (nfs_getdirent_state*) token;
    if(status == NFS_OK){
        state->cookie = nfscookie;

        /* We have the file we want */
        if(num_files >= state->pos){
            //pos is valid entry
            char* name = file_names[state->pos];
            while(name[size] != '\0' && size < state->nbyte-1){
                size++;
            }
            name[size++] = '\0';

            err = copyout((seL4_Word)state->app_buf, (seL4_Word)name, size);
            finish = true;
        } else {
            if(nfscookie == 0){  /* No more entry*/
                finish = true;
                //pos is next free entry
                if(state->pos == num_files + 1){
                    size = 0;
                } else {
                    err = EINVAL;
                }
            } else {             /* Read more */
                state->pos -= num_files;
                assert(state->pos >= 0);
                nfs_readdir(&mnt_point, nfscookie, nfs_dev_getdirent_handler, (uintptr_t)state);
            }
        }

    } else {
        finish = true;
        err = 1;
    }

    if(finish){
        state->callback(state->token, err, size);
        free(state);
    }
}

static void nfs_dev_getdirent(struct vnode *dir, char *buf, size_t nbyte, int pos,
                              serv_sys_getdirent_cb_t callback, void *token){
    nfs_getdirent_state *state = malloc(sizeof(nfs_getdirent_state));
    if (state == NULL) {
        callback(token, ENOMEM, 0);
        return;
    }
    state->app_buf   = buf;
    state->nbyte     = nbyte;
    state->pos       = pos;
    state->callback  = callback;
    state->token     = token;
    nfs_readdir(&mnt_point, 0, nfs_dev_getdirent_handler, (uintptr_t)state);
}

//static int nfs_dev_stat(struct vnode *file, sos_stat_t *buf){
//    if(file == NULL) return EFAULT;
//    if(file->vn == NULL) return EFAULT;
//    if(file->vn->vn_data == NULL) return EFAULT;
//
//    //TODO: update fattr in every call to nfs (lookup, read, write, create)
//    //need to check if fattr has pointers in it
//    fattr_t *fattr = (file->vn->vn_data->fattr);
//
//    /* Turn fattr to sos_stat_t */
//    sos_stat_t stat;
//    stat.st_type = ST_FILE;
//    stat.st_mode = fattr->mode;
//    stat.st_size = fattr->size;
//    stat.st_mtime.seconds = fattr->mtime.seconds;
//    stat.st_mtime.useconds = fattr->mtime.useconds;
//
//    /* Needs to copy the data to sosh space */
//    int err = copyout(buf, stat, sizeof(sos_stat_t));
//
//
//    /* reply sosh*/
//    seL4_CPtr reply_cap = state->reply_cap;
//    seL4_MessageInfo_t reply = seL4_MessageInfo_new(err, 0, 0, 0);
//    seL4_Send(reply_cap, reply);
//    cspace_free_slot(cur_cspace, reply_cap);
//
//    free(state);
//    return 0;
//}

static void nfs_dev_timeout_handler(uint32_t id, void *data){
    (void)id;
    (void)data;
    nfs_timeout();
    register_timer(100000, nfs_dev_timeout_handler, NULL); //100ms
}

void nfs_dev_setup_timeout(void){
    register_timer(100000, nfs_dev_timeout_handler, NULL); //100ms
}
