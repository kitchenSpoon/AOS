#ifndef _SOS_SYSCALL_H_
#define _SOS_SYSCALL_H_

#include "clock.h"

#define SOS_SYSCALL_PRINT       0
#define SOS_SYSCALL_SYSBRK      1
#define SOS_SYSCALL_OPEN        2
#define SOS_SYSCALL_CLOSE       3
#define SOS_SYSCALL_READ        4
#define SOS_SYSCALL_WRITE       5
#define SOS_SYSCALL_TIMESTAMP   6
#define SOS_SYSCALL_SLEEP       7

#define TIMESTAMP_LOW_MASK      (0x00000000ffffffffULL)
#define TIMESTAMP_HIGH_MASK     (0xffffffff00000000ULL)

int serv_sys_open(seL4_Word path, seL4_Word flags, seL4_Word* fd, seL4_Word nbyte);
int serv_sys_close(seL4_Word fd);
int serv_sys_read(seL4_Word fd, seL4_Word buf, seL4_Word nbyte, seL4_Word* len);
int serv_sys_write(seL4_Word fd, seL4_Word buf, seL4_Word nbyte, seL4_Word* len);

/* Syscall timestamp handler */
int serv_sys_timestamp(timestamp_t *ts);

int serv_sys_sleep(const int msec);
#endif /* _SOS_SYSCALL_H_ */
