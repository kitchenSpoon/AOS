#ifndef _SOS_VFS_H_
#define _SOS_VFS_H_

#include "vnode.h"

int vfs_open(char *path, int openflags, struct vnode **ret);
void vfs_close(struct vnode *vn, uint32_t flags);

#endif /* _SOS_VFS_H_ */
