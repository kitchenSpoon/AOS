#include <syscall/syscall.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <proc/proc.h>
#include <errno.h>

#include "vm/copyinout.h"

typedef struct{
    char* kpath;
    size_t len;
    seL4_CPtr fault_ep;
    seL4_CPtr reply_cap;
} serv_proc_create_cont_t;

void serv_proc_create_end(void* token, int err, int pid){
    printf("serv_proc_create_end\n");
    serv_proc_create_cont_t* cont = (serv_proc_create_cont_t*)token;

    seL4_MessageInfo_t reply;
    reply = seL4_MessageInfo_new(err, 0, 0, 1);
    seL4_SetMR(0, pid);
    seL4_Send(cont->reply_cap, reply);
    cspace_free_slot(cur_cspace, cont->reply_cap);

    if (cont->kpath) free(cont->kpath);
    free(cont);
}

void serv_proc_create_part2(void* token, int err){
    serv_proc_create_cont_t* cont = (serv_proc_create_cont_t*)token;
    if(err){
        serv_proc_create_end(token, err, -1);
    } else {
        cont->kpath[cont->len] = '\0';
        proc_create(cont->kpath, cont->fault_ep, serv_proc_create_end, (void*)cont);
    }
}

void serv_proc_create(char* path, size_t len, seL4_CPtr fault_ep, seL4_CPtr reply_cap){

    printf("serv_proc_create entered len = %d\n",len);
    serv_proc_create_cont_t* cont = malloc(sizeof(serv_proc_create_cont_t));
    if(cont == NULL){
        printf("serv_proc_create, err no mem for continuation\n");
        seL4_MessageInfo_t reply;
        reply = seL4_MessageInfo_new(ENOMEM, 0, 0, 1);
        seL4_SetMR(0, -1);
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }
    cont->kpath     = NULL;
    cont->len       = len;
    cont->fault_ep  = fault_ep;
    cont->reply_cap = reply_cap;

    uint32_t permissions = 0;
    if(!as_is_valid_memory(proc_getas(), (seL4_Word)path, len, &permissions) ||
            !(permissions & seL4_CanRead)){
        printf("serv_proc_create, no permission to read path\n");
        serv_proc_create_end((void*)cont, EINVAL, -1);
        return;
    }

    //copyin path;
    cont->kpath = malloc(len+1);
    printf("serv_proc_create, kpath = 0x%08x\n",(seL4_Word)cont->kpath);
    if(cont->kpath == NULL){
        printf("serv_proc_create, no memory for path\n");
        serv_proc_create_end((void*)cont, ENOMEM, -1);
        return;
    }

    int err = copyin((seL4_Word)cont->kpath, (seL4_Word)path, len,
            serv_proc_create_part2, (void*)cont);
    if(err){
        printf("serv_proc_create, copyin fails\n");
        serv_proc_create_end((void*)cont, err, -1);
        return;
    }
}

typedef struct{
    seL4_CPtr reply_cap;
} serv_proc_destroy_cont_t;

void serv_proc_destroy(int pid, seL4_CPtr reply_cap){
    //serv_proc_destroy_cont_t* cont = malloc(sizeof(serv_proc_destroy_cont_t));
    /*if(cont == NULL){
        printf("serv_proc_destroy, no memory\n");
        return;
    }*/
    int err = proc_destroy(pid);

    //reply
    seL4_MessageInfo_t reply;
    reply = seL4_MessageInfo_new(err, 0, 0, 0);
    //seL4_SetMR(0, 0);
    seL4_Send(reply_cap, reply);
    cspace_free_slot(cur_cspace, reply_cap);
    return;
}

void serv_proc_get_id(){
    int id = proc_get_id();
    //reply
    (void)id;
    return;
}

void serv_proc_wait(int id, seL4_CPtr reply_cap){
    (void)id;
    printf("serv_proc_wait\n");
    //check if process id is valid
    //proc_is_valid(id);

    //if it is then call
    //proc_add_wait(id);
}

void serv_proc_status(void){

}
