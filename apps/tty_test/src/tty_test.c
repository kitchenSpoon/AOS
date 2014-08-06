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

// Block a thread forever
// we do this by making an unimplemented system call.
static void
thread_block(void){
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(0, 0, 0, 1);
    seL4_SetTag(tag);
    seL4_SetMR(0, 1);
    seL4_Call(SYSCALL_ENDPOINT_SLOT, tag);
}

int main(void){
    /* initialise communication */
    ttyout_init();

    do {
        for (int i=0; i<100; i++) {
            printf("trying %d time\n", i);
            printf("task:\tHello world, I'm\ttty_test!\n");
            printf("10000000002000000000300000000040000000005000000000600000000070000000008000000000900000000011000000001200000000130000000014000000001500000000160000000017000000001800000000\n");
        }
        thread_block();
        // sleep(1);	// Implement this as a syscall
    } while(1);

    return 0;
}
