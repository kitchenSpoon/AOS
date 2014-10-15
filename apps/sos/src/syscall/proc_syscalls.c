#include <syscall/syscall.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <proc/proc.h>
#include <errno.h>

#include "vm/copyinout.h"

typedef struct{
    seL4_CPtr reply_cap;
    char* kapp_name;
    size_t len;
    seL4_CPtr fault_ep;
} serv_proc_create_cont_t;

void serv_proc_create_end(void* token, int err, int id){
    printf("serv_proc_create_end\n");
    serv_proc_create_cont_t* cont = (serv_proc_create_cont_t*)token;

    seL4_MessageInfo_t reply;
    reply = seL4_MessageInfo_new(err, 0, 0, 1);
    seL4_SetMR(0, id);
    seL4_Send(cont->reply_cap, reply);
    cspace_free_slot(cur_cspace, cont->reply_cap);

    free(cont->kapp_name);
    free(cont);
}

void serv_proc_create_part2(void* token, int err){
    serv_proc_create_cont_t* cont = (serv_proc_create_cont_t*)token;
    if(err){
        serv_proc_create_end(token, 0, err);
    } else {
        cont->kapp_name[cont->len] = '\0';
        proc_create(cont->kapp_name, cont->fault_ep, serv_proc_create_end, (void*)cont);
    }
}

int serv_proc_create(char* app_name, size_t len, seL4_CPtr fault_ep, seL4_CPtr reply_cap){

    printf("serv_proc_create entered len = %d\n",len);
    serv_proc_create_cont_t* cont = malloc(sizeof(serv_proc_create_cont_t));
    if(cont == NULL){
        printf("serv_proc_create, no memory for cont\n");
        return 1;
    }
    cont->fault_ep = fault_ep;
    cont->reply_cap = reply_cap;

    uint32_t permissions = 0;
    if(!as_is_valid_memory(proc_getas(), (seL4_Word)app_name, len, &permissions) ||
            !(permissions & seL4_CanRead)){
        printf("serv_proc_create, no permission to read name\n");
        return EFAULT;
    }

    //copyin app_name;
    cont->kapp_name = malloc(len+1);
    printf("0x%08x\n",(seL4_Word)cont->kapp_name);
    if(cont->kapp_name == NULL){
        printf("serv_proc_create, no memory for name\n");
        return ENOMEM;
    }

    cont->len = len;

    int err = copyin((seL4_Word)cont->kapp_name, (seL4_Word)app_name, len, serv_proc_create_part2, cont);
    if(err){
        printf("serv_proc_create, copyin fails\n");
        return EFAULT;
    }
    return 0;
}

typedef struct{
    seL4_CPtr reply_cap;
} serv_proc_destroy_cont_t;

void serv_proc_destroy(int id, seL4_CPtr reply_cap){
    //serv_proc_destroy_cont_t* cont = malloc(sizeof(serv_proc_destroy_cont_t));
    /*if(cont == NULL){
        printf("serv_proc_destroy, no memory\n");
        return;
    }*/
    int err = proc_destroy(id);

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
