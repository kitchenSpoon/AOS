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

static int
_sys_open(seL4_CPtr reply_cap, seL4_Word path, size_t nbyte, uint32_t flags, int* fd) {
    if (nbyte >= MAX_IO_BUF) {
        return EINVAL;
    }
    if (!is_range_mapped(path, nbyte)){
        return EINVAL;
    }

    int err;
    char kbuf[MAX_IO_BUF];
    err = copyin((seL4_Word)kbuf, (seL4_Word)path, (size_t)nbyte);
    if (err) {
        return err;
    }

    size_t len;
    for(len = 0; len < nbyte && kbuf[len]!='\0'; len++);
    if (len != nbyte) {
        return EINVAL;
    }
    kbuf[len] = '\0';

    err = file_open(kbuf, (int)flags, fd, reply_cap);
    if(err) {
        return err;
    }
    return 0;
}

static int
_sys_close(int fd) {
    if (fd < 0 || fd >= PROCESS_MAX_FILES) {
        return EINVAL;
    }

    int err = file_close(fd);
    if(err) {
        return err;
    }
    return 0;
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

void serv_sys_open(seL4_CPtr reply_cap, seL4_Word path, size_t nbyte, uint32_t flags){
    int fd;
    int err;

    err = _sys_open(reply_cap, path, nbyte, flags, &fd);
    if (err) {
        seL4_MessageInfo_t reply;
        reply = seL4_MessageInfo_new(err, 0, 0, 1);
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
    }

    printf("serv_sys_open\n");
}

void serv_sys_close(seL4_CPtr reply_cap, int fd){
    int err;
    seL4_MessageInfo_t reply;

    err = _sys_close(fd);

    reply = seL4_MessageInfo_new(err, 0, 0, 0);
    seL4_Send(reply_cap, reply);
    cspace_free_slot(cur_cspace, reply_cap);
}

typedef struct {
    seL4_CPtr reply_cap;
    struct openfile *file;
} cont_read_t;

void serv_sys_read_end(void *token, int err, size_t size){
    cont_read_t *cont = (cont_read_t*)token;

    /* Update file offset */
    if(!err){
        cont->file->of_offset += size;
    }

    /* Reply app*/
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(err, 0, 0, 1);
    seL4_SetMR(0, (seL4_Word)size);
    seL4_Send(cont->reply_cap, reply);
    cspace_free_slot(cur_cspace, cont->reply_cap);

    if(cont->file != NULL){
        free(cont->file);
    }
    free(cont);
}

void serv_sys_read(seL4_CPtr reply_cap, int fd, seL4_Word buf, size_t nbyte){
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

    bool is_inval = (fd < 0) || (fd >= PROCESS_MAX_FILES) || (nbyte >= MAX_IO_BUF);
    uint32_t permissions = 0;
    is_inval = is_inval || (!as_is_valid_memory(proc_getas(), buf, nbyte, &permissions));
    is_inval = is_inval || (!(permissions & seL4_CanWrite));
    if(is_inval){
        serv_sys_read_end((void*)cont, EINVAL, 0);
    }

    struct openfile *file;
    err = filetable_findfile(fd, &file);
    if (err) {
        serv_sys_read_end((void*)cont, EINVAL, 0);
    }
    cont->file = file;

    //check read permissions
    if(file->of_accmode != O_RDWR &&
        file->of_accmode != O_RDONLY){
        serv_sys_read_end((void*)cont, EACCES, 0);
    }

    VOP_READ(file->of_vnode, (char*)buf, nbyte, 0, serv_sys_read_end, (void*)cont);
}

typedef struct {
    seL4_CPtr reply_cap;
    struct openfile *file;
} cont_write_t;


void serv_sys_write_end(void *token, int err, size_t size){
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

    if(cont->file != NULL){
        free(cont->file);
    }
    free(cont);
}

void serv_sys_write(seL4_CPtr reply_cap, int fd, seL4_Word buf, size_t nbyte) {

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
    }

    struct openfile *file;
    char kbuf[MAX_IO_BUF];

    err = copyin((seL4_Word)kbuf, (seL4_Word)buf, nbyte);
    if (err) {
        serv_sys_write_end((void*)cont, EINVAL, 0);
    }

    err = filetable_findfile(fd, &file);
    if (err) {
        serv_sys_write_end((void*)cont, EINVAL, 0);
    }
    cont->file = file;

    //check write permissions
    if(file->of_accmode != O_RDWR &&
        file->of_accmode != O_WRONLY){
        serv_sys_write_end((void*)cont, EACCES, 0);
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

//void serv_sys_getdirent_ret(seL4_CPtr

void serv_sys_stat(seL4_CPtr reply_cap, char *path, size_t path_len, sos_stat_t *buf){
    //TODO: just making it compiles
    return;

//    /* Read doesn't check buffer if mapped like open & write,
//     * just check if the memory is valid. It will map page when copyout */
//    uint32_t permissions = 0;
//    if(!as_is_valid_memory(proc_getas(), (seL4_Word)buf, sizeof(sos_stat_t), &permissions) ||
//            !(permissions & seL4_CanWrite)){
//        return EINVAL;
//    }
//
//    //check me
//    if(!as_is_valid_memory(proc_getas(), (seL4_Word)path, path_len, &permissions) ||
//            !(permissions & seL4_CanRead)){
//        return EINVAL;
//    }
//    //we store stat with our vnode so we dont need to deal with nfs
//    //loop through our vnode list
//    vn = vfs_vt_lookup(path);
//    if(vn == NULL){
//        //error
//    }
//
//    VOP_STAT(vn, buf);
}
