#include <assert.h>
#include <string.h>
#include <clock/clock.h>


uint32_t register_timer(uint64_t delay, timer_callback_t callback, void *data) {
    return 0; // just fail for now
}

int remove_timer(uint32_t id) {
    return !CLOCK_R_OK; // fail for now
}

int timer_interrupt(void) {
    return !CLOCK_R_OK; // fail for now
}

timestamp_t time_stamp(void) {
    /*
     * Having a counter to keep counting the time elapsed. To get more accurate
     * information (as counter could be less accurate than 1 microsec), can
     * query time from the current clock counter and add in
     */
    return -1; // nothing for now
}


