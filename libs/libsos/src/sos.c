/*
 * Copyright 2014, NICTA
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(NICTA_BSD)
 */

#include <stdarg.h>
#include <assert.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <sos.h>

#include <sel4/sel4.h>

fildes_t sos_sys_open(const char *path, int flags) {
    assert(!"You need to implement this");
    return -1;
}

int sos_sys_read(fildes_t file, char *buf, size_t nbyte) {
    assert(!"You need to implement this");
    return -1;
}

int sos_sys_write(fildes_t file, const char *buf, size_t nbyte) {
    assert(!"You need to implement this");
    return -1;
}

void sos_sys_usleep(int msec) {
    assert(!"You need to implement this");
}

int64_t sos_sys_time_stamp(void) {
    assert(!"You need to implement this");
    return -1;
}

