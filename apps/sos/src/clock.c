#include <assert.h>
#include <string.h>
#include <cspace/cspace.h>
#include <mapping.h>
#include <sys/panic.h>

#define verbose 5
#include <sys/debug.h>

#include "clock.h"

/* Time in miliseconds that an interrupt will be fired */
#define CLOCK_INT_MILISEC 100
#define CLOCK_INT_MICROSEC (CLOCK_INT_MILISEC*1000)
/* Assumed ticking speed of the clock chosen, default 66MHz for ipg_clk */
#define CLOCK_SPEED 66
/* Clock prescaler should be divisible by CLOCK_SPEED */
#define CLOCK_PRESCALER 1
#define CLOCK_SPEED_PRESCALED (CLOCK_SPEED / CLOCK_PRESCALER)
/* Constant to be loaded into Clock control register when it gets initialized */
#define CLOCK_LOAD_VALUE (CLOCK_INT_MICROSEC*(CLOCK_SPEED_PRESCALED))

#define EPIT1_IRQ_NUM       88
#define EPIT1_BASE_PADDR    0x020D0000
#define EPIT1_SIZE          0x4000
// EPIT1_CR_BASE_MASK 0b000000_01_01_0_0_0_0_1_0_000000000000_1101;
#define EPIT1_CR_MASK       0x0142000D
#define EPIT1_CR            (EPIT1_CR_MASK | ((CLOCK_PRESCALER-1) << 4))

#define EPIT_CR_CLKSRC_SHIFT    24      // Clock source shift (bits 24-25)
#define EPIT_CR_OM_SHIFT        22      // OM shift (bits 22-23)
#define EPIT_CR_STOPEN          BIT(21) // Stop enable bit
#define EPIT_CR_WAITEN          BIT(19) // Wait enable bit
#define EPIT_CR_DBGEN           BIT(18) // Debug enable bit
#define EPIT_CR_IOVW            BIT(17) // Set to overwrite CNR when write to load register
#define EPIT_CR_SWR             BIT(16)
#define EPIT_CR_PRE_SHIFT       4       // Prescalar shift (bits 4-15)
#define EPIT_CR_RLD             BIT(3)  // Reload bit
#define EPIT_CR_OCIEN           BIT(2)  // Enable interrupt
#define EPIT_CR_ENMOD           BIT(1)  // Set ENMOD to reload CNR when re-enable counter
#define EPIT_CR_EN              BIT(0)  // Enable bit     

#define EPIT_CR_CLKSRC          (1 << EPIT_CR_CLKSRC_SHIFT) // Peripheral clock
#define EPIT_CR_PRESCALER       ((CLOCK_PRESCALER-1) << EPIT_CR_PRE_SHIFT)   

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

static uint64_t
cal_load_value(void) {
    return 0;
}
/* Swap timers in timers array */
static void
tswap(timer_t *timers, const int i, const int j) {
    timer_t tmp = timers[i];
    timers[i] = timers[j];
    timers[j] = tmp;
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
    if (initialised) {
        stop_timer();
    }

    /* Initialize callback array */
    ntimers = 0;
    jiffy = 0;
    for (int i=0; i<CLOCK_N_TIMERS; i++) {
        timers[i].id = i+1;
        timers[i].registered = false;
    }

    /* Create IRQ Handler */
    irq_handler = enable_irq(EPIT1_IRQ_NUM, interrupt_ep);

    /* Map device and initialize it */
    //if mapdevice fails, it panics, 
    clkReg = map_device((void*)EPIT1_BASE_PADDR, EPIT1_SIZE); 
    //TODO: setup 2nd timer
    clkReg->cr |= EPIT_CR_SWR;
    printf("Resetting timer...\n");
    while (clkReg->cr & EPIT_CR_SWR);
    printf("Done\n");

    //clkReg->cr = 0;
    uint32_t tmp = 0;
    tmp |= EPIT_CR_CLKSRC;
    tmp |= EPIT_CR_PRESCALER;
    tmp |= EPIT_CR_IOVW;
    tmp |= EPIT_CR_RLD;
    tmp |= EPIT_CR_ENMOD;
    tmp |= EPIT_CR_OCIEN;

    clkReg->cr = tmp;
    clkReg->cmpr = 0;

    clkReg->cr |= EPIT_CR_EN;
    clkReg->lr = CLOCK_LOAD_VALUE;

    printf("CR= 0x%x, LR=%u, CMPR=%u, CNR=%u\n", clkReg->cr, clkReg->lr, clkReg->cmpr, clkReg->cnr);

    initialised = true;
    return CLOCK_R_OK;
}


