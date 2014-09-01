#include <assert.h>
#include <string.h>
#include <cspace/cspace.h>
#include <sel4/sel4.h>

#include "syscall.h"
#include "clock.h"

int
serv_sys_timestamp(timestamp_t *ts) {
    *ts = time_stamp();
    return 0;
}

static void
sleep_callback(uint32_t id, void *data) {
    return 0;
}

int
serv_sys_sleep(const int msec) {
    //register for the clock
    uint64_t delay = (uint64_t)msec * 1000;
    int clock_id = register_timer(delay, sleep_callback, NULL);
    return 0;
}
