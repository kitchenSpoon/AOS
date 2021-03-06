#include <syscall/syscall.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <proc/proc.h>
#include <errno.h>
#include "tool/utility.h"

#include "vm/copyinout.h"

#define verbose 0
#include <sys/debug.h>

/**********************************************************************
 * Server Process Create
 **********************************************************************/

typedef struct{
    char* kpath;
    size_t len;
    seL4_CPtr fault_ep;
    seL4_CPtr reply_cap;
} serv_proc_create_cont_t;

static void serv_proc_create_part2(void* token, int err);
static void serv_proc_create_end(void* token, int err, pid_t pid);

void serv_proc_create(char* path, size_t len, seL4_CPtr fault_ep, seL4_CPtr reply_cap){

    dprintf(3, "serv_proc_create entered len = %d\n",len);
    serv_proc_create_cont_t* cont = malloc(sizeof(serv_proc_create_cont_t));
    if(cont == NULL){
        dprintf(3, "serv_proc_create, err no mem for continuation\n");
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
        dprintf(3, "serv_proc_create, no permission to read path\n");
        serv_proc_create_end((void*)cont, EINVAL, -1);
        return;
    }

    //copyin path;
    cont->kpath = malloc(len+1);
    dprintf(3, "serv_proc_create, kpath = 0x%08x\n",(seL4_Word)cont->kpath);
    if(cont->kpath == NULL){
        dprintf(3, "serv_proc_create, no memory for path\n");
        serv_proc_create_end((void*)cont, ENOMEM, -1);
        return;
    }

    int err = copyin((seL4_Word)cont->kpath, (seL4_Word)path, len,
            serv_proc_create_part2, (void*)cont);
    if(err){
        dprintf(3, "serv_proc_create, copyin fails\n");
        serv_proc_create_end((void*)cont, err, -1);
        return;
    }
}

static void
serv_proc_create_part2(void* token, int err){
    serv_proc_create_cont_t* cont = (serv_proc_create_cont_t*)token;
    if(err){
        serv_proc_create_end(token, err, -1);
    } else {
        cont->kpath[cont->len] = '\0';
        proc_create(cont->kpath, cont->len, cont->fault_ep, serv_proc_create_end, (void*)cont);
    }
}

static void
serv_proc_create_end(void* token, int err, pid_t pid){
    dprintf(3, "serv_proc_create_end\n");
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

/**********************************************************************
 * Server Process Destroy
 **********************************************************************/

typedef struct{
    seL4_CPtr reply_cap;
} serv_proc_destroy_cont_t;

void serv_proc_destroy(pid_t pid, seL4_CPtr reply_cap){
    pid_t cur_pid = proc_get_id();
    int err = proc_destroy(pid);

    if (pid != cur_pid) {
        seL4_MessageInfo_t reply;
        reply = seL4_MessageInfo_new(err, 0, 0, 1);
        seL4_SetMR(0, (seL4_Word)err);
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
    }

    set_cur_proc(PROC_NULL);
    return;
}

/**********************************************************************
 * Server Process Get ID
 **********************************************************************/

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

/**********************************************************************
 * Server Process Wait
 **********************************************************************/

typedef struct {
    seL4_CPtr reply_cap;
    pid_t pid;
} serv_proc_wait_cont_t;

static void serv_proc_wait_cb(void *token, pid_t pid);

void serv_proc_wait(pid_t pid, seL4_CPtr reply_cap){
    pid_t cur_pid = proc_get_id();
    dprintf(3, "serv_proc_wait, proc = %d\n", cur_pid);
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
    cont->pid = cur_pid;

    int err = proc_wait(pid, serv_proc_wait_cb, (void*)cont);
    if (err) {
        dprintf(3, "serv_proc_wait: proc_wait failed\n");
        serv_proc_wait_cb((void*)cont, -1);
    }
}

static void
serv_proc_wait_cb(void *token, pid_t pid) {
    dprintf(3, "serv_proc_wait_cb called, proc = %d\n", proc_get_id());
    serv_proc_wait_cont_t *cont = (serv_proc_wait_cont_t*)token;
    assert(cont != NULL);

    /* Check if the waiting process is still active */
    if (is_proc_alive(cont->pid)) {
        set_cur_proc(PROC_NULL);
        seL4_MessageInfo_t reply;
        reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, (seL4_Word)pid);
        seL4_Send(cont->reply_cap, reply);
    }
    cspace_free_slot(cur_cspace, cont->reply_cap);
    free(cont);

    dprintf(3, "serv_proc_wait_cb ended\n");
    // we don't want to clear cur_proc because the caller is still in process
    // of destroying itself
    //set_cur_proc(PROC_NULL);
}

