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

#define verbose 5
// Block a thread forever
// we do this by making an unimplemented system call.
static void
thread_block(void){
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(0, 0, 0, 1);
    seL4_SetTag(tag);
    seL4_SetMR(0, 100);
    seL4_Call(SYSCALL_ENDPOINT_SLOT, tag);
}

#define NPAGES 27
/* called from pt_test */
static void
do_pt_test( char *buf )
{
    int i;

    /* set */
    for(i = 0; i < NPAGES; i ++)
        buf[i * 4096] = i;

    /* check */
    for(i = 0; i < NPAGES; i ++)
        assert(buf[i * 4096] == i);
}

static void
pt_test( void )
{
    /* need a decent sized stack */
    char buf1[NPAGES * 4096], *buf2 = NULL;

    /* check the stack is above phys mem */
    assert((void *) buf1 > (void *) 0x20000000);

    printf("Starting stack test..\n");
    /* stack test */
    do_pt_test(buf1);
    printf("Passed!\n");

    printf("Starting heap test...\n");
    /* heap test */
    buf2 = malloc(NPAGES * 4096);
    assert(buf2);
    do_pt_test(buf2);
    free(buf2);
    printf("Passed\n");
}

static void
readonly_test(void) {
    printf("Start readonly permission test..\n");
    printf("btw, The process will be killed so you won't see any more notice\n");
    int* addr = (int*)0x9000;

    *addr = 0x42;

    printf("You should not see this!!!\n");
}

#define STACK_START     (0x90000000)
#define STACK_SIZE      (1<<24)
static void
stack_overflow_test(void) {
    printf("Start stack overflow test..\n");
    printf("stacktop = 0x%08x, stackbase = 0x%08x\n", STACK_START, STACK_START-STACK_SIZE);

    int* addr = (int*)(STACK_START - (1<<24) + 4);
    printf("accessing addr = %p\n", addr);
    *addr = 0x42;

    addr = (int*)(STACK_START - STACK_SIZE - 4);
    printf("accessing addr = %p\n", addr);
    printf("Should die after this\n");
    *addr = 0x42;

    printf("NOOOO, test failed\n");
}

int main(void){
    /* initialise communication */
    ttyout_init();

    do {
        printf("task:\tHello world, I'm\ttty_test2!\n");
        //pt_test();
        //readonly_test();
        //stack_overflow_test();
        //thread_block();
        sleep(8);	// Implement this as a syscall
    } while(1);

    return 0;
}
