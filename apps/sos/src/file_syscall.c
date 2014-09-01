#include <stdio.h>
#include <assert.h>
#include <sel4/sel4.h>

#include "addrspace.h"
#include "proc.h"
#include "syscall.h"

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

/*
int sos_sys_close(seL4_Word fd){
    assert(!"boo");
    
    return -1;
}

int sos_sys_open(const char*path, int flags){
    assert(!"boo");
    return -1;
}*/


