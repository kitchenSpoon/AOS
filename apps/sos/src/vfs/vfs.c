#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include "vfs/vfs.h"
#include "dev/console.h"

static
int _create_vnode(char *path, int openflags, struct vnode **ret, seL4_CPtr reply_cap) {
    int err;

    struct vnode *vn = malloc(sizeof(struct vnode));
    if (vn == NULL) {
        return ENOMEM;
    }

    int path_len = strlen(path);    // use strlen as path is a trustworthy string
    vn->vn_name = (char*)malloc(path_len+1);
    if (vn->vn_name == NULL) {
        free(vn);
        return ENOMEM;
    }
    strcpy(vn->vn_name, path);

    vn->vn_ops = malloc(sizeof(struct vnode_ops));
    if (vn->vn_ops == NULL) {
        free(vn->vn_name);
        free(vn);
        return ENOMEM;
    }

    vn->vn_opencount = 0;
    vn->initialised = false;

    if (strcmp(path, "console") == 0) {
        err = con_init(vn, reply_cap);
        if (err) {
            free(vn->vn_name);
            free(vn->vn_ops);
            free(vn);
            return err;
        }
    } else {
//        err = nfs_dev_init(vn, reply_cap);
//        if (err) {
//            free(vn->vn_name);
//            free(vn->vn_ops);
//            free(vn);
//            return err;
//        }
    }

    err = vfs_vnt_insert(vn);
    if (err) {
        VOP_LASTCLOSE(vn);
        free(vn->vn_name);
        free(vn->vn_ops);
        free(vn);
        return err;
    }

    *ret = vn;
    return 0;
}

int vfs_open(char *path, int openflags, struct vnode **ret, seL4_CPtr reply_cap) {

    int err;

    struct vnode *vn;
    vn = vfs_vnt_lookup(path);
    //TODO: Check if vnode is initialised of vn != NULL

    if (vn == NULL) {
        err = _create_vnode(path, openflags, &vn, reply_cap);
        if (err) {
            return err;
        }
        err = VOP_EACHOPEN(vn, openflags);
        if (err) {
            VOP_LASTCLOSE(vn);
            free(vn->vn_name);
            free(vn->vn_ops);
            free(vn);
            return err;
        }
    } else {
        err = VOP_EACHOPEN(vn, openflags);
        if (err) {
            free(vn->vn_name);
            free(vn->vn_ops);
            free(vn);
            return err;
        }
    }

    VOP_INCOPEN(vn);

    *ret = vn;
    return 0;
}

void vfs_close(struct vnode *vn, uint32_t flags) {
    VOP_EACHCLOSE(vn, flags);
    VOP_DECOPEN(vn);
}

struct vnode* vfs_vnt_lookup(const char *path) {
    if (path == NULL) {
        return NULL;
    }
    struct vnode_table_entry *vte = vnode_table_head;
    while (vte != NULL) {
        if (strcmp(path, vte->vte_vn->vn_name) == 0) {
            break;
        }
        vte = vte->next;
    }
    return vte->vte_vn;
}

int vfs_vnt_insert(struct vnode *vn) {
    if (vn == NULL || vn->vn_name == NULL) {
        return EINVAL;
    }
    struct vnode_table_entry *vte = malloc(sizeof(struct vnode_table_entry));
    if (vte == NULL) {
        return ENOMEM;
    }
    vte->vte_vn = vn;
    vte->next = vnode_table_head;
    vnode_table_head = vte;
    return 0;
}

void vfs_vnt_delete(const char *path) {
    if (path == NULL) {
        return;
    }
    if (vnode_table_head == NULL) {
        return;
    }
    if (strcmp(vnode_table_head->vte_vn->vn_name, path) == 0) {
        struct vnode_table_entry *next_vte = vnode_table_head->next;
        free(vnode_table_head);
        vnode_table_head = next_vte;
        return;
    }

    struct vnode_table_entry *vte = vnode_table_head->next;
    struct vnode_table_entry *prev = vnode_table_head;;
    while (vte != NULL) {
        if (strcmp(vte->vte_vn->vn_name, path) == 0) {
            break;
        }
        prev = vte;
        vte = vte->next;
    }

    /* Remove this vnode from the list */
    if (vte != NULL) {
        prev->next = vte->next;
        free(vte);
    }
}
