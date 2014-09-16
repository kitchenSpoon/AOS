#ifndef _SOS_DEVICE_H_
#define _SOS_DEVICE_H_

#include <nfs/nfs.h>

#include "vfs/vfs.h"
#include "vfs/vnode.h"

void nfs_dev_init(struct vnode* vn, vfs_open_cb_t callback, void *vfs_open_token);
int nfs_dev_init_mntpoint(struct vnode *vn, fhandle_t *mnt_point);
void nfs_dev_setup_timeout(void);

#endif /* _SOS_DEVICE_H_ */
