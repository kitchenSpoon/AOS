#ifndef _SOS_VNODE_H_
#define _SOS_VNODE_H_

#include <stdio.h>
struct vnode {
    int vn_refcount;                /* Reference count */
    int vn_opencount;
    void *vn_data;                  /* Filesystem-specific data */
    const struct vnode_ops *vn_ops; /* Functions on this vnode */
};

struct vnode_ops {
    int (*vop_read)(struct vnode *file, char* buf, size_t nbytes);
    int (*vop_write)(struct vnode *file, const char* buf, size_t nbytes);
};

/*
 * Reference count manipulation
 */
void vnode_incref(struct vnode *);
void vnode_decref(struct vnode *);

#endif /* _SOS_VNODE_H_ */
