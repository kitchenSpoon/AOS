#include <errno.h>
#include <string.h>

#include "vfs/vfs.h"
#include "dev/console.h"

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
        // should check some global variable 
        // to determine which filesystem is mounted
        // and we should then use something similiar to
        // fs->lookup 
        // if lookup fail we create the file on nfs 
        // 
        // Then we take the filehandle return by lookup/create
        // and then create a vnode
        // if()vn = create_nfs_vnode(fhandle_t);
        //
        // When we mount nfs, we will be given a fhandle_t to the root
        // of the filesystem, we will have to save it somewhere
        vn = malloc(sizeof(struct vnode));
        if(vn == NULL)
            return ENOMEM;
        //need to copy path to name, this is wrong
        vn->name = path;
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
