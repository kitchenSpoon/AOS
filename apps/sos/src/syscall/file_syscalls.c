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

static int
_sys_read(seL4_CPtr reply_cap, int fd, seL4_Word buf, size_t nbyte){
    if (fd < 0 || fd >= PROCESS_MAX_FILES) {
        return EINVAL;
    }
    /* Read doesn't check buffer if mapped like open & write,
     * just check if the memory is valid. It will map page when copyout */
    uint32_t permissions = 0;
    if(!as_is_valid_memory(proc_getas(), buf, nbyte, &permissions) ||
            !(permissions & seL4_CanWrite)){
        return EINVAL;
    }

    int err;
    struct openfile *file;

    err = filetable_findfile(fd, &file);
    if (err) {
        return err;
    }

    //check read permissions
    if(file->of_accmode != O_RDWR &&
        file->of_accmode != O_RDONLY){
        return EACCES;
    }
    err = VOP_READ(file->of_vnode, (char*)buf, nbyte, reply_cap);
    if (err) {
        return err;
    }

    return 0;
}

static int
_sys_write(int fd, seL4_Word buf, size_t nbyte, size_t* len){
    if (fd < 0 || fd >= PROCESS_MAX_FILES || nbyte >= MAX_IO_BUF) {
        return EINVAL;
    }
    if(!is_range_mapped(buf, nbyte)){
        return EINVAL;
    }

    int err;
    struct openfile *file;
    char kbuf[MAX_IO_BUF];

    err = copyin((seL4_Word)kbuf, (seL4_Word)buf, nbyte);
    if (err) {
        return EINVAL;
    }

    err = filetable_findfile(fd, &file);
    if (err) {
        return err;
    }

    //check write permissions
    if(file->of_accmode != O_RDWR &&
        file->of_accmode != O_WRONLY){
        return EACCES;
    }

    err = VOP_WRITE(file->of_vnode, kbuf, nbyte, len);
    if (err) {
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

void serv_sys_read(seL4_CPtr reply_cap, int fd, seL4_Word buf, size_t nbyte){
    int err;
    seL4_MessageInfo_t reply;

    err = _sys_read(reply_cap, fd, buf, nbyte);
    if (err) {
        reply = seL4_MessageInfo_new(err, 0, 0, 1);
        seL4_SetMR(0, (seL4_Word)-1); // this value can be anything
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
    }
}


void serv_sys_write(seL4_CPtr reply_cap, int fd, seL4_Word buf,
                    size_t nbyte) {

    size_t len;
    int err;
    seL4_MessageInfo_t reply;

    err = _sys_write(fd, buf, nbyte, &len);
    reply = seL4_MessageInfo_new(err, 0, 0, 1);
    seL4_SetMR(0, (seL4_Word)len);
    seL4_Send(reply_cap, reply);
    cspace_free_slot(cur_cspace, reply_cap);
}

void serv_sys_getdirent(int pos, char* name, size_t nbyte){
    //find vnode
    vops->getdirent();
}

void serv_sys_stat(char *path, sos_stat_t *buf){
    //we store stat with our vnode so we dont need to deal with nfs
    //loop through our vnode list 
    //
    vn = that vnode;

    copyout();

    seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
    seL4_SetMR(0, (seL4_Word)sent);
    seL4_Send(reply_cap, reply);
    cspace_free_slot(cur_cspace, reply_cap);
}
