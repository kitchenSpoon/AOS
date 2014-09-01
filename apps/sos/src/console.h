#ifndef _SOS_DEVICE_H_
#define _SOS_DEVICE_H_
#include "vnode.h"

struct vnode* con_vnode;

int create_con_vnode(struct vnode* vn);
int destroy_con_vnode(struct vnode* vn);

#endif /* _SOS_DEVICE_H_ */
