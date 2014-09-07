#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include "vnode.h"

void vnode_incopen(struct vnode *vn) {
    assert(vn != NULL);
    vn->vn_opencount++;
}

void vnode_decopen(struct vnode *vn) {
    assert(vn != NULL);
    assert(vn->vn_opencount > 0);

    if (vn->vn_opencount > 1) {
        vn->vn_opencount--;
    } else {
        // flush changes to the disk i-nodes by calling reclaim
        // Also reclaim should free the vn_data as well
        //result = VOP_RECLAIM(vn);
        VOP_LASTCLOSE(vn);
    }
}
