#include <assert.h>
#include <string.h>
#include <cspace/cspace.h>
#include <sel4/sel4.h>
#include <errno.h>

#include "syscall/syscall.h"
#include "dev/clock.h"
#include "proc/proc.h"

#define TIMESTAMP_LOW_MASK      (0x00000000ffffffffULL)
#define TIMESTAMP_HIGH_MASK     (0xffffffff00000000ULL)

/**********************************************************************
 * TimeStamp
 **********************************************************************/

struct sleep_state{
    seL4_CPtr reply_cap;
    pid_t pid;
};

void
serv_sys_timestamp(seL4_CPtr reply_cap) {
    seL4_MessageInfo_t reply;
    timestamp_t ts;

    ts = time_stamp();
    set_cur_proc(PROC_NULL);
    reply = seL4_MessageInfo_new(0, 0, 0, 2);
    seL4_SetMR(0, (uint32_t)(ts & TIMESTAMP_LOW_MASK));
    seL4_SetMR(1, (uint32_t)((ts & TIMESTAMP_HIGH_MASK)>>32));
    seL4_Send(reply_cap, reply);

    cspace_free_slot(cur_cspace, reply_cap);
}

/**********************************************************************
 * Sleep
 **********************************************************************/

static int _sys_sleep(seL4_CPtr reply_cap, const int msec);

void
serv_sys_sleep(seL4_CPtr reply_cap, const int msec) {
    int err;
    seL4_MessageInfo_t reply;

    err = _sys_sleep(reply_cap, msec);
    if (err) {
        set_cur_proc(PROC_NULL);
        reply = seL4_MessageInfo_new(err, 0, 0, 0);
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
    }
}

static void
sleep_callback(uint32_t id, void *data) {
    seL4_MessageInfo_t reply;
    struct sleep_state *state;

    state = (struct sleep_state*)data;

    if (!is_proc_alive(state->pid)) {
        cspace_free_slot(cur_cspace, state->reply_cap);
        free(state);
        return;
    }
    set_cur_proc(PROC_NULL);
    reply = seL4_MessageInfo_new(0, 0, 0, 0);
    seL4_Send(state->reply_cap, reply);
    cspace_free_slot(cur_cspace, state->reply_cap);
    free(state);
    return;
}

static int
_sys_sleep(seL4_CPtr reply_cap, const int msec) {
    uint64_t delay = (uint64_t)msec * 1000;
    struct sleep_state *state = malloc(sizeof(struct sleep_state));
    if(state == NULL) {
        return ENOMEM;
    }
    state->reply_cap = reply_cap;
    state->pid       = proc_get_id();
    register_timer(delay, sleep_callback, (void*)state);
    return 0;
}


