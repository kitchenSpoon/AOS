#include <assert.h>
#include <string.h>
#include <cspace/cspace.h>
#include <mapping.h>
#define verbose 5
#include <sys/debug.h>

#include "clock.h"

/* Time in miliseconds that an interrupt will be fired */
#define CLOCK_INTERRUPT_TIME 100
/* Assumed ticking speed of the clock chosen, default 66MHz for ipg_clk */
#define CLOCK_ASSUMED_SPEED 66
/* Constant to be loaded into Clock control register when it gets initialized */
#define CLOCK_COMPARE_INTERVAL  (CLOCK_INTERRUPT_TIME*CLOCK_ASSUMED_SPEED*1000)

#define CLOCK_N_TIMERS 64
typedef struct {
    timestamp_t endtime;
    timer_callback_t callback;
    void* data;
    bool registered;
} timer_t;

/* The interrupts counter, count # of irps since the call of timer_start() */
static uint64_t jiffy;

static timer_t timers[CLOCK_N_TIMERS];
static bool initialised;

seL4_CPtr irq_handler;
#define EPIT1_IRQ_NUM 88


/*
 * Convert miliseconds to timestamp_t unit.
 */
static timestamp_t ms2timestamp(uint64_t ms) {
    timestamp_t time = (timestamp_t)ms;
    return time;
}

typedef struct clockRegister{
    seL4_Word cr;
    seL4_Word sr;
    seL4_Word lr;
    seL4_Word cmpr;
    seL4_Word cnr;
	    
} clock_register_t;

int start_timer(seL4_CPtr interrupt_ep) {
    if (initialised) {
        stop_timer();
    }
    /* Initialize callback array */
    jiffy = 0;
    for (int i=0; i<CLOCK_N_TIMERS; i++) {
        timers[i].registered = false;
    }

    /* Create IRQ Handler */
    irq_handler  = cspace_irq_control_get_cap(cur_cspace, seL4_CapIRQControl, EPIT1_IRQ_NUM);
    assert(irq_handler);

    int err = 0;
    /* Assign it to an endpoint*/
    err = seL4_IRQHandler_SetEndpoint(irq_handler, interrupt_ep);
    assert(!err);

    err = seL4_IRQHandler_Ack(irq_handler);
    assert(!err);

    /* Map device and initialize it */
    //if mapdevice fails, it panics, 
    clock_register_t * clkReg = map_device((void*)0x020D0000, 4000); 
    clkReg->cr = 0b00000001010000110000000000001101;
    clkReg->lr = CLOCK_INTERRUPT_TIME;
    clkReg->cmpr = CLOCK_INTERRUPT_TIME;

    initialised = true;
    return CLOCK_R_OK;
}


int stop_timer(void){
    /* Map device and turn it off */
    clock_register_t * clkReg = map_device((void*)0x020D0000, 4000); 
    clkReg->cr = 0b00000001100000110000000000011100;

    int err = 0;
    /* Remove handler with kernel */
    err = seL4_IRQHandler_Clear(irq_handler);
    assert(!err);

    /* Free the irq_handler cap within cspace */
    cspace_err_t cspace_err = cspace_free_slot(cur_cspace, irq_handler);
    assert(cspace_err == CSPACE_NOERROR);

    initialised = false;
    return CLOCK_R_OK;
}

uint32_t register_timer(uint64_t delay, timer_callback_t callback, void *data) {
    if (!initialised) return CLOCK_R_UINT;

    dprintf(0, "registered timer called with delay=%lld\n", delay);
    int id;
    for (id=0; id<CLOCK_N_TIMERS; id++) {
        if (timers[id].registered == false) {
	    timestamp_t curtime = time_stamp();   
            timers[id].endtime = curtime + ms2timestamp(delay);
            timers[id].callback = callback;
            timers[id].data = data;
            timers[id].registered = true;
	    dprintf(0, "id= %d, curtime = %lld, endtime = %lld\n", id, (uint64_t)curtime, (uint64_t)timers[id].endtime);
            break;
        }
    }

    //no available slot for addtional timers
    if (id == CLOCK_N_TIMERS) {
        return 0;
    }

    return id;
}

int remove_timer(uint32_t id) {
    if (!initialised) return CLOCK_R_UINT;

    timers[id].registered = false;
    return CLOCK_R_OK;
}

int timer_interrupt(void) {
    if (!initialised) return CLOCK_R_UINT;

    dprintf(0, "timer interrupt at %lld\n", time_stamp());
    // Could there by concurrency issue here?
    jiffy += 1;
    for (int i=0; i<CLOCK_N_TIMERS; i++) {
        if (timers[i].registered && timers[i].endtime <= time_stamp()) {
            timers[i].callback(i, timers[i].data);
            timers[i].registered = false;
        }
    }

    clock_register_t * clkReg = map_device((void*)0x020D0000, 4000); 
    clkReg->sr = 1;
    
    int err = 0;
    err = seL4_IRQHandler_Ack(irq_handler); 
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

    return CLOCK_INTERRUPT_TIME * jiffy;
    /*
    clock_register_t * clkReg = map_device((void*)0x020D0000, 4000); 
    return CLOCK_INTERRUPT_TIME * jiffy + (clkReg->cnr);
    */
}
