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
#include "proc/proc.h"

static int _nfs_dev_eachopen(struct vnode *file, int flags);
static int _nfs_dev_eachclose(struct vnode *file, uint32_t flags);
static int _nfs_dev_lastclose(struct vnode *file);
static void _nfs_dev_read(struct vnode *file, char* buf, size_t nbytes, size_t offset,
                              vop_read_cb_t callback, void *token);
static void _nfs_dev_write(struct vnode *file, const char* buf, size_t nbytes, size_t offset,
                              vop_write_cb_t callback, void *token);
static void _nfs_dev_getdirent(struct vnode *dir, char *buf, size_t nbyte,
                      int pos, vop_getdirent_cb_t callback, void *token);
static int _nfs_dev_stat(struct vnode *file, sos_stat_t *buf, vop_stat_cb_t callback, void *token);

#define MAX_IO_BUF 0x1000
#define MAX_SERIAL_SEND 100

extern fhandle_t mnt_point;
extern bool starting_first_process;

/**********************************************************************
 * NFS Utility Functions
 **********************************************************************/
 
 struct nfs_data{
    fhandle_t *fh;
    fattr_t   *fattr;
};

/*
 * Common function between nfs_dev_init & nfs_dev_init_mntpoint
 */
static int
_init_helper(struct vnode *vn, fhandle_t *fh, fattr_t *fattr) {
    assert(vn != NULL);
    assert(vn->vn_ops != NULL);
    assert(fh != NULL);
    printf("_init_helper called, proc = %d\n", proc_get_id());
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

    vn->vn_ops->vop_eachopen  = _nfs_dev_eachopen;
    vn->vn_ops->vop_eachclose = _nfs_dev_eachclose;
    vn->vn_ops->vop_lastclose = _nfs_dev_lastclose;
    vn->vn_ops->vop_read      = _nfs_dev_read;
    vn->vn_ops->vop_write     = _nfs_dev_write;
    vn->vn_ops->vop_getdirent = _nfs_dev_getdirent;
    vn->vn_ops->vop_stat      = _nfs_dev_stat;

    vn->vn_opencount  = 0;
    vn->initialised = true;
    return 0;
}

int
nfs_dev_init_mntpoint_vnode(struct vnode* vn, fhandle_t *mnt_point) {
    int err;
    err = _init_helper(vn, mnt_point, NULL);
    return err;
}

/**********************************************************************
 * NFS Create
 **********************************************************************/
 
 typedef struct nfs_init_state{
    struct vnode *file;
    nfs_dev_init_cb_t callback;
    void *token;
    sattr_t sattr;
    pid_t pid;
} nfs_init_state_t;

static void _nfs_dev_create_handler(uintptr_t token, enum nfs_stat status, fhandle_t *fh, fattr_t *fattr);
static void nfs_dev_lookup_handler(uintptr_t token, enum nfs_stat status, fhandle_t *fh, fattr_t *fattr);

static void
nfs_dev_create(nfs_init_state_t *cont){
    cont->sattr.mode           = S_IRUSR | S_IWUSR;
    cont->sattr.uid            = 0;
    cont->sattr.gid            = 0;
    cont->sattr.size           = 0;
    // No easy way to get the time, time(NULL) is not implemented
    cont->sattr.atime.seconds  = 0;
    cont->sattr.atime.useconds = 0;
    cont->sattr.mtime          = cont->sattr.atime;
    nfs_create(&mnt_point, cont->file->vn_name, &cont->sattr, _nfs_dev_create_handler, (uintptr_t)cont);
}

static void
_nfs_dev_create_handler(uintptr_t token, enum nfs_stat status, fhandle_t *fh, fattr_t *fattr){
    nfs_init_state_t *cont = (nfs_init_state_t*)token;
    assert(cont != NULL);

    if (!starting_first_process && !is_proc_alive(cont->pid)) {
        cont->callback(cont->token, EFAULT);
        free(cont);
        return;
    }

    set_cur_proc(cont->pid);
    printf("nfs_dev_create handler called, proc = %d\n", proc_get_id());
    if(status == NFS_OK){
        int err = 0;

        /* place fhandle_t into vnode and add vnode into mapping*/
        err = _init_helper(cont->file, fh, fattr);
        if (err) {
            cont->callback(cont->token, err);
            free(cont);
            return;
        }

        cont->callback(cont->token, 0);
        free(cont);
        return;
    } else {
        //error, nfs failed to create file
        cont->callback(cont->token, EFAULT);
        free(cont);
        return;
    }
}



