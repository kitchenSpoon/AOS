#ifndef _SOS_VNODE_H_
#define _SOS_VNODE_H_

#include <stdio.h>
#include <sel4/sel4.h>
struct vnode {
    int vn_refcount;                /* Reference count */
    int vn_opencount;
    void *vn_data;                  /* Filesystem-specific data */
    struct vnode_ops *vn_ops; /* Functions on this vnode */
};

struct vnode_ops {
    int (*vop_eachopen)(struct vnode *file, int flags);
    int (*vop_lastclose)(struct vnode *file, int flags);
    int (*vop_read)(struct vnode *file, char* buf, size_t nbytes, size_t *len, seL4_CPtr reply_cap);
    int (*vop_write)(struct vnode *file, const char* buf, size_t nbytes, size_t *len);
};

#define __VOP(vn, sym) ((vn)->vn_ops->vop_##sym)

#define VOP_EACHOPEN(vn, flags)         (__VOP(vn, eachopen)(vn, flags))
#define VOP_LASTCLOSE(vn)               (__VOP(vn, lastclose)(vn))

#define VOP_READ(vn, uio)               (__VOP(vn, read)(vn, uio))
#define VOP_READ(vn, uio)               (__VOP(vn, read)(vn, uio))
/*
 * Reference count manipulation
 */
void vnode_incref(struct vnode *);
void vnode_decref(struct vnode *);

#endif /* _SOS_VNODE_H_ */