int stop_timer(void){
    if (!initialised) return CLOCK_R_UINT;
    /* Map device and turn it off */
    clkReg->cr &= ~(0x1);

    int err = 0;
    /* Remove handler within kernel */
    err = seL4_IRQHandler_Clear(irq_handler);
    assert(!err);

    /* Free the irq_handler cap within cspace */
    cspace_err_t cspace_err = cspace_delete_cap(cur_cspace, irq_handler);
    assert(cspace_err == CSPACE_NOERROR);

    //TODO: stop 2nd timer!!

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
    timers[ntimers].endtime = cur_time + delay;
    timers[ntimers].callback = callback;
    timers[ntimers].data = data;
    timers[ntimers].registered = true;
    int id = timers[ntimers].id;
    printf("id=%d, curtime=%lld,  endtime=%lld\n", id, cur_time, timers[ntimers].endtime);
    ntimers += 1;

    /* Re-arrange the timers array to ensure the order */
    for (int i=ntimers-1; i>0; i--) {
        if (timers[i].endtime < timers[i-1].endtime) {
            tswap(timers, i, i-1);
        } else {
            break;
        }
    }
    // TODO: enable 2nd timer

    assert(ntimers >= 0 && ntimers <= CLOCK_N_TIMERS);
    return id;
}

int remove_timer(uint32_t id) {
    if (!initialised) return CLOCK_R_UINT;
    assert(ntimers >= 0 && ntimers <= CLOCK_N_TIMERS);

    if (id <= 0 || id > CLOCK_N_TIMERS) return CLOCK_R_FAIL;

    /* Find the index of the timer */
    int i;
    for (i=0; i<ntimers; i++) {
        if (timers[i].id == id && timers[i].registered) break;
    }
    if (i == ntimers) {
        return CLOCK_R_FAIL;
    }

    /* Swap this timer to the end */
    timer_t tmp = timers[i];
    for (; i<ntimers-1; i++) {
        timers[i] = timers[i+1];
    }
    timers[ntimers-1] = tmp;
    timers[ntimers-1].registered = false;
    ntimers -= 1;

    //TODO: update/disable 2nd timer?

    assert(ntimers >= 0 && ntimers <= CLOCK_N_TIMERS);
    return CLOCK_R_OK;
}

/*
 * Check if there is a timeout. If there is, perform a callback
 * @Return: true if there is at least 1 timeout, false otherwise
 */
static bool
check_timeout(timestamp_t cur_time) {
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
    return (i > 0);
}
int timer_interrupt(void) {
    if (!initialised) return CLOCK_R_UINT;

    timestamp_t cur_time = time_stamp();
    printf("timer interrupt at %lld\n", cur_time);

    bool timeout = check_timeout(cur_time);
    if (timeout) {
        //TODO: enable 2nd timer?
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
     * Apart from jiffy counter, we need to take into account the current value
     * of the clock's counter EPIT_CNR. We also take care of the case when an
     * overflow has happened but haven't got acknowledged
     */
    uint64_t cnr_diff = CLOCK_LOAD_VALUE - clkReg->cnr;
    uint64_t offset = (uint64_t)CLOCK_INT_MICROSEC*cnr_diff / CLOCK_LOAD_VALUE;
    return CLOCK_INT_MICROSEC * (jiffy + clkReg->sr) + offset; 
}
