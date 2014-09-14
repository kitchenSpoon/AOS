#include <stdlib.h>
#include <errno.h>
#include <assert.h>

#include <nfs/nfs.h>
#include <sel4/sel4.h>
#include <cspace/cspace.h>
#include <fcntl.h>

#include "tool/utility.h"
#include "dev/console.h"
#include "vfs/vnode.h"
#include "vm/copyinout.h"

//#define MAX_IO_BUF 0x1000
#define MAX_IO_BUF 6000
#define MAX_SERIAL_SEND 100

extern fhandle_t mnt_point;

struct console{
} console;

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
    seL4_CPtr reply_cap;
    char* app_buf;
    nfscookie_t cookie;
} nfs_getdirent_state;

struct nfs_data{
    fhandle_t *fh;
    fattr_t *fattr;
} nfs_data;

int
nfs_vnode_init(vnode* vn, fhandle_t nfs_fh) {
    vn = malloc(sizeof(struct vnode));
    if (vn == NULL) {
        return ENOMEM;
    }

    struct nfs_data *data = malloc(sizeof(nfs_data));
    if (data == NULL) {
        free(vn);
        return ENOMEM;
    }
    data->fh = fh;
    vn->vn_data       = data;
    vn->vn_opencount  = 0;

    struct vnode_ops *vops = malloc(sizeof(struct vnode_ops));
    if (vops == NULL) {
        free(vn);
        free(data);
        return ENOMEM;
    }
    vops->vop_eachopen  = nfs_eachopen;
    vops->vop_eachclose = nfs_eachclose;
    vops->vop_lastclose = cnfs_lastclose;
    vops->vop_read  = nfs_read;
    vops->vop_write = nfs_write;

    vn->vn_ops = vops;

    return 0;
}

int
con_destroy_vnode(void) {
    if (con_vnode == NULL) {
        return EINVAL;
    }
    /* If any of these fails, that means we have a bug */
    assert(con_vnode->vn_ops != NULL);
    if (con_vnode->vn_opencount != 1) {
        return EINVAL;
    }

    console.serial = NULL;
    console.buf_size = 0;
    console.start = 0;
    console.end= 0;
    console.is_init = 0;

    free(con_vnode->vn_ops);
    free(con_vnode);
    return 0;
}

static void read_handler(struct serial * serial , char c){
    //printf("read_handler called, c = %d\n", (int)c);
    if(console.buf_size < MAX_IO_BUF){
        console.buf[console.end++] = c;
        console.end %= MAX_IO_BUF;
        console.buf_size++;
    }

    if(con_read_state.is_blocked && c == '\n'){
        struct vnode *file = con_read_state.file;
        char* buf = con_read_state.buf;
        size_t nbytes = con_read_state.nbytes;
        seL4_CPtr reply_cap = con_read_state.reply_cap;
        con_read(file, buf, nbytes, reply_cap);
    }
}


void nfs_dev_eachopen_end(uintptr_t token, fhandle_t *fh, fattr_t *fattr){
    //FHSIZE is max fhandle->data size
    int err = 0;

    /* Cast for convience */
    nfs_open_state *state = (nfs_open_state*) token;

    /* Copy data to our vnode*/
    fhandle_t *our_fh = malloc(sizeof(fhandle_t));
    if(our_fh == NULL){
        err = 1;
    }
    memcpy(our_fh->data, fh->data, sizeof(fh->data));

    fattr_t *our_fattr = malloc(sizeof(fattr_t));
    if(our_fattr == NULL){
        free(our_fh);
        err = 1;
    }
    *our_fattr = *fattr;

    state->vnode->vn_data->fh = our_fh;
    state->vnode->vn_data->fattr = our_fattr;
    seL4_CPtr_t reply_cap = token->reply_cap;

    /* place fhandle_t into vnode and add vnode into mapping*/
    //nfs_vnode_init();

    /* reply sosh*/
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(err, 0, 0, 0);
    //seL4_SetMR(0, (seL4_Word)len);
    seL4_Send(reply_cap, reply);
    cspace_free_slot(cur_cspace, reply_cap);

    free(state);
}

