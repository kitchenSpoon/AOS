#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include "vnode.h"

void vnode_incopen(struct vnode *vn) {
    assert(vn != NULL);
    vn->vn_refcount++;
}

void vnode_decopen(struct vnode *vn) {
    assert(vn != NULL);
    assert(vn->vn_refcount > 0);

    if (vn->vn_refcount > 1) {
        vn->vn_refcount--;
    } else {
        // flush changes to the disk i-nodes by calling reclaim
        // Also reclaim should free the vn_data as well
        //result = VOP_RECLAIM(vn);
        VOP_LASTCLOSE(vn);
    }
}
