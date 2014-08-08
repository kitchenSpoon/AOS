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
//change this to
//timestamp_t jiffy; ?
uint64_t jiffy;

typedef struct {

} timer_register_t;


int start_timer(seL4_CPtr interrupte_ep){
    //register handler with kernel
    seL4_IRQHandler_SetEndpoint(interrupte_ep);

    //map device
    //if mapdevice fails, it panics
    vaddr * epit1_cr = map_device((*paddr)0x20D_0000, 4); 
    epit_cr = 0b000000_01_10_0_0_0_0_0_1_000000000001_1_1_0_1; //this is wrong, need to remove underscore
    //vaddr * epit1_sr = map_device((*paddr)0x20D_0004, 4); 
    vaddr * epit1_lr = map_device((*paddr)0x20D_0008, 4); 
    //epit_lr = 330;
    epit_lr = CLOCK_COMPARE_INTERVAL;
    vaddr * epit1_cmpr = map_device((*paddr)0x20D_000C, 4); 
    //epit_cmpr = 330;
    epit_cmpr = CLOCK_COMPARE_INTERVAL;
    //vaddr * epit1_cnr = map_device((*paddr)0x20D_0010, 4); 
    
    return CLOCK_R_OK;
}

int stop_timer(void){
    //map device
    //write value to device register
    vaddr * epit1_cr = map_device((*paddr)0x20D_0000, 4); 
    epit_cr = 0b000000_01_10_0_0_0_0_0_1_000000000001_1_1_0_0;
    //unmap device?

    //remove handler with kernel
    seL4_IRQHandler_Clear(interrupte_ep);
    
    return CLOCK_R_OK;
}

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
    // who is calling this function?
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
