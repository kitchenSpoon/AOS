#include <assert.h>
#include <string.h>
#include <cspace/cspace.h>
#include <mapping.h>
#include <sys/panic.h>

#define verbose 5
#include <sys/debug.h>

#include "clock.h"

/* Time in miliseconds that an interrupt will be fired */
#define CLOCK_INT_TIME 100
/* Assumed ticking speed of the clock chosen, default 66MHz for ipg_clk */
#define CLOCK_SPEED 66
/* Clock prescaler should be divisible by CLOCK_SPEED */
#define CLOCK_PRESCALER 1
#define CLOCK_SPEED_PRESCALED (CLOCK_SPEED / CLOCK_PRESCALER)
/* Constant to be loaded into Clock control register when it gets initialized */
#define CLOCK_LOAD_VALUE (CLOCK_INT_TIME*(CLOCK_SPEED_PRESCALED)*1000)

#define EPIT1_IRQ_NUM       88
#define EPIT1_BASE_PADDR    0x020D0000
#define EPIT1_SIZE          0x4000
// EPIT1_CR_BASE_MASK 0b000000_01_01_0_0_0_0_1_0_000000000000_1101;
#define EPIT1_CR_MASK       0x0142000F
#define EPIT1_CR            (EPIT1_CR_MASK | ((CLOCK_PRESCALER-1) << 4))
//EPIT1_CR_CLR 0b0000_0001_1000_0011_0000_0000_0001_1100;
#define EPIT1_CR_CLR        0x0183001C

#define CLOCK_N_TIMERS 64
typedef struct {
    int id;
    timestamp_t endtime;
    timer_callback_t callback;
    void* data;
    bool registered;
} timer_t;

typedef struct {
    uint32_t cr;
    uint32_t sr;
    uint32_t lr;
    uint32_t cmpr;
    uint32_t cnr;
} clock_register_t;

/* The interrupts counter, count # of irps since the call of timer_start() */
static uint64_t jiffy;

static int ntimers;
static timer_t timers[CLOCK_N_TIMERS];
static bool initialised;

static seL4_CPtr irq_handler;
static clock_register_t *clkReg;


/*
 * Convert miliseconds to timestamp_t unit.
 */
static timestamp_t ms2timestamp(uint64_t ms) {
    timestamp_t time = (timestamp_t)ms;
    return time;
}
/* Swap timers in timers array */
static void
tswap(timer_t *timers, const int i, const int j) {
    timer_t tmp = timers[i];
    timers[i] = timers[j];
    timers[j] = tmp;
}

int start_timer(seL4_CPtr interrupt_ep) {
    if (initialised) {
        stop_timer();
    }

    /* Initialize callback array */
    ntimers = 0;
    jiffy = 0;
    for (int i=0; i<CLOCK_N_TIMERS; i++) {
        timers[i].id = i;
        timers[i].registered = false;
    }

    /* Create IRQ Handler */
    irq_handler = enable_irq(EPIT1_IRQ_NUM, interrupt_ep);

    /* Map device and initialize it */
    //if mapdevice fails, it panics, 
    clkReg = map_device((void*)EPIT1_BASE_PADDR, EPIT1_SIZE); 
    clkReg->cr = EPIT1_CR;
    clkReg->lr = CLOCK_LOAD_VALUE;
    clkReg->cmpr = CLOCK_LOAD_VALUE;

    initialised = true;
    return CLOCK_R_OK;
}


int stop_timer(void){
    if (!initialised) return CLOCK_R_UINT;
    /* Map device and turn it off */
    clkReg->cr = EPIT1_CR_CLR;

    int err = 0;
    /* Remove handler within kernel */
    err = seL4_IRQHandler_Clear(irq_handler);
    assert(!err);

    /* Free the irq_handler cap within cspace */
    cspace_err_t cspace_err = cspace_delete_cap(cur_cspace, irq_handler);
    assert(cspace_err == CSPACE_NOERROR);

    initialised = false;
    return CLOCK_R_OK;
}

