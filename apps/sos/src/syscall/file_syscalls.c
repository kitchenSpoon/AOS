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
#include "vm/swap.h"
#include "syscall/file.h"

#define verbose 0
#include <sys/debug.h>

#define MAX_SERIAL_TRY  0x100
#define MAX_IO_BUF      0x1000

/**********************************************************************
 * Server Print
 **********************************************************************/

void serv_sys_print(seL4_CPtr reply_cap, char* message, size_t len) {
    struct serial* serial = serial_init(); //serial_init does the cacheing

    size_t sent = 0;
    int tries = 0;
    while (sent < len && tries < MAX_SERIAL_TRY) {
        sent += serial_send(serial, message+sent, len-sent);
        tries++;
    }

    set_cur_proc(PROC_NULL);
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
    seL4_SetMR(0, (seL4_Word)sent);
    seL4_Send(reply_cap, reply);
    cspace_free_slot(cur_cspace, reply_cap);
}

/**********************************************************************
 * Server File Open
 **********************************************************************/

typedef struct {
    seL4_CPtr reply_cap;
    char *kbuf;
    size_t nbyte;
    uint32_t flags;
    pid_t pid;
} cont_open_t;

static void serv_sys_open_copyin_cb(void *token, int err);
static void serv_sys_open_end(void *token, int err, int fd);

