#ifndef _SOS_DEVICE_H_
#define _SOS_DEVICE_H_
#include "vnode.h"

struct vnode* con_vnode;

int con_init(void);
int con_destroy_vnode(void);

int con_open(struct vnode *file, int flags);
int con_close(struct vnode *file, int flags);
int con_read(struct vnode *file, char* buf, size_t nbytes, size_t *len, seL4_CPtr reply_cap);
int con_write(struct vnode *file, const char* buf, size_t nbytes, size_t *len);

#endif /* _SOS_DEVICE_H_ */
