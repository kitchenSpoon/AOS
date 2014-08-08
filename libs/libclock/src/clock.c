#include <assert.h>
#include <string.h>
#include <clock/clock.h>

/* Time in miliseconds that an interrupt will be fired */
#define CLOCK_INTERRUPT_TIME 4 
/* Assumed ticking speed of the clock chosen, default 66MHz for ipg_clk */
#define CLOCK_ASSUMED_SPEED 66
/* Constant to be loaded into Clock control register when it gets initialized */
#define CLOCK_COMPARE_INTERVAL  (CLOCK_INTERRUPT_TIME*CLOCK_ASSUMEDSPEED*1000)

/* The interrupts counter, count # of irps since the call of timer_start() */
uint64_t jiffy;

typedef struct {

} timer_register_t;

uint32_t register_timer(uint64_t delay, timer_callback_t callback, void *data) {
    return 0; // just fail for now
}

int remove_timer(uint32_t id) {
    return !CLOCK_R_OK; // fail for now
}

int timer_interrupt(void) {
    // Could there by concurrency issue here?
    jiffy++;
    /* Change clock_cmp_reg here */
    /* Check the queue/data structure that contains the registered timers */
    //TODO:
    return CLOCK_R_OK;
}

timestamp_t time_stamp(void) {
    /*
     * Having a counter to keep counting the time elapsed. To get more accurate
     * information (as counter could be less accurate than 1 microsec), can
     * query time from the current clock counter and add in
     */
    return -1; // nothing for now
}