static void
nfs_dev_lookup_handler(uintptr_t token, enum nfs_stat status, fhandle_t *fh, fattr_t *fattr){
    int err;
    printf("nfs_dev_lookup_handler called\n");

    nfs_init_state_t *cont = (nfs_init_state_t*)token;
    assert(cont != NULL);

    if (!starting_first_process && !is_proc_alive(cont->pid)) {
        cont->callback(cont->token, EFAULT);
        free(cont);
        return;
    }
    set_cur_proc(cont->pid);

    if(status == NFS_OK){
        err = _init_helper(cont->file, fh, fattr);
        cont->callback(cont->token, err);
        free(cont);
        return;
    }

    nfs_dev_create(cont);
}

void
nfs_dev_init(struct vnode* vn, nfs_dev_init_cb_t callback, void *token) {
    printf("nfs_dev_init called, proc = %d", proc_get_id());
    nfs_init_state_t *cont = malloc(sizeof(nfs_init_state_t));
    if (cont == NULL) {
        callback(token, ENOMEM);
        return;
    }

    cont->file            = vn;
    cont->callback        = callback;
    cont->token           = token;
    cont->pid             = proc_get_id();

    enum rpc_stat status = nfs_lookup(&mnt_point, vn->vn_name, nfs_dev_lookup_handler, (uintptr_t)cont);
    if (status != RPC_OK) {
        free(cont);
        callback(token, EFAULT);
        return;
    }
}

/**********************************************************************
 * NFS Open and close
 **********************************************************************/

static int
_nfs_dev_eachopen(struct vnode *file, int flags){
    (void)file;
    (void)flags;
    return 0;
}

static int _nfs_dev_eachclose(struct vnode *file, uint32_t flags){
    printf("nfs_close, proc = %d\n", proc_get_id());
    return 0;
}

