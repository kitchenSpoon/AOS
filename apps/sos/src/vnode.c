#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include "vnode.h"

void vnode_incref(struct vnode *vn) {
    assert(vn != NULL);
    vn->vn_refcount++;
}

void vnode_decref(struct vnode *vn) {
    int result;
    (void)result;

    assert(vn != NULL);

    assert(vn->vn_refcount>0);
    if (vn->vn_refcount>1) {
        vn->vn_refcount--;
    } else {
        // flush changes to the disk i-nodes by calling reclaim
        // Also reclaim should free the vn_data as well
        //result = VOP_RECLAIM(vn);
        free(vn);
    }
}
