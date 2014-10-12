#ifndef _SOS_CONSOLE_H_
#define _SOS_CONSOLE_H_

#include "syscall/syscall.h"
#include "vfs/vnode.h"

typedef void (*copyout_cb_t)(void* token, int err);
int con_init(struct vnode *con_vn);

#endif /* _SOS_CONSOLE_H_ */
