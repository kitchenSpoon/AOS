#include <stdlib.h>
#include <errno.h>
#include <assert.h>

#include <nfs/nfs.h>
#include <sel4/sel4.h>
#include <cspace/cspace.h>
#include <fcntl.h>

#include "tool/utility.h"
#include "vfs/vnode.h"
#include "vm/copyinout.h"
#include "dev/nfs_dev.h"
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
struct con_read_state{
    seL4_CPtr reply_cap;
    bool opened_for_reading;
    int is_blocked;
    struct vnode *file;
    char* buf;
    size_t nbytes;
} con_read_state;

typedef struct nfs_open_state{
    seL4_CPtr reply_cap;
    struct vnode *file;
} nfs_open_state;

struct nfs_data{
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
 * To be called when a new vnode is created (i.e. called in nfs_dev_eachopen_end()
 * Assumes that it is okay to do a shallow
 */
static int
nfs_vnode_init(struct vnode* vn, fhandle_t *nfs_fh, fattr_t *nfs_fattr) {
    vn = malloc(sizeof(struct vnode));
    if (vn == NULL) {
        return ENOMEM;
    }

    struct nfs_data *data = malloc(sizeof(struct nfs_data));
    if (data == NULL) {
        free(vn);
        return ENOMEM;
    }
    data->fh    = nfs_fh;
    data->fattr = nfs_fattr;

    struct vnode_ops *vops = malloc(sizeof(struct vnode_ops));
    if (vops == NULL) {
        free(vn);
        free(data);
        return ENOMEM;
    }
    vops->vop_eachopen  = nfs_dev_eachopen;
    vops->vop_eachclose = nfs_dev_eachclose;
    vops->vop_lastclose = nfs_dev_lastclose;
    vops->vop_read      = nfs_dev_read;
    vops->vop_write     = nfs_dev_write;
    vops->vop_getdirent = nfs_dev_getdirent;

    vn->vn_data       = data;
    vn->vn_opencount  = 0;
    vn->vn_ops        = vops;

    return 0;
}

int nfs_dev_init_mntpoint(struct vnode* vn, fhandle_t *mnt_point) {
    struct nfs_data *data = malloc(sizeof(struct nfs_data));
    if (data == NULL) {
        return ENOMEM;
    }
    data->fh = mnt_point;

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

static void nfs_dev_eachopen_end(uintptr_t token, fhandle_t *fh, fattr_t *fattr){
//    //FHSIZE is max fhandle->data size
//    int err = 0;
//
//    /* Cast for convience */
//    nfs_open_state *state = (nfs_open_state*) token;
//
//    /* Copy data to our vnode*/
//    fhandle_t *our_fh = malloc(sizeof(fhandle_t));
//    if(our_fh == NULL){
//        err = 1;
//    }
//    memcpy(our_fh->data, fh->data, sizeof(fh->data));
//
//    fattr_t *our_fattr = malloc(sizeof(fattr_t));
//    if(our_fattr == NULL){
//        free(our_fh);
//        err = 1;
//    }
//    *our_fattr = *fattr;
//
//    state->vnode->vn_data->fh = our_fh;
//    state->vnode->vn_data->fattr = our_fattr;
//    seL4_CPtr_t reply_cap = token->reply_cap;
//
//    /* place fhandle_t into vnode and add vnode into mapping*/
//    //nfs_vnode_init();
//
//    /* reply sosh*/
//    seL4_MessageInfo_t reply = seL4_MessageInfo_new(err, 0, 0, 0);
//    //seL4_SetMR(0, (seL4_Word)len);
//    seL4_Send(reply_cap, reply);
//    cspace_free_slot(cur_cspace, reply_cap);
//
//    free(state);
}

static void nfs_dev_create_handler(uintptr_t token, enum nfs_stat status, fhandle_t *fh, fattr_t *fattr){
//    if(status == NFS_OK){
//        nfs_dev_eachopen_end(token, fh, fattr);
//    } else {
//        //error, nfs fail to create file
//        /* reply sosh*/
//        seL4_MessageInfo_t reply = seL4_MessageInfo_new(1, 0, 0, 0);
//        //seL4_SetMR(0, (seL4_Word)len);
//        seL4_Send(reply_cap, reply);
//        cspace_free_slot(cur_cspace, reply_cap);
//
//        free(state);
//    }
}

static void nfs_dev_create(const char *name, uintptr_t token){

//    nfs_open_state *state = malloc(sizeof(nfs_open_state));
//    sattr_t sattr;
//    sattr.mode = ASD;
//    sattr.uid = 0;//processid
//    sattr.gid = 0;//groupid
//    sattr.size = 0;//what is the size a new file should have
//    timeval atime, mtime;
//    atime = get_timeval();
//    mtime = get_timeval();
//    sattr.atime = atime;
//    sattr.mtime = mtime;
//
//    nfs_create(mnt_point, name, &sattr, nfs_dev_create_handler, token);
}

static void nfs_dev_lookup_handler(uintptr_t token, enum nfs_stat status, fhandle_t *fh, fattr_t *fattr){
//    if(status == NFS_OK){
//        nfs_dev_each_open_end(token, fh, fattr);
//    } else {
//        nfs_dev_create(state->vnode->name, token);
//    }
}

static int nfs_dev_eachopen(struct vnode *file, int flags){
//    printf("nfs_open called\n");
//
//    //Need a fhandle_t to the root of nfs (provided by nfs_mount(which should be called when we start))
//    if(file == NULL) return EFAULT;
//    if(openfile == NULL) return EFAULT;
//    if(mnt_point == NULL) return EFAULT;
//
//    nfs_open_state *state = malloc(sizeof(nfs_open_state));
//    nfs_lookup(mnt_point, file->name, nfs_dev_lookup_handler, state);
//
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
