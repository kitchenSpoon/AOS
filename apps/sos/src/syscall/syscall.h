/*
 * An interface for syscalls handlers in SOS
 */

#ifndef _SOS_SYSCALL_H_
#define _SOS_SYSCALL_H_

#include "dev/clock.h"

#define PROCESS_MAX_FILES       16

#define SOS_SYSCALL_PRINT       0
#define SOS_SYSCALL_SYSBRK      1
#define SOS_SYSCALL_OPEN        2
#define SOS_SYSCALL_CLOSE       3
#define SOS_SYSCALL_READ        4
#define SOS_SYSCALL_WRITE       5
#define SOS_SYSCALL_TIMESTAMP   6
#define SOS_SYSCALL_SLEEP       7

/* File syscalls */

/*
 * Print out to netcat port.
 * Because this is talking with a device so could be considered a file
 * syscall ?!?
 * @param message - the data to be sent
 * @param len - the length of the message
 */
void serv_sys_print(seL4_CPtr reply_cap, char* message, size_t len);

/*
 * Open a file
 * @param path - pointer to string in user addresspace
 * @param nbyte - length of the string
 * @param flags - flags indication permission of this openfile
 */
void serv_sys_open(seL4_CPtr reply_cap, seL4_Word path, size_t nbyte, uint32_t flags);

/*
 * Close a file
 * @param fd - the file descriptor of the file to be closed
 */
void serv_sys_close(seL4_CPtr reply_cap, int fd);

/*
 * Read data from a file
 * @param fd - the file descriptor of the file to be read.
 *             The file need to have read permission
 * @param buf - user level buffer pointer
 * @param nbyte - the size of *buf*
 */
void serv_sys_read(seL4_CPtr reply_cap, int fd, seL4_Word buf, size_t nbyte);

/*
 * Write data to a file
 * @param fd - the file descriptor of the file to write to.
 *             The file need to have write permission
 * @param buf - user level buffer pointer
 * @param nbyte - the size of *buf*
 */
void serv_sys_write(seL4_CPtr reply_cap, int fd, seL4_Word buf, size_t nbyte);

/*
 * Syscall timestamp handler
 * @param ts - the timestamp returned
 */
void serv_sys_timestamp(seL4_CPtr reply_cap);

/*
 * Get the process to sleep for *msec* miliseconds
 * @param msec - The number of milisecond the process will sleep for
 */
void serv_sys_sleep(seL4_CPtr reply_cap, const int msec);

/*
 * Change the system's break to newbrk
 */
void serv_sys_sbrk(seL4_CPtr reply_cap, seL4_Word newbrk);

#endif /* _SOS_SYSCALL_H_ */