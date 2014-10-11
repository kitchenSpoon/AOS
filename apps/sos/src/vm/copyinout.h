#ifndef _SOS_COPYINOUT_H_
#define _SOS_COPYINOUT_H_

/*
 * Copy memory from user virtual memory address to kernel's given address
 * @precond user data need to be valid
 * @precond kernel need to have enough memory to store the user data
 * @param kbuf - the kernel buffer address
 * @param buf - the user buffer address
 * @param nbyte - the size of user buffer memory need to be copied in
 */
typedef void (*copyin_cb_t)(void* token, int err);
int copyin(seL4_Word kbuf, seL4_Word buf, size_t nbyte, copyin_cb_t callback, void *token);

/*
 * Copy memory from kernel space to user's address space.
 * Will map pages they are valid but unmapped.
 * @precond user buffer is large enough and the memory is valid
 * @param buf - the user buffer address
 * @param kbuf - the kernel buffer address
 * @param nbyte - the size of kernel buffer memory need to be copied in
 */
int copyout(seL4_Word buf, seL4_Word kbuf, size_t nbyte);

#endif /* _SOS_COPYINOUT_H_ */
