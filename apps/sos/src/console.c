#include <stdlib.h>
#include <errno.h>
#include <assert.h>

#include "console.h"
#include "vnode.h"

int
create_con_vnode(struct vnode* vn) {
    if (vn == NULL) {
        vn = malloc(sizeof(struct vnode));
        if (vn == NULL) {
            return ENOMEM;
        }
        vn->vn_refcount  = 1;
        vn->vn_opencount = 1;
        vn->vn_data      = NULL;

        struct vnode_ops *vops = malloc(sizeof(struct vnode_ops));
        if (vops == NULL) {
            free(vn);
            return ENOMEM;
        }
        vops->vop_read  = NULL;
        vops->vop_write = NULL;

        vn->vn_ops = vops;
    }
    return 0;
}

int
destroy_con_vnode(struct vnode *vn) {
    if (vn == NULL) {
        return EINVAL;
    }
    /* If any of these fails, that means we have a bug */
    assert(vn->vn_ops != NULL);
    if (vn->vn_refcount != 1 || vn->vn_opencount != 1) {
        return EINVAL;
    }

    free(vn->vn_ops);
    free(vn);
    return 0;
}
