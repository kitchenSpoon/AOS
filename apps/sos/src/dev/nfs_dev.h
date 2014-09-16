#ifndef _SOS_DEVICE_H_
#define _SOS_DEVICE_H_

#include "vfs/vnode.h"

int nfs_dev_init(struct vnode* vn, seL4_CPtr reply_cap);
int nfs_dev_init_mntpoint(struct vnode *vn, fhandle_t *mnt_point);
void nfs_dev_setup_timeout(void);

#endif /* _SOS_DEVICE_H_ */