void nfs_dev_create_handler(uintptr_t token, enum nfs_stat status, fhandle_t *fh, fattr_t *fattr){
    if(status == NFS_OK){
        nfs_dev_eachopen_end(token, fh, fattr);
    } else {
        //error, nfs fail to create file
        /* reply sosh*/
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(1, 0, 0, 0);
        //seL4_SetMR(0, (seL4_Word)len);
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);

        free(state);
    }
}

void nfs_dev_create(const char *name, uintptr_t token){

    nfs_open_state *state = malloc(sizeof(nfs_open_state));
    sattr_t sattr;
    sattr.mode = ASD;
    sattr.uid = 0;//processid
    sattr.gid = 0;//groupid
    sattr.size = 0;//what is the size a new file should have
    timeval atime, mtime;
    atime = get_timeval();
    mtime = get_timeval();
    sattr.atime = atime;
    sattr.mtime = mtime;

    nfs_create(mnt_point, name, &sattr, nfs_dev_create_handler, token);
}

void nfs_dev_lookup_handler(uintptr_t token, enum nfs_stat status, fhandle_t *fh, fattr_t *fattr){
    if(status == NFS_OK){
        nfs_dev_each_open_end(token, fh, fattr);
    } else {
        nfs_dev_create(state->vnode->name, token);
    }
}

int nfs_dev_eachopen(struct vnode *file, int flags, struct openfile *openfile){
    printf("nfs_open called\n");

    //Need a fhandle_t to the root of nfs (provided by nfs_mount(which should be called when we start))
    if(file == NULL) return EFAULT;
    if(openfile == NULL) return EFAULT;
    if(mnt_point == NULL) return EFAULT;

    nfs_open_state *state = malloc(sizeof(nfs_open_state));
    nfs_lookup(mnt_point, file->name, nfs_dev_lookup_handler, state);

    return 0;
}

int nfs_dev_eachclose(struct vnode *file, uint32_t flags){
    printf("con_eachclose\n");
    if(flags == O_RDWR || flags == O_RDONLY) {
        if(console.serial == NULL) {
            return EFAULT;
        }
        int err = serial_register_handler(console.serial, NULL);
        if(err){ // should not happen
            return EFAULT;
        }

        console.buf_size = 0;
        con_read_state.opened_for_reading = 0;
    }
    return 0;
}

int nfs_lastclose(struct vnode *vn) {
    (void)vn;
    return 0;
}

void nfs_dev_write_handler(uintptr_t token, enum nfs_stat status, fattr_t *fattr, int count){
    int err = 0;
    if(status == NFS_OK){
        /* Cast for convience */
        nfs_write_state *state = (nfs_write_state*) token;

        // can we write more data than the file can hold?
        /* Update openfile */
        state->openfile->offset += count;

    } else {
        //error
        err = 1;
    }

    /* reply sosh*/
    seL4_CPtr reply_cap = state->reply_cap;
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(err, 0, 0, 1);
    seL4_SetMR(0, (seL4_Word)count);
    seL4_Send(reply_cap, reply);
    cspace_free_slot(cur_cspace, reply_cap);

    free(state);
}

//we do not need len anymore since it will be invalid when our callack finishes, please remove or not use it
int nfs_dev_write(struct vnode *file, const char* buf, size_t nbytes, size_t offset, size_t *len, struct openfile *openfile, seL4_CPtr reply_cap) {

    nfs_write_state state = malloc(sizeof(nfs_write_state));
    state->reply = reply_cap;
    state->openfile = openfile;
    rpc_stat status = nfs_write(mnt_point, offset, nbytes, buf, nfs_dev_write_handler, state);
    return 0;
}

