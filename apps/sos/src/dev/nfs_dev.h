#ifndef _SOS_DEVICE_H_
#define _SOS_DEVICE_H_

#include "vfs/vnode.h"

int nfs_dev_init(struct vnode*, seL4_CPtr reply_cap);

int nfs_dev_eachopen(struct vnode *file, int flags);
int nfs_dev_eachclose(struct vnode *file, uint32_t flags);
int nfs_dev_lastclose(struct vnode *file);
int nfs_dev_read(struct vnode *file, char* buf, size_t nbytes, seL4_CPtr reply_cap);
int nfs_dev_write(struct vnode *file, const char* buf, size_t nbytes, size_t *len);

#endif /* _SOS_DEVICE_H_ */
