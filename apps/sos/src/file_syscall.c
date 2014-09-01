#include <stdio.h>
#include <assert.h>
#include <sel4/sel4.h>
#include <limits.h>
#include <errno.h>

#include "utility.h"
#include "addrspace.h"
#include "proc.h"
#include "syscall.h"

#define MAX_IO_BUF 0x1000

static bool
check_range_page_mapped(seL4_Word vaddr, size_t nbyte) {
    seL4_Word vpage = PAGE_ALIGN(vaddr);
    while (vpage < vaddr+nbyte) {
        bool mapped = sos_page_is_mapped(proc_getas(), PAGE_ALIGN(vpage));
        if (!mapped) {
            return false;
        }
        vpage += PAGE_SIZE;
    }
    return true;
}

int serv_sys_open(seL4_Word path, seL4_Word flags, seL4_Word* fd, seL4_Word nbyte){
    //path is a address
    //translate path

    if(!check_range_page_mapped(path, nbyte)){
        return EINVAL;
    }

    seL4_Word kvaddr;
    int err = sos_get_kvaddr(proc_getas(), path, &kvaddr);
    if (err) {
        return err;
    }

    char* kpath = (char*) kvaddr;
    char kbuf[MAX_IO_BUF];
    int i = 0;
    for(i = 0; i < (int)nbyte && kpath[i]!='\0'; i++){
        kbuf[i] = kpath[i];
    }
    if (i != nbyte) {
        return EINVAL;
    }
    kbuf[i] = '\0';

    err = file_open(kbuf, (int)flags, (int*)fd);
    if(err)
        return err;

    return 0;
}

int serv_sys_close(seL4_Word fd){
    int err = file_close(fd);
    if(err)
        return err;
    return 0;
}

int serv_sys_read(seL4_Word fd, seL4_Word buf, seL4_Word nbyte, seL4_Word* len){
    //path is a address
    //translate path
    seL4_Word kvaddr;
    int err;
    err = sos_get_kvaddr(proc_getas(), buf, &kvaddr);
    if (err) {
        return err;
    }
    if (nbyte > MAX_IO_BUF) {
        return EINVAL;
    }

    char kbuf[MAX_IO_BUF];

    if(!check_range_page_mapped(buf, nbyte)){
        return EINVAL;
    }

    //check if fd is valid
    struct openfile *file;
    err = filetable_findfile(fd, &file);
    if (err) {
        return err;
    }

    file->of_vnode->vn_ops->vop_read(file->of_vnode, kbuf, nbyte, len);

    //copy mem from kbuf out to kvaddr page by page

    return 0;
}


int serv_sys_write(seL4_Word fd, seL4_Word buf, seL4_Word nbyte, seL4_Word* len){
    //path is a address
    //translate path
    seL4_Word kvaddr;
    int err;
    err = sos_get_kvaddr(proc_getas(), buf, &kvaddr);
    if (err) {
        return err;
    }
    if (nbyte > MAX_IO_BUF) {
        return EINVAL;
    }

    char kbuf[MAX_IO_BUF];

    if(!check_range_page_mapped(buf, nbyte)){
        return EINVAL;
    }
    //copy from buf to kbuf

    //check if fd is valid
    struct openfile *file;
    err = filetable_findfile(fd, &file);
    if (err) {
        return err;
    }

    file->of_vnode->vn_ops->vop_write(file->of_vnode, kbuf, nbyte, len);

    return 0;
}
