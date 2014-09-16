#ifndef _SOS_VNODE_H_
#define _SOS_VNODE_H_

#include <stdio.h>
#include <sel4/sel4.h>
#include "syscall/syscall.h"

struct vnode {
    bool initialised;
    int vn_opencount;
    char *vn_name;
    void *vn_data;                  /* Filesystem-specific data */
    struct vnode_ops *vn_ops; /* Functions on this vnode */
};

struct vnode_ops {
    int (*vop_eachopen)(struct vnode *file, int flags);
    int (*vop_eachclose)(struct vnode *file, uint32_t flags);
    int (*vop_lastclose)(struct vnode *file);   // lastclose cleans up the vn_data
    void (*vop_read)(struct vnode *file, char* buf, size_t nbytes, size_t offset,
                            serv_sys_read_cb_t callback, void *token);
    void (*vop_write)(struct vnode *file, const char* buf, size_t offset, size_t nbytes,
                            serv_sys_write_cb_t callback, void *token);
    void (*vop_getdirent)(struct vnode *dir, char *buf, size_t nbyte,
                          int pos, serv_sys_getdirent_cb_t callback, void *token);
    //int (*vop_stat)(struct vnode *file, sos_stat_t *buf);
};

#define __VOP(vn, sym) ((vn)->vn_ops->vop_##sym)

#define VOP_EACHOPEN(vn, flags)                     (__VOP(vn, eachopen)(vn, flags))
#define VOP_EACHCLOSE(vn, flags)                    (__VOP(vn, eachclose)(vn, flags))
#define VOP_LASTCLOSE(vn)                           (__VOP(vn, lastclose)(vn))

#define VOP_READ(vn, buf, nbytes, offset, callback, token)        (__VOP(vn, read)(vn, buf, nbytes, offset, callback, token))
#define VOP_WRITE(vn, buf, nbyte, offset, callback, token)              (__VOP(vn, write)(vn, buf, nbyte, offset, callback, token))
#define VOP_GETDIRENT(vn, buf, nbyte, pos, callback, token) (__VOP(vn, getdirent)(vn, buf, nbyte, pos, callback, token))
#define VOP_STAT(vn, buf)                           (__VOP(vn, stat)(vn, buf))

/*
 * Reference count manipulation
 */
void vnode_incopen(struct vnode *);
void vnode_decopen(struct vnode *);
#define VOP_INCOPEN(vn)             vnode_incopen(vn)
#define VOP_DECOPEN(vn)             vnode_decopen(vn)

#endif /* _SOS_VNODE_H_ */
