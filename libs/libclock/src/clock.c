#include <assert.h>
#include <string.h>
#include <clock/clock.h>

/* Time in miliseconds that an interrupt will be fired */
#define CLOCK_INTERRUPT_TIME 4
/* Assumed ticking speed of the clock chosen, default 66MHz for ipg_clk */
#define CLOCK_ASSUMED_SPEED 66
/* Constant to be loaded into Clock control register when it gets initialized */
#define CLOCK_COMPARE_INTERVAL  (CLOCK_INTERRUPT_TIME*CLOCK_ASSUMEDSPEED*1000)

#define CLOCK_N_TIMERS 64
typedef struct {
    timestamp_t endtime;
    timer_callback_t callback;
    void* data;
    bool registered;
} timer_t;

/* The interrupts counter, count # of irps since the call of timer_start() */
uint64_t jiffy;

timer_t timers[CLOCK_N_TIMERS];
bool initialised;

/*
 * Convert miliseconds to timestamp_t unit.
 */
static timestamp_t ms2timestamp(uint64_t ms) {
    timestamp_t time = (timestamp_t)ms;
    return time;
}

int start_timer(seL4_CPtr interrupt_ep) {
    if (initialised) {
        stop_timer();
    }

    jiffy = 0;
    for (int i=0; i<CLOCK_N_TIMERS; i++) {
        timers[i].registered = false;
    }
    initialised = true;
    return CLOCK_R_OK;
}

uint32_t register_timer(uint64_t delay, timer_callback_t callback, void *data) {
    if (!initialised) return CLOCK_R_UINT;

    int id;
    for (id=0; id<CLOCK_N_TIMERS; id++) {
        if (timers[id].registered = false) {
            timers[id].endtime = time_stamp() + ms2timestamp(delay);
            timers[id].callback = callback;
            timers[id].data = data;
            timers[id].registered = true;
            break;
        }
    }
    if (id == CLOCK_N_TIMERS) {
        return 0;
    }

    return id;
}

int remove_timer(uint32_t id) {
    if (!initialised) return CLOCK_R_UINT;

    if (timers[id].registered) {
        timers[id].registered = false;
        return CLOCK_R_OK;
    }
    
    /* How do they define successful? */
    return CLOCK_R_FAIL;
}

int timer_interrupt(void) {
    if (!initialised) return CLOCK_R_UINT;

    // Could there by concurrency issue here?
    jiffy += 1;
    for (int i=0; i<CLOCK_N_TIMERS; i++) {
        if (!timers[i].registered) continue;
        if (timers[i].endtime <= time_stamp()) {
            timers[i].callback(i, timers[i].data);
            timers[i].registered = false;
        }
    }
    return CLOCK_R_OK;
}

timestamp_t time_stamp(void) {
    if (!initialised) return CLOCK_R_UINT;

    /*
     * Having a counter (jiffy) to keep counting the time elapsed. To get more
     * accurate information (as jiffy could be less accurate than 1 ms), can
     * query time from the current clock counter and add in.
     */
    return CLOCK_INTERRUPT_TIME * jiffy;
}

int stop_timer(void) {
    initialised = false;
    return CLOCK_R_OK;
}
