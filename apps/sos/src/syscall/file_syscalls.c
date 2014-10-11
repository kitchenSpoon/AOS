#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <sel4/sel4.h>
#include <serial/serial.h>
#include <fcntl.h>

#include "tool/utility.h"
#include "vm/addrspace.h"
#include "vfs/vfs.h"
#include "proc/proc.h"
#include "syscall/syscall.h"
#include "vm/copyinout.h"
#include "vm/vm.h"

#define MAX_SERIAL_TRY  0x100
#define MAX_IO_BUF      0x1000

void serv_sys_print(seL4_CPtr reply_cap, char* message, size_t len) {
    struct serial* serial = serial_init(); //serial_init does the cacheing

    size_t sent = 0;
    int tries = 0;
    while (sent < len && tries < MAX_SERIAL_TRY) {
        sent += serial_send(serial, message+sent, len-sent);
        tries++;
    }

    seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
    seL4_SetMR(0, (seL4_Word)sent);
    seL4_Send(reply_cap, reply);
    cspace_free_slot(cur_cspace, reply_cap);
}

typedef struct {
    seL4_CPtr reply_cap;
    char *kbuf;
    size_t nbyte;
    uint32_t flags;
} cont_open_t;

static void
serv_sys_open_end(void *token, int err, int fd) {
    printf("serv_sys_open_end called\n");
    cont_open_t *cont = (cont_open_t*)token;
    assert(cont != NULL);

    if (cont->kbuf == NULL) {
        free(cont->kbuf);
    }

    seL4_MessageInfo_t reply;
    reply = seL4_MessageInfo_new(err, 0, 0, 1);
    seL4_SetMR(0, (seL4_Word)fd);
    seL4_Send(cont->reply_cap, reply);
    cspace_free_slot(cur_cspace, cont->reply_cap);
    //printf("--serv_cont_end = %p\n", cont);

    free(cont);
}

static void
serv_sys_open_copyin_cb(void *token, int err) {
    if (err) {
        serv_sys_open_end(token, err, -1);
        return;
    }
    cont_open_t *cont = (cont_open_t*)token;
    cont->kbuf[cont->nbyte] = '\0';

    file_open(cont->kbuf, (int)cont->flags, serv_sys_open_end, (void*)cont);
}

void serv_sys_open(seL4_CPtr reply_cap, seL4_Word path, size_t nbyte, uint32_t flags){
    printf("serv_sys_open called\n");
    cont_open_t *cont = malloc(sizeof(cont_open_t));
    //printf("--serv_cont = %p, size = %u\n", cont, sizeof(cont_open_t));
    if (cont == NULL) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(ENOMEM, 0, 0, 1);
        seL4_SetMR(0, (seL4_Word)-1);
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }
    cont->reply_cap = reply_cap;
    cont->kbuf      = NULL;
    cont->nbyte     = nbyte;
    cont->flags     = flags;

    addrspace_t *as = proc_getas();
    if ((nbyte > MAX_NAME_LEN) || (!as_is_valid_memory(as, path, nbyte, NULL))){
        serv_sys_open_end((void*)cont, EINVAL, -1);
        return;
    }

    cont->kbuf = malloc(MAX_NAME_LEN+1);
    if (cont->kbuf == NULL) {
        printf("serv_sys_open: nomem for kbuf\n");
        serv_sys_open_end((void*)cont, ENOMEM, -1);
        return;
    }

    int err;
    err = copyin((seL4_Word)cont->kbuf, (seL4_Word)path, (size_t)nbyte,
            serv_sys_open_copyin_cb, (void*)cont);
    if (err) {
        printf("serv_sys_open: err when copyin\n");
        serv_sys_open_end((void*)cont, err, -1);
        return;
    }
}

void serv_sys_close(seL4_CPtr reply_cap, int fd){
    int err = 0;
    //printf("fd = %d\n", fd);
    if (fd < 0 || fd >= PROCESS_MAX_FILES) {
        err = EINVAL;
    }

    err = err || file_close(fd);

    seL4_MessageInfo_t reply = seL4_MessageInfo_new(err, 0, 0, 0);
    seL4_Send(reply_cap, reply);
    cspace_free_slot(cur_cspace, reply_cap);
}

