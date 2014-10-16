#ifndef _SOS_NFS_DEV_H_
#define _SOS_NFS_DEV_H_

#include <nfs/nfs.h>

#include "vfs/vnode.h"

typedef void (*nfs_dev_init_cb_t)(void* token, int err);
typedef void (*nfs_dev_stat_cb_t)(void *token, int err);

/* Populating the data in the vnode */
void nfs_dev_init(struct vnode *vn, nfs_dev_init_cb_t callback, void *token);

/* Initialize the mountpoint's vnode, used to initialise the NFS file system */
int  nfs_dev_init_mntpoint_vnode(struct vnode *vn, fhandle_t *mnt_point);

/* Setup timeout for NFS to pick up packets */
void nfs_dev_setup_timeout(void);

/* This function get stat directly from the path instead of from vnode like
 * VOP_STAT */
void nfs_dev_getstat(char *path, size_t path_len, sos_stat_t *buf, nfs_dev_stat_cb_t callback, void *token);

/* Given a vnode, get the NFS fhandle_t for the file referenced by that vnode
 * This is done in constant time */
int  nfs_dev_get_fhandle(struct vnode *vn, fhandle_t **fh);


#endif /* _SOS_NFS_DEV_H_ */
