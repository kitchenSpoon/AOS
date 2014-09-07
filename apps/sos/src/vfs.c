#include <errno.h>
#include <string.h>

#include "vfs.h"
#include "console.h"

int vfs_open(char *path, int openflags, struct vnode **ret) {
    int err;

    struct vnode *vn;
    /* Extract the Vnode related to this path */
    if (strcmp(path, "console") == 0) {
        err = con_init();
        if (err) {
            return err;
        }
        vn = con_vnode;
    } else {
        // Don't handle it for now
        vn = NULL;
        return EFAULT;
    }
    err = VOP_EACHOPEN(vn, openflags);
    if (err) {
        return err;
    }

    VOP_INCOPEN(vn);

    *ret = vn;
    return 0;
}

void vfs_close(struct vnode *vn, uint32_t flags) {
    VOP_EACHCLOSE(vn, flags);
    VOP_DECOPEN(vn);
}