typedef struct {
    seL4_CPtr reply_cap;
    struct openfile *file;
    size_t bytes_read;
    size_t bytes_wanted;
    char* buf;
} cont_read_t;

void serv_sys_read_end(void *token, int err, size_t size, bool more_to_read){
    //printf("serv_read_end called\n");
    //printf("serv_read_end size = %u\n", size);
    cont_read_t *cont = (cont_read_t*)token;

    /* Update file offset */
    if(!err){
        cont->file->of_offset += size;
        cont->bytes_read += size;
    }

    //printf("serv_read_end bytes_read = %u, bytes_wanted = %u\n", cont->bytes_read, cont->bytes_wanted);
    if(err || !more_to_read || cont->bytes_read >= cont->bytes_wanted){
        /* Reply app*/
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(err, 0, 0, 1);
        seL4_SetMR(0, (seL4_Word)cont->bytes_read);
        seL4_Send(cont->reply_cap, reply);
        cspace_free_slot(cur_cspace, cont->reply_cap);

        free(cont);
    } else {
        /* Theres more stuff to read */
        VOP_READ(cont->file->of_vnode, cont->buf + cont->bytes_read,
                 MIN(cont->bytes_wanted - cont->bytes_read, MAX_IO_BUF),
                 cont->file->of_offset, serv_sys_read_end, (void*)cont);
    }
    //printf("serv_read_end out\n");
}

void serv_sys_read(seL4_CPtr reply_cap, int fd, seL4_Word buf, size_t nbyte){
    //printf("serv read\n");
    int err;
    cont_read_t *cont = malloc(sizeof(cont_read_t));
    if (cont == NULL) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(ENOMEM, 0, 0, 1);
        seL4_SetMR(0, (seL4_Word)0);
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }
    cont->reply_cap = reply_cap;
    cont->file = NULL;
    cont->buf = (char*)buf;
    cont->bytes_read = 0;
    cont->bytes_wanted = nbyte;

    //have to read multiple times if nbyte >= MAX_IO_BUF
    bool is_inval = (fd < 0) || (fd >= PROCESS_MAX_FILES);// || (nbyte >= MAX_IO_BUF);

    uint32_t permissions = 0;
    is_inval = is_inval || (!as_is_valid_memory(proc_getas(), buf, nbyte, &permissions));
    is_inval = is_inval || (!(permissions & seL4_CanWrite));
    if(is_inval){
        serv_sys_read_end((void*)cont, EINVAL, 0, false);
        return;
    }

    struct openfile *file;
    err = filetable_findfile(fd, &file);
    if (err) {
        serv_sys_read_end((void*)cont, EINVAL, 0, false);
        return;
    }
    cont->file = file;

    //check read permissions
    if(file->of_accmode != O_RDWR &&
        file->of_accmode != O_RDONLY){
        serv_sys_read_end((void*)cont, EACCES, 0, false);
        return;
    }

    VOP_READ(file->of_vnode, (char*)buf, MIN(nbyte, MAX_IO_BUF), file->of_offset, serv_sys_read_end, (void*)cont);
    //printf("serv read finish\n");
}

typedef struct {
    seL4_CPtr reply_cap;
    struct openfile *file;
    char *buf;
    char *kbuf;
    size_t nbyte;
    size_t byte_read;
    size_t wanna_send;
} cont_write_t;

static void serv_sys_write_get_kbuf(void *token, seL4_Word kvaddr);
static void serv_sys_write_copyin(void *token, int err, size_t size);
static void serv_sys_write_do_write(void *token, int err);
static void serv_sys_write_end(cont_write_t* cont, int err);