static int _nfs_dev_lastclose(struct vnode *vn) {
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

/**********************************************************************
 * NFS Write
 **********************************************************************/
 
 typedef struct nfs_write_state{
    vop_write_cb_t callback;
    void* token;
    struct vnode *file;
    pid_t pid;
} nfs_write_state;

static void _nfs_dev_write_handler(uintptr_t token, enum nfs_stat status, fattr_t *fattr, int count);

static void _nfs_dev_write(struct vnode *file, const char* buf, size_t nbytes, size_t offset,
                              vop_write_cb_t callback, void *token){
    printf("_nfs_dev_write offset = %d, proc = %d\n", offset, proc_get_id());
    nfs_write_state *cont = malloc(sizeof(nfs_write_state));
    if (cont == NULL) {
        callback(token, ENOMEM, 0);
        return;
    }
    cont->callback  = callback;
    cont->token     = token;
    cont->file      = file;
    cont->pid       = proc_get_id();

    struct nfs_data *data = (struct nfs_data *)file->vn_data;
    enum rpc_stat status = nfs_write(data->fh, offset, nbytes, buf, _nfs_dev_write_handler, (uintptr_t)cont);
    if (status != RPC_OK) {
        free(cont);
        callback(token, EFAULT, 0);
        return;
    }
}

static void _nfs_dev_write_handler(uintptr_t token, enum nfs_stat status, fattr_t *fattr, int count){

    int err = 0;
    nfs_write_state *cont = (nfs_write_state*)token;
    assert(cont != NULL);

    if (starting_first_process || is_proc_alive(cont->pid)) {
        set_cur_proc(cont->pid);
        printf("_nfs_dev_write_handler count = %d, proc = %d\n", count, proc_get_id());

        struct nfs_data *data = (struct nfs_data*)cont->file->vn_data;
        assert(data != NULL);
        *(data->fattr) = *fattr;

        if(status != NFS_OK){
            err = EFAULT;
        }
        
    } else {
        err = EFAULT;
    }

    cont->callback(cont->token, err, count);
    free(cont);
}

/**********************************************************************
 * NFS Read
 **********************************************************************/

typedef struct nfs_read_state{
    struct vnode *file;
    char* app_buf;
    size_t count;
    vop_read_cb_t callback;
    void* token;
    void* data_backup;
    pid_t pid;
} nfs_read_state;

static void _nfs_dev_read_handler(uintptr_t token, enum nfs_stat status, fattr_t *fattr, int count, void* data);
static void _nfs_dev_read_end(void* token, int err);

static void _nfs_dev_read(struct vnode *file, char* buf, size_t nbytes, size_t offset,
                              vop_read_cb_t callback, void *token){
    assert(file != NULL);
    printf("_nfs_dev_read called, proc = %d\n", proc_get_id());
    nfs_read_state *cont = malloc(sizeof(nfs_read_state));
    if (cont == NULL) {
        callback(token, ENOMEM, 0, false);
        return;
    }
    cont->app_buf   = buf;
    cont->count     = 0;
    cont->callback  = callback;
    cont->token     = token;
    cont->file      = file;
    cont->data_backup = NULL;
    cont->pid       = proc_get_id();

    struct nfs_data *data = (struct nfs_data*)(file->vn_data);
    enum rpc_stat status = nfs_read(data->fh, offset, nbytes, _nfs_dev_read_handler, (uintptr_t)cont);
    if (status != RPC_OK) {
        free(cont);
        callback(token, EFAULT, 0, false);
    }
    return;

    //printf("_nfs_dev_read finish\n");
}

static void _nfs_dev_read_handler(uintptr_t token, enum nfs_stat status, fattr_t *fattr, int count, void* data){
    //printf("count = %d\n", count);
    int err;
    nfs_read_state *cont = (nfs_read_state*)token;
    assert(cont != NULL);

    if (!starting_first_process && !is_proc_alive(cont->pid)) {
        _nfs_dev_read_end((void*)token, EFAULT);
        return;
    }

    set_cur_proc(cont->pid);
    printf("_nfs_dev_read_handler called, proc = %d\n", proc_get_id());
    //printf("nfs read status = %d\n", status);
    if(status == NFS_OK){
        struct nfs_data *nfs_data = (struct nfs_data*)cont->file->vn_data;
        assert(nfs_data != NULL);
        *(nfs_data->fattr) = *fattr;

        void * data_backup = malloc((size_t)count);
        if (data_backup == NULL) {
            _nfs_dev_read_end((void*)token, ENOMEM);
            return;
        }
        cont->data_backup = data_backup;

        memcpy(data_backup, data, (size_t)count);

        cont->count = count;
        err = copyout((seL4_Word)cont->app_buf, (seL4_Word)data_backup, count, _nfs_dev_read_end, (void*)cont);
        if(err){
            _nfs_dev_read_end((void*)token, err);
        }
        return;
    } else {
        _nfs_dev_read_end((void*)token, EFAULT);
        return;
    }
}

static void _nfs_dev_read_end(void* token, int err){
    nfs_read_state *cont = (nfs_read_state*)token;
    assert(cont != NULL);

    if (cont->data_backup != NULL) {
        free(cont->data_backup);
    }

    if (err) {
        cont->callback(cont->token, err, 0, false);
        free(cont);
        return;
    }

    cont->callback(cont->token, 0, cont->count, cont->count != 0);
    free(cont);
}

/**********************************************************************
 * NFS GetDirent
 **********************************************************************/
 
 typedef struct nfs_getdirent_state_t{
    int pos;
    size_t nbyte;
    size_t size;
    char* name;
    char* app_buf;
    nfscookie_t cookie;
    vop_getdirent_cb_t callback;
    void* token;
    pid_t pid;
} nfs_getdirent_state_t;

static void _nfs_dev_getdirent_handler(uintptr_t token, enum nfs_stat status, int num_files,
        char* file_names[], nfscookie_t nfscookie);
static void _nfs_dev_getdirent_done_copyout(void* token, int err);

static void 
_nfs_dev_getdirent(struct vnode *dir, char *buf, size_t nbyte, int pos,
                              vop_getdirent_cb_t callback, void *token){
    printf("_nfs_dev_getdirent called, proc = %d\n", proc_get_id());
    nfs_getdirent_state_t *cont = malloc(sizeof(nfs_getdirent_state_t));
    if (cont == NULL) {
        callback(token, ENOMEM, 0);
        return;
    }
    cont->app_buf   = buf;
    cont->nbyte     = nbyte;
    cont->pos       = pos;
    cont->callback  = callback;
    cont->token     = token;
    cont->pid       = proc_get_id();
    enum rpc_stat status = nfs_readdir(&mnt_point, 0, _nfs_dev_getdirent_handler, (uintptr_t)cont);
    if (status != RPC_OK) {
        cont->callback(cont->token, EFAULT, 0);
        free(cont);
    }
}

static void
_nfs_dev_getdirent_handler(uintptr_t token, enum nfs_stat status, int num_files,
        char* file_names[], nfscookie_t nfscookie){
    int err = 0;
    size_t size = 0;
    bool finish = false;
    nfs_getdirent_state_t *cont = (nfs_getdirent_state_t*) token;
    assert(cont != NULL);
    if (!starting_first_process && !is_proc_alive(cont->pid)) {
        cont->callback(cont->token, EFAULT, 0);
        free(cont);
        return;
    }
    set_cur_proc(cont->pid);

    if(status == NFS_OK){
        cont->cookie = nfscookie;

        /* We have the file we want */
        if(num_files > cont->pos){
            /* pos is valid entry */

            while(file_names[cont->pos][size] != '\0' && size < cont->nbyte-1){
                size++;
            }
            char* name = malloc(size+1);
            if (name == NULL) {
                err = ENOMEM;
                finish = true;
            } else {
                strncpy(name, file_names[cont->pos], size);
                name[size++] = '\0';
                cont->size = size;
                cont->name = name;
                err = copyout((seL4_Word)cont->app_buf, (seL4_Word)name, size,
                        _nfs_dev_getdirent_done_copyout, (void*)cont);

                //set callback when there is an error, otherwise wait till copyout returns
                finish = (bool)err;
            }
        } else {
            if(nfscookie == 0){  /* No more entry*/
                //pos is next free entry
                if(cont->pos == num_files + 1){
                    size = 0;
                } else {
                    err = EINVAL;
                }
                finish = true;
            } else {             /* Read more */
                cont->pos -= num_files;
                assert(cont->pos >= 0);
                enum rpc_stat rpc_status = nfs_readdir(&mnt_point, nfscookie,
                        _nfs_dev_getdirent_handler, (uintptr_t)cont);
                if (rpc_status != RPC_OK) {
                    finish = true;
                }
            }
        }

    } else {
        finish = true;
        err = EFAULT;
    }

    if(finish){
        cont->callback(cont->token, err, size);
        free(cont);
    }
}

static void 
_nfs_dev_getdirent_done_copyout(void* token, int err){
    nfs_getdirent_state_t *cont = (nfs_getdirent_state_t*) token;
    assert(cont != NULL);
    if (starting_first_process || is_proc_alive(cont->pid)) {
        set_cur_proc(cont->pid);
    } else {
        err = EFAULT;
    }

    free(cont->name);
    if(err){
        cont->callback(cont->token, err, 0);
        free(cont);
        return;
    }

    cont->callback(cont->token, 0, cont->size);
    free(cont);
}

/**********************************************************************
 * NFS Stat
 **********************************************************************/
 
 typedef struct {
    vop_stat_cb_t callback;
    void *token;
    sos_stat_t *stat;
    pid_t pid;
} nfs_stat_state_t;
 
typedef struct nfs_stat_state{
    sos_stat_t* stat_buf;
    nfs_dev_stat_cb_t callback;
    void* token;
    sos_stat_t* stat;
    char* path;
    pid_t pid;
} nfs_dev_getstat_state_t;

static void _nfs_dev_stat_copyout_cb(void *token, int err);
static void _nfs_dev_getstat_lookup_end(void* token, int err);
static void _nfs_dev_getstat_lookup_cb(uintptr_t token, enum nfs_stat status,
                                fhandle_t* fh, fattr_t* fattr);
                                
void nfs_dev_getstat(char *path, size_t path_len, sos_stat_t *buf,
        nfs_dev_stat_cb_t callback, void *token) {
    printf("nfs_dev_getstat called, proc = %d\n", proc_get_id());
    assert(path != NULL);
    assert(buf != NULL);
    assert(callback != NULL);

    nfs_dev_getstat_state_t *cont = malloc(sizeof(nfs_dev_getstat_state_t));
    if (cont == NULL) {
        callback(token, ENOMEM);
        return;
    }
    cont->path = path;
    cont->stat_buf = buf;
    cont->callback = callback;
    cont->token = token;
    cont->stat = NULL;
    cont->pid  = proc_get_id();

    enum rpc_stat status = nfs_lookup(&mnt_point, path, _nfs_dev_getstat_lookup_cb, (uintptr_t)cont);
    if (status != RPC_OK) {
        _nfs_dev_getstat_lookup_cb((uintptr_t)cont, status, NULL, NULL);
        return;
    }
}

static int 
_nfs_dev_stat(struct vnode *file, sos_stat_t *buf, vop_stat_cb_t callback, void *token){
    printf("_nfs_dev_stat called, proc = %d\n", proc_get_id());
    if(file == NULL) return EFAULT;
    if(file->vn_data == NULL) return EFAULT;

    struct nfs_data *data = (struct nfs_data*)file->vn_data;
    fattr_t *fattr = (data->fattr);

    /* Turn fattr to sos_stat_t */
    sos_stat_t *stat = malloc(sizeof(sos_stat_t));
    if (stat == NULL) {
        return ENOMEM;
    }
    stat->st_type = ST_FILE;
    stat->st_mode = fattr->mode;
    stat->st_size = fattr->size;
    stat->st_mtime.seconds = fattr->mtime.seconds;
    stat->st_mtime.useconds = fattr->mtime.useconds;

    nfs_stat_state_t *cont = malloc(sizeof(nfs_stat_state_t));
    if (cont == NULL) {
        free(stat);
        return ENOMEM;
    }
    cont->callback = callback;
    cont->token    = token;
    cont->stat     = stat;
    cont->pid      = proc_get_id();

    /* Needs to copy the data to sosh space */
    int err = copyout((seL4_Word)buf, (seL4_Word)&stat, sizeof(sos_stat_t),
            _nfs_dev_stat_copyout_cb, (void*)cont);
    if (err) {
        free(stat);
        free(cont);
        return err;
    }
    return 0;
}

static void 
_nfs_dev_stat_copyout_cb(void *token, int err) {
    printf("_nfs_dev_stat_copyout_cb\n");
    nfs_stat_state_t *cont = (nfs_stat_state_t*)token;
    assert(cont != NULL);

    if (starting_first_process || is_proc_alive(cont->pid)) {
        set_cur_proc(cont->pid);
    } else {
        err = EFAULT;
    }
    free(cont->stat);
    cont->callback(cont->token, err);
    free(cont);
}

static void
_nfs_dev_getstat_lookup_cb(uintptr_t token, enum nfs_stat status,
                                fhandle_t* fh, fattr_t* fattr) {
    nfs_dev_getstat_state_t *cont = (nfs_dev_getstat_state_t*)token;
    assert(cont != NULL);

    if (!starting_first_process && !is_proc_alive(cont->pid)) {
        _nfs_dev_getstat_lookup_end((void*)cont, EFAULT);
        return;
    }
    set_cur_proc(cont->pid);
    printf("_nfs_dev_getstat_lookup_cb called, proc = %d\n", proc_get_id());

    if(status != NFS_OK){
        _nfs_dev_getstat_lookup_end((void*)cont, EFAULT);
        return;
    }

    /* Turn fattr to sos_stat_t */
    cont->stat = malloc(sizeof(sos_stat_t));
    if (cont->stat == NULL) {
        _nfs_dev_getstat_lookup_end((void*)cont, ENOMEM);
        return;
    }
    cont->stat->st_type = ST_FILE;
    cont->stat->st_mode = fattr->mode;
    cont->stat->st_size = fattr->size;
    cont->stat->st_mtime.seconds = fattr->mtime.seconds;
    cont->stat->st_mtime.useconds = fattr->mtime.useconds;

    /* Show no permission when user ask for swap file perm
     * I know magic string is bad, but changing the entire function interface
     * for such a small coherence manner doesn't worth it
     */
    if (strcmp(cont->path, "swap") == 0) {
        cont->stat->st_mode = 0;
    }

    /* Needs to copy the data to sosh space */
    copyout((seL4_Word)cont->stat_buf, (seL4_Word)cont->stat, sizeof(sos_stat_t),
            _nfs_dev_getstat_lookup_end, (void*)cont);
    return;
}

static void
_nfs_dev_getstat_lookup_end(void* token, int err){
    nfs_dev_getstat_state_t *cont = (nfs_dev_getstat_state_t*)token;
    assert(cont != NULL);

    if (starting_first_process || is_proc_alive(cont->pid)) {
        set_cur_proc(cont->pid);
    } else {
        err = EFAULT;
    }

    if (cont->stat != NULL) {
        free(cont->stat);
    }
    cont->callback(cont->token, err);
    free(cont);
}

/**********************************************************************
 * NFS Timeout
 **********************************************************************/

static void 
_nfs_dev_timeout_handler(uint32_t id, void *data){
    (void)id;
    (void)data;
    nfs_timeout();
    register_timer(100000, _nfs_dev_timeout_handler, NULL); //100ms
}

void nfs_dev_setup_timeout(void){
    register_timer(100000, _nfs_dev_timeout_handler, NULL); //100ms
}

int nfs_dev_get_fhandle(struct vnode *vn, fhandle_t **fh) {
    if (fh == NULL || vn == NULL) {
        return EINVAL;
    }
    if (vn->vn_data == NULL) {
        return EFAULT;
    }

    struct nfs_data *data = (struct nfs_data*)vn->vn_data;
    *fh = data->fh;

    return 0;
}
