#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <sel4/sel4.h>
#include <serial/serial.h>

#include "utility.h"
#include "addrspace.h"
#include "proc.h"
#include "syscall.h"
#include "copyinout.h"

#define MAX_SERIAL_TRY  0x100
#define MAX_IO_BUF      0x1000

/*
 * Check if the user pages from VADDR to VADDR+NBYTE are mapped
 */
static bool
validate_user_mem(seL4_Word vaddr, size_t nbyte) {
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


int serv_sys_print(char* message, size_t len, size_t *sent) {
    struct serial* serial = serial_init(); //serial_init does the cacheing

    *sent = 0;
    int tries = 0;
    while (*sent < len && tries < MAX_SERIAL_TRY) {
        *sent += serial_send(serial, message+*sent, len-*sent);
        tries++;
    }
    return 0;
}

int serv_sys_open(seL4_Word path, size_t nbyte, uint32_t flags, int* fd){
    if (nbyte >= MAX_IO_BUF) {
        return EINVAL;
    }
    if (!validate_user_mem(path, nbyte)){
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

    err = file_open(kbuf, (int)flags, fd);
    if(err) {
        return err;
    }

    return 0;
}

int serv_sys_close(int fd){
    if (fd < 0 || fd >= PROCESS_MAX_FILES) {
        return EINVAL;
    }
    int err = file_close(fd);
    if(err) {
        return err;
    }
    return 0;
}

int serv_sys_read(int fd, seL4_Word buf, size_t nbyte, size_t* len){
    if (fd < 0 || fd >= PROCESS_MAX_FILES || nbyte >= MAX_IO_BUF) {
        return EINVAL;
    }
    if(!validate_user_mem(buf, nbyte)){
        return EINVAL;
    }

    int err;
    struct openfile *file;
    char kbuf[MAX_IO_BUF];

    err = filetable_findfile(fd, &file);
    if (err) {
        return err;
    }

    err = file->of_vnode->vn_ops->vop_read(file->of_vnode, kbuf, nbyte, len);
    if (err) {
        return err;
    }

    err = copyout((seL4_Word)buf, (seL4_Word)kbuf, *len);
    if (err) {
        return err;
    }

    return 0;
}


int serv_sys_write(int fd, seL4_Word buf, size_t nbyte, size_t* len){
    if (fd < 0 || fd >= PROCESS_MAX_FILES || nbyte >= MAX_IO_BUF) {
        return EINVAL;
    }
    if(!validate_user_mem(buf, nbyte)){
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

    err = file->of_vnode->vn_ops->vop_write(file->of_vnode, kbuf, nbyte, len);
    //VOP_WRITE(file->of_vnode, kbuf, nbyte, len);
    if (err) {
        return err;
    }

    return 0;
}