void nfs_dev_read_handler(uintptr_t token, enum nfs_stat status, fattr_t *fattr, int count, void* data){
    int err = 0;
    if(status == NFS_OK){
        /* Cast for convience */
        nfs_read_state *state = (nfs_read_state*) token;

        /* Needs to copy the data to sosh space */
        int err = copyout(state->app_buf, data, count);

        /* Update openfile */
        if(!err){
            openfile->offset += count;
        }
    } else {
        //error
        err = 1;
    }


    /* reply sosh*/
    seL4_CPtr reply_cap = state->reply_cap;
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(err, 0, 0, 1);
    seL4_SetMR(0, (seL4_Word)count);
    seL4_Send(reply_cap, reply);
    cspace_free_slot(cur_cspace, reply_cap);

    free(state);
}
int nfs_dev_read(struct vnode *file, char* buf, size_t nbytes, int offset, struct openfile *openfile, seL4_CPtr reply_cap){
    nfs_read_state *state = malloc(sizeof(nfs_read_state));
    state->reply_cap = reply_cap;
    state->app_buf = buf;
    state->openfile = openfile;
    nfs_read(file->vn_data->fh, offset, nbytes, nfs_dev_read_handler, state);

    return 0;
}

void nfs_dev_getdirent_handler(uintptr_t token, enum nfs_stat status, int num_files, char* file_names[], nfscookie_t nfscookie){
    int size = 0, finish = 0, err = 0;
    if(status == NFS_OK){
        nfs_getdirent_state *state = (nfs_getdirent_state*) token;
        state->cookie = nfscookie;

        /* We have the file we want */
        if(num_files >= state->pos){
            //pos is next free entry
            if(state->pos == num_files + 1){
                size = 0;
            //pos is valid entry
            } else {
                char* name = file_names[pos];
                while(name[size] != '\0' && size < state->nbyte){
                    size++;
                }

                err = copyout(state->buf, name, size);
            }
            finish = 1;
        } else {
            if(nfscookie == 0){  /* No more entry*/
                //non-existent entry
                finish = 1;
            } else {             /* Read more */
                pos -= num_files;
                assert(pos >= 0);
                nfs_readdir(mnt_point, nfscookie, nfs_dev_getdirent_handler, state);
            }
        }
    } else {
        finish = 1;
        err = 1;
    }

    if(finish){
        /* reply sosh*/
        seL4_CPtr reply_cap = state->reply_cap;
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(err, 0, 0, 1);
        seL4_SetMR(0, (seL4_Word)size);
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);

        free(state);
    }
}

int nfs_dev_getdirent(struct vnode *file, char *buf, int pos, size_t nbyte, seL4_CPtr reply_cap){
    nfs_getdirent_state *state = malloc(sizeof(nfs_getdirent_state));
    state->reply_cap = reply_cap;
    state->pos = pos;
    state->app_buf = buf;
    state->nbyte = nbyte;
    nfs_readdir(mnt_point, 0, nfs_dev_getdirent_handler, state);
}

int nfs_dev_stat(struct vnode *file, sos_stat_t *buf){
    if(file == NULL) return EFAULT;
    if(file->vn == NULL) return EFAULT;
    if(file->vn->vn_data == NULL) return EFAULT;

    //need to check if fattr has pointers in it
    fattr_t *fattr = (file->vn->vn_data->fattr);

    /* Turn fattr to sos_stat_t */
    sos_stat_t stat;
    stat.st_type = ST_FILE;
    stat.st_mode = fattr->mode;
    stat.st_size = fattr->size;
    stat.st_mtime.seconds = fattr->mtime.seconds;
    stat.st_mtime.useconds = fattr->mtime.useconds;

    /* Needs to copy the data to sosh space */
    int err = copyout(buf, stat, sizeof(sos_stat_t));


    /* reply sosh*/
    seL4_CPtr reply_cap = state->reply_cap;
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(err, 0, 0, 0);
    seL4_Send(reply_cap, reply);
    cspace_free_slot(cur_cspace, reply_cap);

    free(state);
    return 0;
}

void nfs_dev_timeout_handler(){
    nfs_timeout();
    register_timer(100000, nfs_dev_timeout_handler, NULL); //100ms
}

void nfs_dev_timeout(){
    register_timer(100000, nfs_dev_timeout_handler, NULL); //100ms
}
