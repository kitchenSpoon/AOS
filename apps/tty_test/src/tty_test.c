/*
 * Copyright 2014, NICTA
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(NICTA_BSD)
 */

/****************************************************************************
 *
 *      $Id:  $
 *
 *      Description: Simple milestone 0 test.
 *
 *      Author:			Godfrey van der Linden
 *      Original Author:	Ben Leslie
 *
 ****************************************************************************/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <sel4/sel4.h>

#include "ttyout.h"
//#include <clock/clock.h>

// Block a thread forever
// we do this by making an unimplemented system call.
static void
thread_block(void){
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(0, 0, 0, 1);
    seL4_SetTag(tag);
    seL4_SetMR(0, 1);
    seL4_Call(SYSCALL_ENDPOINT_SLOT, tag);
}

/*
static void
cb(uint32_t id, void* data) {
    printf("timer %d called backed!!!\n", id);
    //printf("mess sent was: %s\n", (char*)data);
    //printf("current timestamp is: %lld\n", (long long)time_stamp());
}
*/

int main(void){
    /* initialise communication */
    ttyout_init();

    printf("task:\tHello world, I'm\ttty_test!\n");
    do {
	//printf("wtasdfasdf\n");
	    
        //printf("task:\tHello world, I'm\ttty_test!\n");
	/*
        int t = 5;
        while (t--) {
            register_timer(100, cb, NULL);
        }
	*/
        //thread_block();
        // sleep(1);	// Implement this as a syscall
    } while(1);

    return 0;
}
