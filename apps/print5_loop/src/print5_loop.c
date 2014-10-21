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

#include <sos.h>

#define verbose 5

static int exec2(void) {
    pid_t pid;
    pid = sos_process_create("print5");
    printf("Child pid=%d\n", pid);
    sos_process_wait(pid);

    return 0;
}
int main(void){
    /* initialise communication */

    do {
        printf("task:\tHello world, I'm\ttty_test2!\n");
        exec2();
    } while(1);

    return 0;
}