uint32_t register_timer(uint64_t delay, timer_callback_t callback, void *data) {
    if (!initialised) return CLOCK_R_UINT;
    assert(ntimers >= 0 && ntimers <= CLOCK_N_TIMERS);
    if (ntimers == CLOCK_N_TIMERS) {
        return 0;
    }
    printf("registered timer called with delay=%lld\n", delay);

    /* Put the new timer into the last slot */
    timestamp_t cur_time = time_stamp();
    timers[ntimers].endtime = cur_time + ms2timestamp(delay);
    timers[ntimers].callback = callback;
    timers[ntimers].data = data;
    timers[ntimers].registered = true;
    int id = timers[ntimers].id;
    printf("id=%d, curtime=%lld,  endtime=%lld\n",
            id, cur_time, timers[ntimers].endtime);
    ntimers += 1;

    /* Re-arrange the timers array to ensure the order */
    for (int i=ntimers-1; i>0; i--) {
        if (timers[i].endtime < timers[i-1].endtime) {
            tswap(timers, i, i-1);
        } else {
            break;
        }
    }

    assert(ntimers >= 0 && ntimers <= CLOCK_N_TIMERS);
    return id;
}

int remove_timer(uint32_t id) {
    if (!initialised) return CLOCK_R_UINT;
    assert(ntimers >= 0 && ntimers <= CLOCK_N_TIMERS);

    /* Find the index of the timer */
    int i;
    for (i=0; i<ntimers; i++) {
        if (timers[i].id == id) break;
    }
    if (i == ntimers) {
        return CLOCK_R_FAIL;
    }

    timers[i].registered = false;

    /* Remove this timer from timers queue */
    timer_t tmp = timers[i];
    ntimers -= 1;
    for (; i<ntimers-2; i++) {
        timers[i] = timers[i+1];
    }
    timers[ntimers] = tmp;

    assert(ntimers >= 0 && ntimers <= CLOCK_N_TIMERS);
    return CLOCK_R_OK;
}

int timer_interrupt(void) {
    if (!initialised) return CLOCK_R_UINT;

    timestamp_t cur_time = time_stamp();
    printf("timer interrupt at %lld\n", cur_time);
    int i;
    for (i=0; i<ntimers; i++) {
        if (timers[i].registered && timers[i].endtime <= cur_time) {
            timers[i].callback(timers[i].id, timers[i].data);
            timers[i].registered = false;
        } else {
            break;
        }
    }
    if (i > 0) {
        for (int j = i; j<ntimers; j++) {
            timers[j-i] = timers[j];
        }
        ntimers -= i;
    }
    if (i > 0) {
        for (int j = i; j<ntimers; j++) {
            timers[j-i] = timers[j];
        }
        ntimers -= i;
    }

    jiffy += 1;
    clkReg->sr = 1;
    int err = seL4_IRQHandler_Ack(irq_handler);
    assert(!err);

    return CLOCK_R_OK;
}

timestamp_t time_stamp(void) {
    if (!initialised) return CLOCK_R_UINT;

    /*
     * Having a counter (jiffy) to keep counting the time elapsed. To get more
     * accurate information (as jiffy could be less accurate than 1 ms), can
     * query time from the current clock counter and add in.
     */
    //int cnr_diff = CLOCK_COMPARE_INTERVAL - clkReg->cnr;
    //int offset = (long long)CLOCK_INT_TIME*cnr_diff / CLOCK_COMPARE_INTERVAL;
    //return CLOCK_INT_TIME * jiffy + offset;
    //return CLOCK_INT_TIME * jiffy + CLOCK_INT_TIME*(clkReg->lr - clkReg->cnr)/CLOCK_LOAD_VALUE;
    return CLOCK_INT_TIME * (jiffy + clkReg->sr); 
}
