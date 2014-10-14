#ifndef _SOS_NFS_DEV_H_
#define _SOS_NFS_DEV_H_

#include <nfs/nfs.h>

#include "vfs/vnode.h"

typedef void (*nfs_dev_init_cb_t)(void* token, int err);
typedef void (*nfs_dev_stat_cb_t)(void *token, int err);

void nfs_dev_init(struct vnode *vn, nfs_dev_init_cb_t callback, void *token);
int  nfs_dev_init_mntpoint_vnode(struct vnode *vn, fhandle_t *mnt_point);
void nfs_dev_setup_timeout(void);
void nfs_dev_getstat(char *path, size_t path_len, sos_stat_t *buf, nfs_dev_stat_cb_t callback, void *token);
int  nfs_dev_get_fhandle(struct vnode *vn, fhandle_t **fh);


#endif /* _SOS_NFS_DEV_H_ */
