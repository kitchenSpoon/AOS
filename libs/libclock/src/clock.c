#include <assert.h>
#include <string.h>
#include <clock/clock.h>
#include <cspace/cspace.h>
//#include <apps/sos/src/mapping.h>

/* Time in miliseconds that an interrupt will be fired */
#define CLOCK_INTERRUPT_TIME 4
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
//change this to
//timestamp_t jiffy; ?
uint64_t jiffy;

timer_t timers[CLOCK_N_TIMERS];
bool initialised;

seL4_CPtr irq_handler;
#define EPIT1_IRQ_NUM 88


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

    /* Map device and initialize it */
    //if mapdevice fails, it panics, 
    seL4_Word * epit1_cr = map_device((void*)0x20D0000, 4); 
    (*epit1_cr) = 0b00000001100000010000000000011101; //this is wrong, need to remove underscore
    
    seL4_Word * epit1_lr = map_device((void*)0x20D0008, 4); 
    (*epit1_lr) = CLOCK_COMPARE_INTERVAL;
    
    seL4_Word * epit1_cmpr = map_device((void*)0x20D000C, 4); 
    (*epit1_cmpr) = CLOCK_COMPARE_INTERVAL;

    initialised = true;
    return CLOCK_R_OK;
}


int stop_timer(void){
    /* Map device and turn it off */
    seL4_Word * epit1_cr = map_device((void*)0x20D0000, 4); 
    (*epit1_cr) = 0b00000001100000010000000000011100;

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

    //no available slot for addtional timers
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

    seL4_Word * epit1_sr = map_device((void*)0x20D0004, 4); 
    (*epit1_sr) = 1;
    
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
    seL4_Word * epit1_cnr = map_device((void*)0x20D0010, 4); 
    return CLOCK_INTERRUPT_TIME * jiffy + (*epit1_cnr);
}


