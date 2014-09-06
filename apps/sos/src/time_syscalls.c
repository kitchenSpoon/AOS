#include <assert.h>
#include <string.h>
#include <cspace/cspace.h>
#include <sel4/sel4.h>
#include <errno.h>

#include "syscall.h"
#include "clock.h"

struct sleep_state{
    seL4_CPtr reply_cap;
};

int
serv_sys_timestamp(timestamp_t *ts) {
    *ts = time_stamp();
    return 0;
}

static void
sleep_callback(uint32_t id, void *data) {
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(0,0,0,0);
    seL4_Send(((struct sleep_state*)data)->reply_cap, reply);
    cspace_free_slot(cur_cspace, ((struct sleep_state*)data)->reply_cap);
    return;
}

int
serv_sys_sleep(seL4_CPtr reply_cap, const int msec) {
    //register for the clock
    uint64_t delay = (uint64_t)msec * 1000;
    struct sleep_state *state = malloc(sizeof(struct sleep_state));
    state->reply_cap = reply_cap;
    if(state == NULL) {
        return ENOMEM;
    }
    register_timer(delay, sleep_callback, (void*)state);
    return 0;
}
