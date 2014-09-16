#ifndef _SOS_DEVICE_H_
#define _SOS_DEVICE_H_

#include "vfs/vnode.h"

int nfs_dev_init(struct vnode* vn, seL4_CPtr reply_cap);
int nfs_dev_init_mntpoint(struct vnode *vn, fhandle_t *mnt_point);
void nfs_dev_setup_timeout(void);

int nfs_dev_eachopen(struct vnode *file, int flags);
int nfs_dev_eachclose(struct vnode *file, uint32_t flags);
int nfs_dev_lastclose(struct vnode *file);
int nfs_dev_read(struct vnode *file, char* buf, size_t nbytes, seL4_CPtr reply_cap);
int nfs_dev_write(struct vnode *file, const char* buf, size_t nbytes, size_t *len);
int nfs_dev_getdirent(struct vnode *dir, char *buf, size_t nbyte,
                         int pos, seL4_CPtr reply_cap);

#endif /* _SOS_DEVICE_H_ */
