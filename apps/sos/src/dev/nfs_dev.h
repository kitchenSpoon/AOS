#ifndef _SOS_NFS_DEV_H_
#define _SOS_NFS_DEV_H_

#include <nfs/nfs.h>

#include "vfs/vfs.h"
#include "vfs/vnode.h"

void nfs_dev_init(struct vnode* vn, vfs_open_cb_t callback, void *vfs_open_token);
int nfs_dev_init_mntpoint(struct vnode *vn, fhandle_t *mnt_point);
void nfs_dev_setup_timeout(void);
void nfs_dev_getstat(char *path, size_t path_len, sos_stat_t *buf, vfs_stat_cb_t callback, void *token);

#endif /* _SOS_NFS_DEV_H_ */
