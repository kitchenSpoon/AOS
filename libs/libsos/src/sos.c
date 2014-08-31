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

#define min(a, b) ((a) < (b) ? (a) : (b))
#define FAIL_TOLERANCE  10

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

size_t sos_write(void *vData, size_t count) {
    const char *realdata = vData;
    int tot_sent = 0;
    /*
     * We break the data into messages of size maximum seL4_MsgMaxLength-1
     * The first slot is used to indicate syscall number
     */
    int tries = 0;
    int expected_sending = count / (seL4_MsgMaxLength-1) + 1;
    while (tot_sent < count && tries < expected_sending*FAIL_TOLERANCE) {
        int len = min(count - tot_sent, seL4_MsgMaxLength-1);
        seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, 1+len);
        seL4_SetTag(tag);
        seL4_SetMR(0, 0); // Call syscall 0

        for (int i=0; i<len; i++) {
            seL4_SetMR(i+1, realdata[i+tot_sent]);
        }
        seL4_MessageInfo_t message = seL4_Call(SOS_IPC_EP_CAP, tag);
        int sent = (int)seL4_MessageInfo_get_label(message);
        if (sent < len) { /* some error handling? */ }
        tot_sent += sent;
        tries++;
    }

    return tot_sent;
}

void sos_sys_usleep(int msec) {
    assert(!"You need to implement this");
}

int64_t sos_sys_time_stamp(void) {
    assert(!"You need to implement this");
    return -1;
}

int sos_getdirent(int pos, char *name, size_t nbyte) {
    printf("System call not implemented\n");
    return -1;
}

int sos_stat(const char *path, sos_stat_t *buf) {
    printf("System call not implemented\n");
    return -1;
}

pid_t sos_process_create(const char *path) {
    printf("System call not implemented\n");
    return -1;
}

int sos_process_status(sos_process_t *processes, unsigned max) {
    printf("System call not implemented\n");
    return 0;
}

pid_t sos_process_wait(pid_t pid) {
    printf("System call not implemented\n");
    return -1;
}
