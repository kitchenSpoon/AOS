#ifndef _SOS_CONSOLE_H_
#define _SOS_CONSOLE_H_

#include "syscall/syscall.h"
#include "vfs/vnode.h"

int con_init(struct vnode *con_vn);

#endif /* _SOS_CONSOLE_H_ */