void serv_sys_write(seL4_CPtr reply_cap, int fd, seL4_Word buf, size_t nbyte) {

    //printf("serv_sys_write called\n");
    int err;

    cont_write_t *cont = malloc(sizeof(cont_write_t));
    if (cont == NULL) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(ENOMEM, 0, 0, 1);
        seL4_SetMR(0, (seL4_Word)0);
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }
    cont->reply_cap  = reply_cap;
    cont->file       = NULL;
    cont->buf        = (char*)buf;
    cont->kbuf       = NULL;
    cont->nbyte      = nbyte;
    cont->byte_read  = 0;
    cont->wanna_send = 0;

    addrspace_t *as = proc_getas();
    bool is_inval = (fd < 0) || (fd >= PROCESS_MAX_FILES);
    is_inval = is_inval || (!as_is_valid_memory(as, buf, nbyte, NULL));
    if(is_inval){
        serv_sys_write_end(cont, EINVAL);
        return;
    }

    struct openfile *file;
    err = filetable_findfile(fd, &file);
    if (err) {
        serv_sys_write_end(cont, EINVAL);
        return;
    }
    cont->file = file;

    //check write permissions
    if(file->of_accmode != O_RDWR &&
        file->of_accmode != O_WRONLY){
        serv_sys_write_end(cont, EACCES);
        return;
    }

    err = frame_alloc(0, NULL, true, serv_sys_write_get_kbuf, (void*)cont);
    if (err) {
        serv_sys_write_end(cont, EFAULT);
        return;
    }
}

static void
serv_sys_write_get_kbuf(void *token, seL4_Word kvaddr) {
    cont_write_t *cont = (cont_write_t*)token;
    char *kbuf = (char*)kvaddr;
    if (kbuf == NULL) {
        printf("serv_sys_write_get_kbuf: ENOMEM\n");
        serv_sys_write_end(cont, ENOMEM);
        return;
    }
    cont->kbuf = kbuf;
    serv_sys_write_copyin((void*)cont, 0, 0);
}

/* Is inteded to be called repeatedly in this layer if the write data is too large */
static void
serv_sys_write_copyin(void *token, int err, size_t size) {
    //printf("serv_sys_write_copyin called\n");
    cont_write_t *cont = (cont_write_t*)token;
    assert(cont != NULL);

    if (err) {
        serv_sys_write_end(cont, err);
        return;
    }

    cont->file->of_offset += size;
    cont->byte_read       += size;
    if (cont->byte_read < cont->nbyte) {
        seL4_Word vaddr;
        vaddr = (seL4_Word)cont->buf + cont->byte_read;
        cont->wanna_send = PAGE_SIZE - (vaddr - PAGE_ALIGN(vaddr));
        cont->wanna_send = MIN(cont->wanna_send, cont->nbyte - cont->byte_read);

        err = copyin((seL4_Word)cont->kbuf, (seL4_Word)(cont->buf + cont->byte_read), cont->wanna_send,
                serv_sys_write_do_write, (void*)cont);
        if (err) {
            printf("serv_sys_write_copyin: fail when copyin\n");
            serv_sys_write_end(cont, EINVAL);
            return;
        }
    } else {
        serv_sys_write_end(cont, 0);
        return;
    }
    //printf("serv_sys_write_copyin ended\n");
}

static void
serv_sys_write_do_write(void *token, int err) {
    cont_write_t *cont = (cont_write_t*)token;
    assert(cont != NULL);

    if (err) {
        serv_sys_write_end(cont, err);
        return;
    }
    VOP_WRITE(cont->file->of_vnode, cont->kbuf, cont->wanna_send, cont->file->of_offset,
            serv_sys_write_copyin, (void*)cont);
}

static
void serv_sys_write_end(cont_write_t* cont, int err) {
    //printf("serv_write_end\n");

    /* Reply app*/
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(err, 0, 0, 1);
    seL4_SetMR(0, (seL4_Word)cont->byte_read);
    seL4_Send(cont->reply_cap, reply);
    cspace_free_slot(cur_cspace, cont->reply_cap);

    if (cont->kbuf != NULL) {
        err = frame_free((seL4_Word)cont->kbuf);
        assert(!err);
    }
    free(cont);
}

typedef struct {
    seL4_CPtr reply_cap;
} cont_getdirent_t;

