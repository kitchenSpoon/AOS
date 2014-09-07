#include <errno.h>
#include "vfs.h"

int vfs_open(char *path, int openflags, struct vnode **ret) {
    int err;

    /* Extract the Vnode related to this path */
    if (strcmp(path, "console") == 0) {
        err = con_init();
        if (err) {
            return err;
        }
        *ret = con_vnode;
    } else {
        // Don't handle it for now
        *ret = NULL;
        return EFAULT;
    }
    err = 

    return 0;
}

void vfs_close(struct vnode *vn) {
}
