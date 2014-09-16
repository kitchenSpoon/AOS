#ifndef _SOS_DEVICE_H_
#define _SOS_DEVICE_H_

#include "vfs/vnode.h"

int con_init(struct vnode *con_vn);

int con_eachopen(struct vnode *file, int flags);
int con_eachclose(struct vnode *file, uint32_t flags);
int con_lastclose(struct vnode *file);
int con_read(struct vnode *file, char* buf, size_t nbytes, seL4_CPtr reply_cap);
int con_write(struct vnode *file, const char* buf, size_t nbytes, size_t *len);

#endif /* _SOS_DEVICE_H_ */
