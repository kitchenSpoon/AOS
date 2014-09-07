#ifndef _SOS_VFS_H_
#define _SOS_VFS_H_

int vfs_open(char *path, int openflags, struct vnode **ret);
void vfs_close(struct vnode *vn);

#endif /* _SOS_VFS_H_ */
