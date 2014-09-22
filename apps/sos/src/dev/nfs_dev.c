#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <string.h>

#include <nfs/nfs.h>
#include <sel4/sel4.h>
#include <cspace/cspace.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#include "dev/nfs_dev.h"

#include "tool/utility.h"
#include "vfs/vnode.h"
#include "vm/copyinout.h"
#include "dev/clock.h"

static int nfs_dev_eachopen(struct vnode *file, int flags);
static int nfs_dev_eachclose(struct vnode *file, uint32_t flags);
static int nfs_dev_lastclose(struct vnode *file);
static void nfs_dev_read(struct vnode *file, char* buf, size_t nbytes, size_t offset,
                              vop_read_cb_t callback, void *token);
static void nfs_dev_write(struct vnode *file, const char* buf, size_t nbytes, size_t offset,
                              vop_write_cb_t callback, void *token);
static void nfs_dev_getdirent(struct vnode *dir, char *buf, size_t nbyte,
                      int pos, vop_getdirent_cb_t callback, void *token);
static int nfs_dev_stat(struct vnode *file, sos_stat_t *buf);

#define MAX_IO_BUF 0x1000
#define MAX_SERIAL_SEND 100

extern fhandle_t mnt_point;

struct nfs_data{
    fhandle_t *fh;
    fattr_t   *fattr;
};

typedef struct nfs_open_state{
    struct vnode *file;
    nfs_dev_init_cb_t callback;
    void *token;
    sattr_t sattr;
} nfs_open_state_t;

typedef struct nfs_write_state{
    vop_write_cb_t callback;
    void* token;
    struct vnode *file;
} nfs_write_state;

typedef struct nfs_read_state{
    struct vnode *file;
    char* app_buf;
    vop_read_cb_t callback;
    void* token;
} nfs_read_state;

typedef struct nfs_stat_state{
    sos_stat_t* stat_buf;
    nfs_dev_stat_cb_t callback;
    void* token;
} nfs_stat_state_t;

typedef struct nfs_getdirent_state{
    int pos;
    size_t nbyte;
    char* app_buf;
    nfscookie_t cookie;
    vop_getdirent_cb_t callback;
    void* token;
} nfs_getdirent_state;

/*
 * Common function between nfs_dev_init & nfs_dev_init_mntpoint
 */
static int
init_helper(struct vnode *vn, fhandle_t *fh, fattr_t *fattr) {
    assert(vn != NULL);
    assert(vn->vn_ops != NULL);
    assert(fh != NULL);
    printf("init_helper called\n");
    struct nfs_data *data = malloc(sizeof(struct nfs_data));
    if (data == NULL) {
        return ENOMEM;
    }

    data->fh = malloc(sizeof(fhandle_t));
    if(data->fh == NULL){
        free(data);
        return ENOMEM;
    }
    memcpy(data->fh->data, fh->data, sizeof(fh->data));

    data->fattr = NULL;
    if (fattr != NULL) {
        data->fattr = malloc(sizeof(fattr_t));
        if(data->fattr == NULL){
            free(data->fh);
            free(data);
            return ENOMEM;
        }
        *(data->fattr) = *fattr;

        vn->sattr.st_type = ST_FILE;
        vn->sattr.st_mode = fattr->mode;
        vn->sattr.st_size = fattr->size;
        vn->sattr.st_mtime.seconds = fattr->mtime.seconds;
        vn->sattr.st_mtime.useconds = fattr->mtime.useconds;
    }

    vn->vn_data = data;

    vn->vn_ops->vop_eachopen  = nfs_dev_eachopen;
    vn->vn_ops->vop_eachclose = nfs_dev_eachclose;
    vn->vn_ops->vop_lastclose = nfs_dev_lastclose;
    vn->vn_ops->vop_read      = nfs_dev_read;
    vn->vn_ops->vop_write     = nfs_dev_write;
    vn->vn_ops->vop_getdirent = nfs_dev_getdirent;
    vn->vn_ops->vop_stat      = nfs_dev_stat;

    vn->vn_opencount  = 0;
    vn->initialised = true;
    return 0;
}

static void
nfs_dev_create_handler(uintptr_t token, enum nfs_stat status, fhandle_t *fh, fattr_t *fattr){
    printf("nfs_dev_create handler called\n");
    nfs_open_state_t *state = (nfs_open_state_t*)token;
    if(status == NFS_OK){
        int err = 0;

        /* place fhandle_t into vnode and add vnode into mapping*/
        err = init_helper(state->file, fh, fattr);
        if (err) {
            state->callback(state->token, err);
            free(state);
            return;
        }

        state->callback(state->token, 0);
        free(state);
        return;
    } else {
        //error, nfs failed to create file
        state->callback(state->token, EFAULT);
        free(state);
        return;
    }
}

