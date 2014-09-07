/*
 * An interface for syscalls handlers in SOS
 */

#ifndef _SOS_SYSCALL_H_
#define _SOS_SYSCALL_H_

#include "clock.h"

#define PROCESS_MAX_FILES       16

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

/* File syscalls */

/*
 * Print out to netcat port.
 * Because this is talking with a device so could be considered a file
 * syscall?!?
 * @param message - the data to be sent
 * @param len - the length of the message
 * @param sent - return the number of bytes sent
 * Returns 0 if successful
 */
int serv_sys_print(char* message, size_t len, size_t *sent);

/*
 * Open a file
 * @param path - pointer to string in user addresspace
 * @param nbyte - length of the string
 * @param flags - flags indication permission of this openfile
 * @param fd - the return filedescriptor
 * Returns 0 if successful
 */
int serv_sys_open(seL4_Word path, size_t nbyte, uint32_t flags, int* fd);

/*
 * Close a file
 * @param fd - the file descriptor of the file to be closed
 * Returns 0 if successful
 */
int serv_sys_close(int fd);

/*
 * Read data from a file
 * @param fd - the file descriptor of the file to be read.
 *             The file need to have read permission
 * @param buf - user level buffer pointer
 * @param nbyte - the size of *buf*
 * @param len - The length of the data read
 * Returns 0 if successful
 */
int serv_sys_read(seL4_CPtr reply_cap, int fd, seL4_Word buf, size_t nbyte);

/*
 * Write data to a file
 * @param fd - the file descriptor of the file to write to.
 *             The file need to have write permission
 * @param buf - user level buffer pointer
 * @param nbyte - the size of *buf*
 * @param len - The length of the data written to the file
 * Returns 0 if successful
 */
int serv_sys_write(int fd, seL4_Word buf, size_t nbyte, size_t* len);

/*
 * Syscall timestamp handler
 * @param ts - the timestamp returned
 * Returns 0 if successful
 */
int serv_sys_timestamp(timestamp_t *ts);

/*
 * Get the process to sleep for *msec* miliseconds
 * @param msec - The number of milisecond the process will sleep for
 * Returns 0 if successful
 */
int serv_sys_sleep(seL4_CPtr reply_cap, const int msec);
#endif /* _SOS_SYSCALL_H_ */
