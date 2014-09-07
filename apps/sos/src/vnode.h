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
    int (*vop_open)(struct vnode *file, int flags);
    int (*vop_close)(struct vnode *file);
    int (*vop_read)(struct vnode *file, char* buf, size_t nbytes, seL4_CPtr reply_cap);
    int (*vop_write)(struct vnode *file, const char* buf, size_t nbytes, size_t *len);
};

/*
 * Reference count manipulation
 */
void vnode_incref(struct vnode *);
void vnode_decref(struct vnode *);

#endif /* _SOS_VNODE_H_ */
