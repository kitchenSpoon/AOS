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
#include <unistd.h>


#include "ttyout.h"

#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <utils/time.h>

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


static int micro_time(int argc, char *argv[]) {
    struct timeval time;
    gettimeofday(&time, NULL);
    uint64_t micros = (uint64_t)time.tv_sec * US_IN_S + (uint64_t)time.tv_usec;
    printf("%llu microseconds since boot\n", micros);
    return 0;
}

static uint64_t getmicro_time() {
    struct timeval time;
    gettimeofday(&time, NULL);
    uint64_t micros = (uint64_t)time.tv_sec * US_IN_S + (uint64_t)time.tv_usec;
    return micros;
}


static void bm_read(char* filename, char* buf, size_t buf_size){
    printf("Reading with IO buf request %u\n", buf_size);

    int fd = open(filename, O_RDONLY);
    assert(fd >= 0);
    uint64_t start_time = getmicro_time();

    read(fd, buf, buf_size);

    uint64_t end_time = getmicro_time();
    printf("time taken: %lu\n", (long unsigned)(end_time - start_time));
    close(fd);
}

static void bm_write(char* filename, char* buf, size_t buf_size){
    printf("Writing with IO buf request %u\n", buf_size);

    int fd = open(filename, O_WRONLY);
    assert(fd >= 0);
    uint64_t start_time = getmicro_time();

    size_t written = 0;
    while (written < buf_size){
        written += write(fd, buf+written, buf_size-written);
    }

    uint64_t end_time = getmicro_time();
    printf("time taken: %lu\n", (long unsigned)(end_time - start_time));
    close(fd);
}

static int
benchmark(){
    /* Reads */
    printf("Reading\n");
    /* Reading with IO request changing*/
    printf("Reading with IO request changing\n");
    char buf1000[1001];
    char buf5000[5001];
    char buf10000[10000];
    char buf50000[50000];
    char buf100000[100000];
    char buf200000[200000];
    char buf400000[400000];
    char buf800000[800000];
    char buf1600000[1600000];
    char buf3200000[3200000];
    char buf100000_2[100000];
    char buf200000_2[200000];
    char buf400000_2[400000];
    bm_read("read_test_1000", (char*)buf1000,1000);
    bm_write("write_test_1000", (char*)buf1000,1000);

    bm_read("read_test_5000", (char*)buf5000,5000);
    bm_write("write_test_5000", (char*)buf5000,5000);

    bm_read("read_test_10000", (char*)buf10000,10000);
    bm_write("write_test_10000", (char*)buf10000,10000);

    bm_read("read_test_50000", (char*)buf50000,50000);
    bm_write("write_test_50000", (char*)buf50000,50000);

    bm_read("read_test_100000", (char*)buf100000,100000);
    bm_write("write_test_100000", (char*)buf100000,100000);

    bm_read("read_test_200000", (char*)buf200000,200000);
    bm_write("write_test_200000", (char*)buf200000,200000);

    bm_read("read_test_400000", (char*)buf400000,400000);
    bm_write("write_test_400000", (char*)buf400000,400000);

    bm_read("read_test_800000", (char*)buf800000,800000);
    bm_write("write_test_800000", (char*)buf800000,800000);

    bm_read("read_test_1600000", (char*)buf1600000,1600000);
    bm_write("write_test_1600000", (char*)buf1600000, 1600000);

    bm_read("read_test_3200000", (char*)buf3200000,3200000);
    bm_write("write_test_3200000", (char*)buf3200000, 3200000);

    bm_read("read_test_100000", (char*)buf100000_2,100000);
    bm_write("write_test_100000_2", (char*)buf100000_2,100000);

    bm_read("read_test_200000", (char*)buf200000_2,200000);
    bm_write("write_test_200000_2", (char*)buf200000_2,200000);

    bm_read("read_test_400000", (char*)buf400000_2,400000);
    bm_write("write_test_400000_2", (char*)buf400000_2,400000);

    /* Reading with packet changing */

    /* Writes */
    printf("Writing\n");
    /* Writing with IO request changing*/
    /* Writing with packet changing */
    return 0;
}


int main(void){
    /* initialise communication */
    ttyout_init();

    do {
        printf("task:\tHello world, I'm\ttty_test!\n");
        benchmark();
        //pt_test();
        //readonly_test();
        //stack_overflow_test();
        //thread_block();
        //sleep(8);	// Implement this as a syscall
    } while(1);

    return 0;
}
