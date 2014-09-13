#ifndef _SOS_DEVICE_H_
#define _SOS_DEVICE_H_

#include "vfs/vnode.h"

struct vnode* con_vnode;

int nfs_init(void);
int nfs_destroy_vnode(void);

int nfs_eachopen(struct vnode *file, int flags);
int nfs_eachclose(struct vnode *file, uint32_t flags);
int nfs_lastclose(struct vnode *file);
int nfs_read(struct vnode *file, char* buf, size_t nbytes, seL4_CPtr reply_cap);
int nfs_write(struct vnode *file, const char* buf, size_t nbytes, size_t *len);

#endif /* _SOS_DEVICE_H_ */
