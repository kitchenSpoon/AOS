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

#define EPIT1_IRQ_NUM       88
#define EPIT1_BASE_PADDR    0x020D0000
#define EPIT1_SIZE          0x4000

#define EPIT2_IRQ_NUM       89
#define EPIT2_BASE_PADDR    0x020D4000
#define EPIT2_SIZE          0x4000


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

static seL4_CPtr irq_handler1, irq_handler2;
clock_register_t *epit1, *epit2;

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


/* We have similar setup for EPIT1 & EPIT2 */
static void
setup_epit(clock_register_t *epit) {
    epit->cr |= EPIT_CR_SWR;
    printf("Resetting an EPIT\n");
    while (epit->cr & EPIT_CR_SWR);
    printf("Done\n");

    uint32_t tmp = 0;
    tmp |= EPIT_CR_CLKSRC;
    tmp |= EPIT_CR_PRESCALER;
    tmp |= EPIT_CR_IOVW;
    tmp |= EPIT_CR_RLD;
    tmp |= EPIT_CR_ENMOD;
    tmp |= EPIT_CR_OCIEN;
    epit->cr = tmp;

    printf("EPIT: CR= 0x%x, LR=%u, CMPR=%u, CNR=%u\n", epit->cr, epit->lr, epit->cmpr, epit->cnr);
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

    irq_handler1 = enable_irq(EPIT1_IRQ_NUM, interrupt_ep);
    epit1 = (clock_register_t*)map_device((void*)EPIT1_BASE_PADDR, EPIT1_SIZE); 
    setup_epit(epit1);
    epit1->cr |= EPIT_CR_EN;
    epit1->lr = CLOCK_LOAD_VALUE;

    irq_handler2 = enable_irq(EPIT2_IRQ_NUM, interrupt_ep);
    epit2 = (clock_register_t*)map_device((void*)EPIT2_BASE_PADDR, EPIT2_SIZE); 
    setup_epit(epit2);
    epit2->cr &= ~EPIT_CR_EN;

    initialised = true;
    return CLOCK_R_OK;
}


int stop_timer(void){
    if (!initialised) return CLOCK_R_UINT;
    int err = 0;
    cspace_err_t cspace_err;
    
    /* Cleanup in seL4 kernel & cspace */
    epit1->cr &= ~EPIT_CR_EN;
    err = seL4_IRQHandler_Clear(irq_handler1);
    assert(!err);
    cspace_err = cspace_delete_cap(cur_cspace, irq_handler1);
    assert(cspace_err == CSPACE_NOERROR);

    epit2->cr &= ~EPIT_CR_EN;
    err = seL4_IRQHandler_Clear(irq_handler2);
    assert(!err);
    cspace_err = cspace_delete_cap(cur_cspace, irq_handler2);
    assert(cspace_err == CSPACE_NOERROR);

    initialised = false;
    return CLOCK_R_OK;
}

/* Check when the next timeout will happen
 * if the next timeout is less than the resolution
 * of our main timer interval, we use a second variable
 * timer for finer resolution.
 *
 * If the next timeout is not within the main timer's 
 * resolution , we will stop running till there is.
 * */
static void
update_var_timer(timestamp_t cur_time) {

   if(timers[0].registered && timers[0].endtime <= cur_time + CLOCK_INT_MILISEC){
     uint32_t counter = cur_time + CLOCK_INT_MILISEC - timers[0].endtime;
     epit2->cr |= EPIT_CR_EN;
     epit2->lr = counter;
   } else {
     /* stop var_timer */
     epit2->cr &= ~EPIT_CR_EN;
   }
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

    update_var_timer(cur_time);

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

    update_var_timer(time_stamp());

    assert(ntimers >= 0 && ntimers <= CLOCK_N_TIMERS);
    return CLOCK_R_OK;
}

/*
 * Check if there is a timeout. If there is, perform a callback
 * @Return: true if there is at least 1 timeout, false otherwise
 */
static void
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
}

int timer_interrupt(void) {
    if (!initialised) return CLOCK_R_UINT;

    timestamp_t cur_time = time_stamp();
    printf("timer interrupt at %lld\n", cur_time);

    check_timeout(cur_time);
    if (epit1->sr) {
        printf("epit1 called\n");
        jiffy += 1;
        epit1->sr = 1;
        int err = seL4_IRQHandler_Ack(irq_handler1);
        assert(!err);
    }
    if (epit2->sr) {
        printf("epit2 called\n");
        epit2->sr = 1;
        int err = seL4_IRQHandler_Ack(irq_handler2);
        assert(!err);
    }

    return CLOCK_R_OK;
}

timestamp_t time_stamp(void) {
    if (!initialised) return CLOCK_R_UINT;

    /*
     * Apart from jiffy counter, we need to take into account the current value
     * of the clock's counter EPIT_CNR. We also take care of the case when an
     * overflow has happened but haven't got acknowledged
     */
    uint64_t cnr_diff = CLOCK_LOAD_VALUE - epit1->cnr;
    uint64_t offset = (uint64_t)CLOCK_INT_MICROSEC*cnr_diff / CLOCK_LOAD_VALUE;
    return CLOCK_INT_MICROSEC * (jiffy + epit1->sr) + offset; 
}