static void
nfs_dev_create(nfs_open_state_t *state){
    state->sattr.mode  = S_IRUSR | S_IWUSR;
    state->sattr.uid   = 0;
    state->sattr.gid   = 0;
    state->sattr.size  = 0;
    // No easy way to get the time, time(NULL) is not implemented
    state->sattr.atime.seconds = 0;
    state->sattr.atime.useconds = 0;
    state->sattr.mtime = state->sattr.atime;
    nfs_create(&mnt_point, state->file->vn_name, &state->sattr, nfs_dev_create_handler, (uintptr_t)state);
}

static void
nfs_dev_lookup_handler(uintptr_t token, enum nfs_stat status, fhandle_t *fh, fattr_t *fattr){
    int err;

    nfs_open_state_t *state = (nfs_open_state_t*)token;

    if(status == NFS_OK){
        err = init_helper(state->file, fh, fattr);
        state->callback(state->token, err);
        free(state);
        return;
    }

    nfs_dev_create(state);
}

void
nfs_dev_init(struct vnode* vn, nfs_dev_init_cb_t callback, void *token) {
    printf("nfs_dev_init called\n");
    nfs_open_state_t *state = malloc(sizeof(nfs_open_state_t));
    if (state == NULL) {
        callback(token, ENOMEM);
        return;
    }

    state->file            = vn;
    state->callback        = callback;
    state->token           = token;

    enum rpc_stat status = nfs_lookup(&mnt_point, vn->vn_name, nfs_dev_lookup_handler, (uintptr_t)state);
    if (status != RPC_OK) {
        free(state);
        callback(token, EFAULT);
        return;
    }
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

    printf("nfs_close\n");
    return 0;
}

static int nfs_dev_lastclose(struct vnode *vn) {
    struct nfs_data *data = (struct nfs_data*)vn->vn_data;
    if (data->fh != NULL) {
        free(data->fh);
    }
    if (data->fattr != NULL) {
        free(data->fattr);
    }
    free(data);
    return 0;
}

static void nfs_dev_write_handler(uintptr_t token, enum nfs_stat status, fattr_t *fattr, int count){
    //printf("nfs_dev_write_handler count = %d\n", count);
    //printf("nfs_dev_write_hander status = %d\n", status);

    int err = 0;
    nfs_write_state *state = (nfs_write_state*)token;
    assert(state != NULL);
    struct nfs_data *data = (struct nfs_data*)state->file->vn_data;
    assert(data != NULL);
    *(data->fattr) = *fattr;

    if(status == NFS_OK){
        //TODO: wtf is this?
        // can we write more data than the file can hold?
        /* Update openfile */
        //state->openfile->offset += count;

    } else {
        //error
        err = 1;
    }

    state->callback(state->token, err, count);
    free(state);
}

static void nfs_dev_write(struct vnode *file, const char* buf, size_t nbytes, size_t offset,
                              vop_write_cb_t callback, void *token){
    //printf("nfs_dev_write buf = %s\n asdasdasdas \n", buf);
    //printf("nfs_dev_write offset = %d\n", offset);
    nfs_write_state *state = malloc(sizeof(nfs_write_state));
    if (state == NULL) {
        callback(token, ENOMEM, 0);
        return;
    }
    state->callback  = callback;
    state->token     = token;
    state->file      = file;
    struct nfs_data *data = (struct nfs_data *)file->vn_data;
    enum rpc_stat status = nfs_write(data->fh, offset, nbytes, buf, nfs_dev_write_handler, (uintptr_t)state);
    if (status != RPC_OK) {
        free(state);
        callback(token, EFAULT, 0);
        return;
    }
}


static void nfs_dev_read_handler(uintptr_t token, enum nfs_stat status, fattr_t *fattr, int count, void* data){
    //printf("nfs_dev_read_handler called\n");
    //printf("count = %d\n", count);
    int err = 0;
    nfs_read_state *state = (nfs_read_state*)token;
    assert(state != NULL);

    struct nfs_data *nfs_data = (struct nfs_data*)state->file->vn_data;
    assert(nfs_data != NULL);
    *(nfs_data->fattr) = *fattr;
    //printf("nfs read status = %d\n", status);
    if(status == NFS_OK){
        err = copyout((seL4_Word)state->app_buf, (seL4_Word)data, count);
    } else {
        //error
        err = 1;
    }

    state->callback(state->token, err, (size_t)count, count != 0);
    free(state);
}