void serv_sys_open(seL4_CPtr reply_cap, seL4_Word path, size_t nbyte, uint32_t flags){
    dprintf(3, "serv_sys_open called\n");
    cont_open_t *cont = malloc(sizeof(cont_open_t));
    //dprintf(3, "--serv_cont = %p, size = %u\n", cont, sizeof(cont_open_t));
    if (cont == NULL) {
        set_cur_proc(PROC_NULL);
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
    cont->pid       = proc_get_id();

    addrspace_t *as = proc_getas();
    if ((nbyte > MAX_NAME_LEN) || (!as_is_valid_memory(as, path, nbyte, NULL))){
        serv_sys_open_end((void*)cont, EINVAL, -1);
        return;
    }

    cont->kbuf = malloc(MAX_NAME_LEN+1);
    if (cont->kbuf == NULL) {
        dprintf(3, "serv_sys_open: nomem for kbuf\n");
        serv_sys_open_end((void*)cont, ENOMEM, -1);
        return;
    }

    int err;
    err = copyin((seL4_Word)cont->kbuf, (seL4_Word)path, (size_t)nbyte,
            serv_sys_open_copyin_cb, (void*)cont);
    if (err) {
        dprintf(3, "serv_sys_open: err when copyin\n");
        serv_sys_open_end((void*)cont, err, -1);
        return;
    }
}

static void
serv_sys_open_copyin_cb(void *token, int err) {
    if (err) {
        serv_sys_open_end(token, err, -1);
        return;
    }

    cont_open_t *cont = (cont_open_t*)token;
    cont->kbuf[cont->nbyte] = '\0';

    if (strcmp(cont->kbuf, SWAP_FILE_NAME) == 0) {
        serv_sys_open_end((void*)cont, EINVAL, -1);
        return;
    }

    file_open(cont->kbuf, (int)cont->flags, serv_sys_open_end, (void*)cont);
}

static void
serv_sys_open_end(void *token, int err, int fd) {
    dprintf(3, "serv_sys_open_end called\n");
    cont_open_t *cont = (cont_open_t*)token;
    assert(cont != NULL);

    if (!is_proc_alive(cont->pid)) {
        dprintf(3, "serv_sys_open_end: proc is killed\n");
        cspace_free_slot(cur_cspace, cont->reply_cap);
        free(cont);
        return;
    }
    set_cur_proc(PROC_NULL);
    if (cont->kbuf == NULL) {
        free(cont->kbuf);
    }

    if(err){
        dprintf(3, "serv_sys_open err = %d\n", err);
    }

    seL4_MessageInfo_t reply;
    reply = seL4_MessageInfo_new(err, 0, 0, 1);
    seL4_SetMR(0, (seL4_Word)fd);
    seL4_Send(cont->reply_cap, reply);
    cspace_free_slot(cur_cspace, cont->reply_cap);
    dprintf(3, "--serv_cont_end = %p\n", cont);

    free(cont);
}

/**********************************************************************
 * Server File Close
 **********************************************************************/

void serv_sys_close(seL4_CPtr reply_cap, int fd){
    int err = 0;
    //dprintf(3, "fd = %d\n", fd);
    if (fd < 0 || fd >= PROCESS_MAX_FILES) {
        err = EINVAL;
    }

    err = err || file_close(fd);

    set_cur_proc(PROC_NULL);
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(err, 0, 0, 0);
    seL4_Send(reply_cap, reply);
    cspace_free_slot(cur_cspace, reply_cap);
}

/**********************************************************************
 * Server File Read
 **********************************************************************/

typedef struct {
    seL4_CPtr reply_cap;
    struct openfile *file;
    size_t bytes_read;
    size_t bytes_wanted;
    char* buf;
    pid_t pid;
} cont_read_t;

void serv_sys_read_end(void *token, int err, size_t size, bool more_to_read);

void serv_sys_read(seL4_CPtr reply_cap, int fd, seL4_Word buf, size_t nbyte){
    //dprintf(3, "serv read\n");
    int err;
    cont_read_t *cont = malloc(sizeof(cont_read_t));
    if (cont == NULL) {
        set_cur_proc(PROC_NULL);
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
    cont->pid = proc_get_id();

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

    VOP_READ(file->of_vnode, (char*)buf, MIN(nbyte, MAX_IO_BUF),
            file->of_offset, serv_sys_read_end, (void*)cont);
    //dprintf(3, "serv read finish\n");
}

void serv_sys_read_end(void *token, int err, size_t size, bool more_to_read){
    //dprintf(3, "serv_read_end called\n");
    //dprintf(3, "serv_read_end size = %u\n", size);
    cont_read_t *cont = (cont_read_t*)token;

    if (!is_proc_alive(cont->pid)) {
        dprintf(3, "serv_sys_read_end: proc is killed\n");
        cspace_free_slot(cur_cspace, cont->reply_cap);
        free(cont);
        return;
    }

    /* Update file offset */
    if(!err){
        cont->file->of_offset += size;
        cont->bytes_read += size;
    }

    //dprintf(3, "serv_read_end bytes_read = %u, bytes_wanted = %u\n", cont->bytes_read, cont->bytes_wanted);
    if(err || !more_to_read || cont->bytes_read >= cont->bytes_wanted){
        /* Reply app*/
        set_cur_proc(PROC_NULL);
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
    //dprintf(3, "serv_read_end out\n");
}

/**********************************************************************
 * Server File Write
 **********************************************************************/

typedef struct {
    seL4_CPtr reply_cap;
    struct openfile *file;
    char *buf;
    char *kbuf;
    size_t nbyte;
    size_t byte_written;
    size_t wanna_send;
    pid_t pid;
} cont_write_t;

static void serv_sys_write_get_kbuf(void *token, seL4_Word kvaddr);
static void serv_sys_write_copyin(void *token, int err, size_t size);
static void serv_sys_write_do_write(void *token, int err);
static void serv_sys_write_end(cont_write_t* cont, int err);

void serv_sys_write(seL4_CPtr reply_cap, int fd, seL4_Word buf, size_t nbyte) {

    dprintf(3, "serv_sys_write called\n");
    int err;

    cont_write_t *cont = malloc(sizeof(cont_write_t));
    if (cont == NULL) {
        set_cur_proc(PROC_NULL);
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
    cont->byte_written  = 0;
    cont->wanna_send = 0;
    cont->pid        = proc_get_id();

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

    err = frame_alloc(0, NULL, PROC_NULL, true, serv_sys_write_get_kbuf, (void*)cont);
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
        dprintf(3, "serv_sys_write_get_kbuf: ENOMEM\n");
        serv_sys_write_end(cont, ENOMEM);
        return;
    }
    cont->kbuf = kbuf;
    serv_sys_write_copyin((void*)cont, 0, 0);
}

/* Is inteded to be called repeatedly in this layer if the write data is too large */
static void
serv_sys_write_copyin(void *token, int err, size_t size) {
    dprintf(3, "serv_sys_write_copyin called\n");
    cont_write_t *cont = (cont_write_t*)token;
    assert(cont != NULL);

    if (err) {
        serv_sys_write_end(cont, err);
        return;
    }

    cont->file->of_offset += size;
    cont->byte_written       += size;
    dprintf(3, "of_offset = %u, byte_written = %u\n", (unsigned int)cont->file->of_offset, cont->byte_written);
    if (cont->byte_written < cont->nbyte) {
        seL4_Word vaddr;
        vaddr = (seL4_Word)cont->buf + cont->byte_written;
        cont->wanna_send = PAGE_SIZE - (vaddr - PAGE_ALIGN(vaddr));
        cont->wanna_send = MIN(cont->wanna_send, cont->nbyte - cont->byte_written);
        dprintf(3, "wanna_send = %u\n", cont->wanna_send);

        err = copyin((seL4_Word)cont->kbuf, (seL4_Word)vaddr, cont->wanna_send,
                serv_sys_write_do_write, (void*)cont);
        if (err) {
            dprintf(3, "serv_sys_write_copyin: fail when copyin\n");
            serv_sys_write_end(cont, EINVAL);
            return;
        }
    } else {
        serv_sys_write_end(cont, 0);
        return;
    }
    dprintf(3, "serv_sys_write_copyin ended\n");
}

static void
serv_sys_write_do_write(void *token, int err) {
    dprintf(3, "serv_sys_do_write called\n");
    cont_write_t *cont = (cont_write_t*)token;
    assert(cont != NULL);

    if (err) {
        serv_sys_write_end(cont, err);
        return;
    }
    dprintf(3, "serv_sys_do_write prepare to write\n");
    VOP_WRITE(cont->file->of_vnode, cont->kbuf, cont->wanna_send, cont->file->of_offset,
            serv_sys_write_copyin, (void*)cont);
}

static
void serv_sys_write_end(cont_write_t* cont, int err) {
    dprintf(3, "serv_write_end\n");

    if (!is_proc_alive(cont->pid)) {
        dprintf(3, "serv_sys_write_end: proc is killed\n");
        if (cont->kbuf != NULL) {
            frame_free((seL4_Word)cont->kbuf);
        }

        cspace_free_slot(cur_cspace, cont->reply_cap);
        free(cont);
        return;
    }

    set_cur_proc(PROC_NULL);

    /* Reply app*/
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(err, 0, 0, 1);
    seL4_SetMR(0, (seL4_Word)cont->byte_written);
    seL4_Send(cont->reply_cap, reply);
    cspace_free_slot(cur_cspace, cont->reply_cap);

    if (cont->kbuf != NULL) {
        frame_free((seL4_Word)cont->kbuf);
    }
    free(cont);
}

/**********************************************************************
 * Server Getdirent
 **********************************************************************/

typedef struct {
    seL4_CPtr reply_cap;
    pid_t pid;
} cont_getdirent_t;

static void serv_sys_getdirent_end(void *token, int err, size_t size);

void serv_sys_getdirent(seL4_CPtr reply_cap, int pos, char* name, size_t nbyte){
    uint32_t permissions = 0;

    cont_getdirent_t *cont = malloc(sizeof(cont_getdirent_t));
    if (cont == NULL) {
        set_cur_proc(PROC_NULL);
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(ENOMEM, 0, 0, 1);
        seL4_SetMR(0, (seL4_Word)0);
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }
    cont->reply_cap = reply_cap;
    cont->pid       = proc_get_id();

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

static void serv_sys_getdirent_end(void *token, int err, size_t size) {
    cont_getdirent_t *cont = (cont_getdirent_t*)token;

    if (!is_proc_alive(cont->pid)) {
        dprintf(3, "serv_sys_getdirent_end: proc is killed\n");
        cspace_free_slot(cur_cspace, cont->reply_cap);
        free(cont);
        return;
    }

    set_cur_proc(PROC_NULL);
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(err, 0, 0, 1);
    seL4_SetMR(0, (seL4_Word)size);
    seL4_Send(cont->reply_cap, reply);
    cspace_free_slot(cur_cspace, cont->reply_cap);

    free(cont);
}

/**********************************************************************
 * Server File Stat
 **********************************************************************/

typedef struct {
    seL4_CPtr reply_cap;
    char *kbuf;
    sos_stat_t *buf;
    size_t path_len;
    pid_t pid;
} cont_stat_t;

static void serv_sys_stat_copyin_cb(void *token, int err);
static void serv_sys_stat_end(void *token, int err);

void serv_sys_stat(seL4_CPtr reply_cap, char *path, size_t path_len, sos_stat_t *buf){
    /* Read doesn't check buffer if mapped like open & write,
     * just check if the memory is valid. It will map page when copyout */
    int err = 0;
    dprintf(3, "serv_sys_stat called\n");
    cont_stat_t *cont = malloc(sizeof(cont_stat_t));
    if(cont == NULL){
        set_cur_proc(PROC_NULL);
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(ENOMEM, 0, 0, 0);
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }
    cont->reply_cap = reply_cap;
    cont->buf       = buf;
    cont->path_len  = path_len;
    cont->kbuf      = NULL;
    cont->pid       = proc_get_id();

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

static void
serv_sys_stat_copyin_cb(void *token, int err) {
    dprintf(3, "serv_sys_stat_cb\n");
    if (err) {
        serv_sys_stat_end(token, err);
        return;
    }
    dprintf(3, "serv_sys_stat_cb\n");
    cont_stat_t *cont = (cont_stat_t*)token;
    dprintf(3, "serv_sys_stat_cb\n");

    cont->kbuf[cont->path_len] = '\0';
    dprintf(3, "serv_sys_stat_cb\n");

    vfs_stat(cont->kbuf, cont->path_len, cont->buf, serv_sys_stat_end, (void*)cont);
}

static void serv_sys_stat_end(void *token, int err){
    dprintf(3, "serv_sys_stat_end\n");
    cont_stat_t *cont = (cont_stat_t*)token;
    assert(cont != NULL);

    if (!is_proc_alive(cont->pid)) {
        dprintf(3, "serv_sys_stat_end: proc is killed\n");
        cspace_free_slot(cur_cspace, cont->reply_cap);
        free(cont);
        return;
    }
    set_cur_proc(PROC_NULL);

    if (cont->kbuf != NULL) {
        free(cont->kbuf);
    }

    //set_cur_proc(PROC_NULL);

    /* reply sosh*/
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(err, 0, 0, 0);
    seL4_Send(cont->reply_cap, reply);
    cspace_free_slot(cur_cspace, cont->reply_cap);

    free(cont);
}
