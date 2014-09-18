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

#define MAX_SERIAL_TRY  0x100
#define MAX_IO_BUF      0x1000

/*
 * Check if the user pages from VADDR to VADDR+NBYTE are mapped
 */
static bool
is_range_mapped(seL4_Word vaddr, size_t nbyte) {
    seL4_Word vpage = PAGE_ALIGN(vaddr);
    while (vpage < vaddr+nbyte) {
        bool mapped = sos_page_is_mapped(proc_getas(), vpage);
        if (!mapped) {
            return false;
        }
        vpage += PAGE_SIZE;
    }
    return true;
}

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
} cont_open_t;

void serv_sys_open_end(void *token, int err, int fd) {
    printf("serv_sys_open_end called\n");
    cont_open_t *cont = (cont_open_t*)token;

    seL4_MessageInfo_t reply;
    reply = seL4_MessageInfo_new(err, 0, 0, 1);
    seL4_SetMR(0, (seL4_Word)fd);
    seL4_Send(cont->reply_cap, reply);
    cspace_free_slot(cur_cspace, cont->reply_cap);
    //printf("--serv_cont_end = %p\n", cont);

    //printf("token at at %p\n", cont);
    free(cont);
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

    if ((nbyte >= MAX_IO_BUF) || (!is_range_mapped(path, nbyte))){
        serv_sys_open_end((void*)cont, EINVAL, -1);
        return;
    }

    int err;
    char kbuf[MAX_IO_BUF];
    err = copyin((seL4_Word)kbuf, (seL4_Word)path, (size_t)nbyte);
    if (err) {
        serv_sys_open_end((void*)cont, err, -1);
        return;
    }

    size_t len;
    for(len = 0; len < nbyte && kbuf[len]!='\0'; len++);
    if (len != nbyte) {
        serv_sys_open_end((void*)cont, EINVAL, -1);
        return;
    }
    kbuf[len] = '\0';

    file_open(kbuf, (int)flags, serv_sys_open_end, (void*)cont);
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
} cont_read_t;

void serv_sys_read_end(void *token, int err, size_t size){
    printf("serv read end\n");
    cont_read_t *cont = (cont_read_t*)token;

    printf("serv_read_end 0.25\n");

    /* Update file offset */
    if(!err){
        printf("serv_read_end 0.5\n");
        cont->file->of_offset += size;
    }

    printf("serv_read_end 1\n");
    /* Reply app*/
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(err, 0, 0, 1);
    printf("serv_read_end size = %u\n", size);
    seL4_SetMR(0, (seL4_Word)size);
    seL4_Send(cont->reply_cap, reply);
    cspace_free_slot(cur_cspace, cont->reply_cap);

    free(cont);
    printf("serv_read_end out\n");
}

void serv_sys_read(seL4_CPtr reply_cap, int fd, seL4_Word buf, size_t nbyte){
    printf("serv read\n");
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

    printf("serv read2\n");
    //have to read multiple times if nbyte >= MAX_IO_BUFF
    bool is_inval = (fd < 0) || (fd >= PROCESS_MAX_FILES);// || (nbyte >= MAX_IO_BUF);
    if(is_inval){
        printf("err1.8\n");}
    uint32_t permissions = 0;
    is_inval = is_inval || (!as_is_valid_memory(proc_getas(), buf, nbyte, &permissions));
    if(is_inval){
        printf("err1.9\n");}
    is_inval = is_inval || (!(permissions & seL4_CanWrite));
    if(is_inval){
        printf("err2\n");
        serv_sys_read_end((void*)cont, EINVAL, 0);
        return;
    }

    printf("serv read3\n");
    struct openfile *file;
    err = filetable_findfile(fd, &file);
    if (err) {
        printf("err3\n");
        serv_sys_read_end((void*)cont, EINVAL, 0);
        return;
    }
    cont->file = file;

    printf("serv read4\n");
    //check read permissions
    if(file->of_accmode != O_RDWR &&
        file->of_accmode != O_RDONLY){
        printf("err4\n");
        serv_sys_read_end((void*)cont, EACCES, 0);
        return;
    }

    printf("serv read5\n");
    VOP_READ(file->of_vnode, (char*)buf, nbyte, file->of_offset, serv_sys_read_end, (void*)cont);
    printf("serv read finish\n");
}

typedef struct {
    seL4_CPtr reply_cap;
    struct openfile *file;
} cont_write_t;


void serv_sys_write_end(void *token, int err, size_t size){
    printf("serv_write_end\n");
    cont_write_t *cont = (cont_write_t*)token;

    /* Update file offset */
    if(!err){
        cont->file->of_offset += size;
    }

    /* Reply app*/
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(err, 0, 0, 1);
    seL4_SetMR(0, (seL4_Word)size);
    seL4_Send(cont->reply_cap, reply);
    cspace_free_slot(cur_cspace, cont->reply_cap);

    free(cont);
}

void serv_sys_write(seL4_CPtr reply_cap, int fd, seL4_Word buf, size_t nbyte) {

    printf("write\n");
    int err;

    cont_write_t *cont = malloc(sizeof(cont_write_t));
    if (cont == NULL) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(ENOMEM, 0, 0, 1);
        seL4_SetMR(0, (seL4_Word)0);
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }
    cont->reply_cap = reply_cap;
    cont->file = NULL;

    bool is_inval = (fd < 0) || (fd >= PROCESS_MAX_FILES) || (nbyte >= MAX_IO_BUF);
    is_inval = is_inval || (!is_range_mapped(buf, nbyte));
    if(is_inval){
        serv_sys_write_end((void*)cont, EINVAL, 0);
        return;
    }

    struct openfile *file;
    char kbuf[MAX_IO_BUF];

    err = copyin((seL4_Word)kbuf, (seL4_Word)buf, nbyte);
    if (err) {
        serv_sys_write_end((void*)cont, EINVAL, 0);
        return;
    }

    err = filetable_findfile(fd, &file);
    if (err) {
        serv_sys_write_end((void*)cont, EINVAL, 0);
        return;
    }
    cont->file = file;

    //check write permissions
    if(file->of_accmode != O_RDWR &&
        file->of_accmode != O_WRONLY){
        serv_sys_write_end((void*)cont, EACCES, 0);
        return;
    }


    VOP_WRITE(file->of_vnode, kbuf, 0, nbyte, serv_sys_write_end, (void*)cont);
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
} cont_stat_t;

static void serv_sys_stat_end(void *token, int err){
    cont_stat_t *cont = (cont_stat_t*)token;

    /* reply sosh*/
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(err, 0, 0, 0);
    seL4_Send(cont->reply_cap, reply);
    cspace_free_slot(cur_cspace, cont->reply_cap);

    free(cont);
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

    char kbuf[MAX_IO_BUF];
    err = copyin((seL4_Word)kbuf, (seL4_Word)path, path_len);
    if (err) {
        serv_sys_stat_end((void*)cont, err);
        return;
    }
    kbuf[path_len] = '\0';

    vfs_stat(kbuf, path_len, buf, serv_sys_stat_end, (void *)cont);
}
