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

static void
serv_proc_create_end(void* token, int err, pid_t pid){
    printf("serv_proc_create_end\n");
    serv_proc_create_cont_t* cont = (serv_proc_create_cont_t*)token;

    set_cur_proc(PROC_NULL);
    seL4_MessageInfo_t reply;
    reply = seL4_MessageInfo_new(err, 0, 0, 1);
    seL4_SetMR(0, pid);
    seL4_Send(cont->reply_cap, reply);
    cspace_free_slot(cur_cspace, cont->reply_cap);

    if (cont->kpath) free(cont->kpath);
    free(cont);
}

static void
serv_proc_create_part2(void* token, int err){
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
        set_cur_proc(PROC_NULL);
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

void serv_proc_destroy(pid_t pid, seL4_CPtr reply_cap){
    //serv_proc_destroy_cont_t* cont = malloc(sizeof(serv_proc_destroy_cont_t));
    /*if(cont == NULL){
        printf("serv_proc_destroy, no memory\n");
        return;
    }*/
    int err = proc_destroy(pid);

    set_cur_proc(PROC_NULL);
    seL4_MessageInfo_t reply;
    reply = seL4_MessageInfo_new(err, 0, 0, 0);
    //seL4_SetMR(0, 0);
    seL4_Send(reply_cap, reply);
    cspace_free_slot(cur_cspace, reply_cap);
    return;
}

void serv_proc_get_id(seL4_CPtr reply_cap){
    pid_t id = proc_get_id();

    set_cur_proc(PROC_NULL);

    seL4_MessageInfo_t reply;
    reply = seL4_MessageInfo_new(0, 0, 0, 1);
    seL4_SetMR(0, id);
    seL4_Send(reply_cap, reply);
    cspace_free_slot(cur_cspace, reply_cap);
    return;
}

typedef struct {
    seL4_CPtr reply_cap;
} serv_proc_wait_cont_t;

static void
serv_proc_wait_cb(void *token, pid_t pid) {
    serv_proc_wait_cont_t *cont = (serv_proc_wait_cont_t*)token;
    assert(cont != NULL);

    seL4_MessageInfo_t reply;
    reply = seL4_MessageInfo_new(0, 0, 0, 1);
    seL4_SetMR(0, pid);
    seL4_Send(cont->reply_cap, reply);
    cspace_free_slot(cur_cspace, cont->reply_cap);
    free(cont);

    // we don't want to clear cur_proc because the caller is still in process
    // of destroying itself
    //set_cur_proc(PROC_NULL);
}

void serv_proc_wait(pid_t pid, seL4_CPtr reply_cap){
    printf("serv_proc_wait, proc = %d\n", proc_get_id());
    serv_proc_wait_cont_t *cont = malloc(sizeof(serv_proc_wait_cont_t));
    if (cont == NULL) {
        // No memory, reply to user with pid -1
        seL4_MessageInfo_t reply;
        reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, -1);
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);

        set_cur_proc(PROC_NULL);
        return;
    }
    cont->reply_cap = reply_cap;

    int err = proc_wait(pid, serv_proc_wait_cb, (void*)cont);
    if (err) {
        serv_proc_wait_cb((void*)cont, -1);
    }
}

void serv_proc_status(void){
    set_cur_proc(PROC_NULL);
}
