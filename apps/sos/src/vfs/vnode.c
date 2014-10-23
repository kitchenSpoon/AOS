#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include "vfs/vfs.h"
#include "vfs/vnode.h"

/****************************************************************
 * Vnode Increase Open Count
 ***************************************************************/

void vnode_incopen(struct vnode *vn) {
    assert(vn != NULL);
    vn->vn_opencount++;
}

/****************************************************************
 * Vnode Decrease Open Count
 ***************************************************************/

void vnode_decopen(struct vnode *vn) {
    assert(vn != NULL);
    assert(vn->vn_opencount > 0);

    if (vn->vn_opencount > 1) {
        vn->vn_opencount--;
    } else {
        //result = VOP_RECLAIM(vn); // flush changes to the disk i-nodes

        VOP_LASTCLOSE(vn);
        vfs_vnt_delete(vn->vn_name);
        free(vn->vn_name);
        free(vn->vn_ops);
        free(vn);
    }
}
