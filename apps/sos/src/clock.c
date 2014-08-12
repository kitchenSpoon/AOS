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
#define CLOCK_COMPARE_INTERVAL (CLOCK_INT_TIME*(CLOCK_SPEED_PRESCALED)*1000)

#define BIT(n) (1ul<<(n))

#define EPIT_CLKSRC        (0b01 << 24) // Clock source - 0b01 == peripheral clock
#define EPIT_OM            (0b00 << 22) // Output mode - 0b00 == no output
#define EPIT_OM_SHIFT      (22)         // Output mode shift
#define EPIT_STOPEN        BIT(21)      // Stop enable
#define EPIT_WAITEN        BIT(19)      // Wait enable
#define EPIT_DBGEN         BIT(18)      // Debug enable
#define EPIT_IOVW          BIT(17)      // Counter overwrite when set load reg
#define EPIT_SWR           BIT(16)      // Software reset
#define EPIT_PRESCALAR_SHIFT 4
#define EPIT_PRESCALAR     ((CLOCK_PRESCALER-1) << 4) // Prescalar
#define EPIT_RLD           BIT(3)       // Reload control -
                                        // 1 == reload from load register
#define EPIT_OCIEN         BIT(2)       // Compare interrupt enable
#define EPIT_ENMOD         BIT(1)       // Enable mode - 0b01 == Counter restart when re-enabled
#define EPIT_EN            BIT(0)       // Enable bit


#define EPIT1_IRQ_NUM       88
#define EPIT1_BASE_PADDR    0x020D0000
#define EPIT1_SIZE          (0x4000)

#define CLOCK_N_TIMERS 64
typedef struct {
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

static seL4_CPtr
enable_irq(int irq, seL4_CPtr aep) {
    seL4_CPtr cap;
    int err;
    /* Create an IRQ handler */
    cap = cspace_irq_control_get_cap(cur_cspace, seL4_CapIRQControl, irq);
    conditional_panic(!cap, "Failed to acquire and IRQ control cap");
    /* Assign to an end point */
    err = seL4_IRQHandler_SetEndpoint(cap, aep);
    conditional_panic(err, "Failed to set interrupt endpoint");
    /* Ack the handler before continuing */
    err = seL4_IRQHandler_Ack(cap);
    conditional_panic(err, "Failure to acknowledge pending interrupts");
    return cap;
}

int start_timer(seL4_CPtr interrupt_ep) {
    dprintf(0, "initialised = %d\n", initialised);
    if (initialised) {
        dprintf(0, "Reseting timer...\n");
        stop_timer();
    }

    dprintf(0, "Initializing timer...\n");
    /* Initialize callback array */
    jiffy = 0;
    for (int i=0; i<CLOCK_N_TIMERS; i++) {
        timers[i].registered = false;
    }

    /* Create IRQ Handler */
    irq_handler = enable_irq(EPIT1_IRQ_NUM, interrupt_ep);

    /* Map device and initialize it */
    //if mapdevice fails, it panics, 
    clkReg = map_device((void*)EPIT1_BASE_PADDR, EPIT1_SIZE); 

    /*
    clkReg->cr &= ~EPIT_EN;

    clkReg->cr &= ~(0xfff << EPIT_PRESCALAR_SHIFT);
    //clkReg->cr |= EPIT_PRESCALAR;
    clkReg->cr |= EPIT_RLD;

    clkReg->cr &= ~(0b11 << EPIT_OM_SHIFT); //clear EPIT output
    clkReg->cr &= ~EPIT_OCIEN;
    clkReg->cr |= EPIT_CLKSRC;
    clkReg->sr = 1;
    clkReg->cr |= EPIT_ENMOD;
    clkReg->cr |= EPIT_EN;
    clkReg->cr |= EPIT_OCIEN;

    clkReg->lr = CLOCK_COMPARE_INTERVAL;
    clkReg->cmpr = CLOCK_COMPARE_INTERVAL;
    */
    clkReg->cr = 0;
    clkReg->cr |= EPIT_CLKSRC;
    clkReg->cr |= EPIT_OM;
    clkReg->cr |= EPIT_IOVW;
    clkReg->cr |= EPIT_PRESCALAR;
    clkReg->cr |= EPIT_RLD;
    clkReg->cr |= EPIT_OCIEN;
    clkReg->cr |= EPIT_ENMOD;
    clkReg->cr |= EPIT_EN;
    
    clkReg->lr = CLOCK_COMPARE_INTERVAL;
    clkReg->cmpr = CLOCK_COMPARE_INTERVAL;
    clkReg->sr = 1;

    dprintf(0, "clkReg->cr = 0x%x, clkReg->lr = %d, clkReg->cmpr = %d\n",
                clkReg->cr, clkReg->lr, clkReg->cmpr);

    initialised = true;
    return CLOCK_R_OK;
}


int stop_timer(void){
    if (!initialised) return CLOCK_R_UINT;
    /* Map device and turn it off */
    clkReg->cr = 0;

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
    int cnr_diff = CLOCK_COMPARE_INTERVAL - clkReg->cnr;
    int offset = (long long)CLOCK_INT_TIME*cnr_diff / CLOCK_COMPARE_INTERVAL;
    return CLOCK_INT_TIME * jiffy + offset;
}