static void serv_sys_getdirent_end(void *token, int err, size_t size) {
    cont_getdirent_t *cont = (cont_getdirent_t*)token;

    seL4_MessageInfo_t reply = seL4_MessageInfo_new(err, 0, 0, 1);
    seL4_SetMR(0, (seL4_Word)size);
    seL4_Send(cont->reply_cap, reply);
    cspace_free_slot(cur_cspace, cont->reply_cap);

    free(cont);
}

void serv_sys_getdirent(seL4_CPtr reply_cap, int pos, char* name, size_t nbyte){
    uint32_t permissions = 0;

    cont_getdirent_t *cont = malloc(sizeof(cont_getdirent_t));
    if (cont == NULL) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(ENOMEM, 0, 0, 1);
        seL4_SetMR(0, (seL4_Word)0);
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }
    cont->reply_cap = reply_cap;

    bool is_inval = !as_is_valid_memory(proc_getas(), (seL4_Word)name, sizeof(sos_stat_t), &permissions);
    is_inval = is_inval || !(permissions & seL4_CanWrite);

    if(is_inval) {
        serv_sys_getdirent_end((void*)cont, EINVAL, 0);
        return;
    }

    struct vnode *vn = vfs_vnt_lookup("/");
    if (vn == NULL) {
        serv_sys_getdirent_end((void*)cont, ENOMEM, 0);
        return;
    }

    VOP_GETDIRENT(vn, name, nbyte, pos, serv_sys_getdirent_end, (void*)cont);
}

typedef struct {
    seL4_CPtr reply_cap;
    char *kbuf;
    sos_stat_t *buf;
    size_t path_len;
} cont_stat_t;

static void serv_sys_stat_end(void *token, int err){
    cont_stat_t *cont = (cont_stat_t*)token;
    assert(cont != NULL);

    if (cont->kbuf != NULL) {
        free(cont->kbuf);
    }

    /* reply sosh*/
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(err, 0, 0, 0);
    seL4_Send(cont->reply_cap, reply);
    cspace_free_slot(cur_cspace, cont->reply_cap);

    free(cont);
}

static void
serv_sys_stat_copyin_cb(void *token, int err) {
    if (err) {
        serv_sys_stat_end(token, err);
        return;
    }
    cont_stat_t *cont = (cont_stat_t*)token;

    cont->kbuf[cont->path_len] = '\0';

    vfs_stat(cont->kbuf, cont->path_len, cont->buf, serv_sys_stat_end, (void*)cont);
}

void serv_sys_stat(seL4_CPtr reply_cap, char *path, size_t path_len, sos_stat_t *buf){
    /* Read doesn't check buffer if mapped like open & write,
     * just check if the memory is valid. It will map page when copyout */
    int err = 0;

    cont_stat_t *cont = malloc(sizeof(cont_stat_t));
    if(cont == NULL){
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(ENOMEM, 0, 0, 0);
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }
    cont->reply_cap = reply_cap;
    cont->buf       = buf;
    cont->path_len  = path_len;
    cont->kbuf      = NULL;

    uint32_t permissions = 0;
    if(!as_is_valid_memory(proc_getas(), (seL4_Word)buf, sizeof(sos_stat_t), &permissions) ||
            !(permissions & seL4_CanWrite)){
        serv_sys_stat_end((void*)cont, EINVAL);
        return;
    }

    if(!as_is_valid_memory(proc_getas(), (seL4_Word)path, path_len, &permissions) ||
            !(permissions & seL4_CanRead)){
        serv_sys_stat_end((void*)cont, EINVAL);
        return;
    }

    cont->kbuf = malloc(MAX_NAME_LEN + 1);
    if (cont->kbuf == NULL) {
        serv_sys_stat_end((void*)cont, ENOMEM);
        return;
    }

    err = copyin((seL4_Word)cont->kbuf, (seL4_Word)path, path_len,
            serv_sys_stat_copyin_cb, (void*)cont);
    if (err) {
        serv_sys_stat_end((void*)cont, err);
        return;
    }
}