/**********************************************************************
 * Server Process Status
 **********************************************************************/

typedef struct {
    sos_process_t *kbuf;
    unsigned num;
    seL4_CPtr reply_cap;
    pid_t pid;
} serv_proc_status_cont_t ;

void serv_proc_status_end(void* token, int err);

void serv_proc_status(seL4_Word buf, unsigned max, seL4_CPtr reply_cap){
    dprintf(3, "serv_proc_status\n");

    serv_proc_status_cont_t* cont = malloc(sizeof(serv_proc_status_cont_t));
    if(cont == NULL){
        set_cur_proc(PROC_NULL);
        //send message back
        seL4_MessageInfo_t reply;
        reply = seL4_MessageInfo_new(ENOMEM, 0, 0, 0);
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }
    cont->reply_cap = reply_cap;
    cont->kbuf = NULL;
    cont->num = 0;
    cont->pid = proc_get_id();


    dprintf(3, "serv_proc_status\n");
    uint32_t permissions = 0;
    if(!as_is_valid_memory(proc_getas(), (seL4_Word)buf, sizeof(sos_process_t) * MIN(MAX_PROC, max), &permissions) ||
            !(permissions & seL4_CanWrite)){
        serv_proc_status_end((void*)cont, EINVAL);
        return;
    }

    dprintf(3, "serv_proc_status\n");
    sos_process_t* kbuf = malloc(sizeof(sos_process_t) * MIN(MAX_PROC, max));
    if(kbuf == NULL){
        serv_proc_status_end((void*)cont, ENOMEM);
        return;
    }
    cont->kbuf = kbuf;

    dprintf(3, "serv_proc_status\n");
    cont->num = 0;
    for(int i = 0; i < MAX_PROC; i++){
        if(processes[i] != NULL){
            kbuf[cont->num].pid = processes[i]->pid;
            kbuf[cont->num].size = processes[i]->size;
            kbuf[cont->num].stime = (unsigned int)time_stamp()/1000 - processes[i]->stime;
            memcpy(kbuf[cont->num].command, processes[i]->name, processes[i]->name_len);
            kbuf[cont->num].command[processes[i]->name_len] = '\0';
            dprintf(3, "serv_proc_status, procing i = %d kbuf name = %s\n",i,kbuf[cont->num].command);

            cont->num++;
            if(cont->num >= max) break;
        }
    }

    dprintf(3, "serv_proc_status\n");
    int err = copyout((seL4_Word)buf, (seL4_Word)kbuf, sizeof(sos_process_t) * MIN(cont->num, max), serv_proc_status_end, (void*)cont);
    if(err){
        serv_proc_status_end((void*)cont, err);
        return;
    }
}

void serv_proc_status_end(void* token, int err){
    dprintf(3, "serv_proc_status end\n");
    serv_proc_status_cont_t* cont = (serv_proc_status_cont_t*)token;

    if (cont->kbuf) {
        free(cont->kbuf);
    }
    if (is_proc_alive(cont->pid)) {
        set_cur_proc(PROC_NULL);
        seL4_MessageInfo_t reply;
        reply = seL4_MessageInfo_new(err, 0, 0, 1);
        seL4_SetMR(0, cont->num);
        seL4_Send(cont->reply_cap, reply);
    }
    cspace_free_slot(cur_cspace, cont->reply_cap);
    free(cont);
}


