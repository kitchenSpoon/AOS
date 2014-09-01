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

#define SOS_SYSCALL_PRINT       0
#define SOS_SYSCALL_SYSBRK      1
#define SOS_SYSCALL_OPEN        2
#define SOS_SYSCALL_CLOSE       3
#define SOS_SYSCALL_READ        4
#define SOS_SYSCALL_WRITE       5
#define SOS_SYSCALL_TIMESTAMP   6
#define SOS_SYSCALL_SLEEP       7

fildes_t sos_sys_open(const char *path, int flags) {
    assert(!"You need to implement this");
    //check path to path + min(endof path, MAX_IO_BUF) is mapped;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, 3);
    seL4_SetTag(tag);
    seL4_SetMR(0, SOS_SYSCALL_OPEN);
    seL4_SetMR(1, (seL4_Word)path);
    seL4_SetMR(2, flags);
    seL4_MessageInfo_t message = seL4_Call(SOS_IPC_EP_CAP, tag);
    int err = seL4_MessageInfo_get_label(message); 
    fildes_t fd = seL4_GetMR(0);
    return fd;
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
    seL4_MessageInfo_t send_tag;

    send_tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, 2);
    seL4_SetTag(send_tag);
    seL4_SetMR(0, SOS_SYSCALL_SLEEP);
    seL4_SetMR(1, msec);

    seL4_Call(SOS_IPC_EP_CAP, send_tag);
}

int64_t sos_sys_time_stamp(void) {
    seL4_MessageInfo_t send_tag, reply_tag;
    int err;
    uint64_t highbits, lowbits;
    int64_t timestamp;

    /* Prepare the request */
    send_tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, 1);
    seL4_SetTag(send_tag);
    seL4_SetMR(0, SOS_SYSCALL_TIMESTAMP);

    /* Send the request & wait for answer */
    reply_tag = seL4_Call(SOS_IPC_EP_CAP, send_tag);
    err = (int)seL4_MessageInfo_get_label(reply_tag);
    if (err) {
        return -1;
    }

    /* Extract the timestamp */
    highbits = (uint64_t)seL4_GetMR(1) << 32;
    lowbits  = (uint64_t)seL4_GetMR(0);
    timestamp = (int64_t)(highbits | lowbits);

    return timestamp;
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
int sos_process_delete(pid_t pid) {
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
