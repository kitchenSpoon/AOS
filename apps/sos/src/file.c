#include <assert.h>
#include <syscall.h>

#include <stdio.h>
#include <assert.h>
#include <strings.h>
#include <limits.h>
#include <sel4/sel4.h>
#include <cspace/cspace.h>
#include <ut_manager/ut.h>
#include <errno.h>

#include "vm.h"
#include "mapping.h"
#include "vmem_layout.h"
#include "utility.h"

int serv_sys_open(seL4_Word path, seL4_Word flags){
    assert(!"boo");
    //path is a address
    //translate path
    seL4_Word kvaddr;
    sos_get_kvaddr(proc_getas(), path, &kvaddr);
    char *kpath = (char*)kvaddr;
    //read
    //while(kpath[i]!='\0' && i < MAX_IO_BUFF){
    //
    //}
    
    //if(kpath == "console"){
    //  create_open_file_handler();
    //} else {
    //  //not implemented yet
    //}
    return -1;
}


fildes_t sos_sys_close(seL4_Word fd){
    assert(!"boo");
    
    return -1;
}

fildes_t sos_sys_open(const char*path, int flags){
    assert(!"boo");
    return -1;
}