static void nfs_dev_read(struct vnode *file, char* buf, size_t nbytes, size_t offset,
                              vop_read_cb_t callback, void *token){
    assert(file != NULL);
    //printf("nfs_dev_read called\n");
    nfs_read_state *state = malloc(sizeof(nfs_read_state));
    if (state == NULL) {
        callback(token, ENOMEM, 0, false);
        return;
    }
    state->app_buf   = buf;
    state->callback  = callback;
    state->token     = token;
    state->file      = file;

    struct nfs_data *data = (struct nfs_data*)(file->vn_data);
    enum rpc_stat status = nfs_read(data->fh, offset, nbytes, nfs_dev_read_handler, (uintptr_t)state);
    if (status != RPC_OK) {
        free(state);
        callback(token, EFAULT, 0, false);
        return;
    }

    //printf("nfs_dev_read finish\n");
}

static void nfs_dev_getdirent_handler(uintptr_t token, enum nfs_stat status, int num_files, char* file_names[], nfscookie_t nfscookie){
    int err = 0;
    size_t size = 0;
    bool finish = false;
    nfs_getdirent_state *state = (nfs_getdirent_state*) token;
    if(status == NFS_OK){
        state->cookie = nfscookie;

        /* We have the file we want */
        if(num_files > state->pos){
            //pos is valid entry
            while(file_names[state->pos][size] != '\0' && size < state->nbyte-1){
                size++;
            }
            char* name = malloc(size+1);
            if (name == NULL) {
                err = ENOMEM;
            } else {
                strncpy(name, file_names[state->pos], size);
                name[size++] = '\0';
                err = copyout((seL4_Word)state->app_buf, (seL4_Word)name, size);
                free(name);
            }
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
                              vop_getdirent_cb_t callback, void *token){
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

static int nfs_dev_stat(struct vnode *file, sos_stat_t *buf){
    if(file == NULL) return EFAULT;
    if(file->vn_data == NULL) return EFAULT;

    struct nfs_data *data = (struct nfs_data*)file->vn_data;
    fattr_t *fattr = (data->fattr);

    /* Turn fattr to sos_stat_t */
    sos_stat_t stat;
    stat.st_type = ST_FILE;
    stat.st_mode = fattr->mode;
    stat.st_size = fattr->size;
    stat.st_mtime.seconds = fattr->mtime.seconds;
    stat.st_mtime.useconds = fattr->mtime.useconds;

    /* Needs to copy the data to sosh space */
    int err = copyout((seL4_Word)buf, (seL4_Word)&stat, sizeof(sos_stat_t));

    return err;
}


void nfs_dev_getstat_lookup_cb(uintptr_t token, enum nfs_stat status,
                                fhandle_t* fh, fattr_t* fattr) {
    //printf("nfs_dev_getstat_lookup_cb called\n");
    nfs_stat_state_t *state = (nfs_stat_state_t*)token;

    if(status != NFS_OK){
        state->callback(state->token, EFAULT);
        free(state);
        return;
    }

    /* Turn fattr to sos_stat_t */
    sos_stat_t stat;
    stat.st_type = ST_FILE;
    stat.st_mode = fattr->mode;
    stat.st_size = fattr->size;
    stat.st_mtime.seconds = fattr->mtime.seconds;
    stat.st_mtime.useconds = fattr->mtime.useconds;

    /* Needs to copy the data to sosh space */
    int err = copyout((seL4_Word)state->stat_buf, (seL4_Word)&stat, sizeof(sos_stat_t));

    state->callback(state->token, err);
    free(state);
}

void nfs_dev_getstat(char *path, size_t path_len, sos_stat_t *buf, nfs_dev_stat_cb_t callback, void *token) {
    //printf("nfs_dev_getstat called\n");
    assert(path != NULL);
    assert(buf != NULL);
    assert(callback != NULL);

    nfs_stat_state_t *cont = malloc(sizeof(nfs_stat_state_t));
    if (cont == NULL) {
        callback(token, ENOMEM);
        return;
    }
    cont->stat_buf = buf;
    cont->callback = callback;
    cont->token = token;

    enum rpc_stat status = nfs_lookup(&mnt_point, path, nfs_dev_getstat_lookup_cb, (uintptr_t)cont);
    if (status != RPC_OK) {
        nfs_dev_getstat_lookup_cb((uintptr_t)cont, status, NULL, NULL);
        return;
    }
}

static void nfs_dev_timeout_handler(uint32_t id, void *data){
    (void)id;
    (void)data;
    nfs_timeout();
    register_timer(100000, nfs_dev_timeout_handler, NULL); //100ms
}

void nfs_dev_setup_timeout(void){
    register_timer(100000, nfs_dev_timeout_handler, NULL); //100ms
}
