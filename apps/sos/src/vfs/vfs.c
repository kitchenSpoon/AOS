#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include "vfs/vfs.h"
#include "dev/nfs_dev.h"
#include "dev/console.h"

typedef struct {
    file_open_cb_t callback;
    void *file_open_token;
    struct vnode *vn;
    int openflags;
} cont_vfs_open_t;

static void
vfs_open_3_end_create(void* vfs_open_token, int err) {
    printf("vfs_open_3_end_create called\n");
    assert(vfs_open_token != NULL);

    cont_vfs_open_t *cont = (cont_vfs_open_t*)vfs_open_token;
    cont_vfs_open_t local_cont = *cont;
    free(cont);

    if (err || VOP_EACHOPEN(local_cont.vn, local_cont.openflags)) {
        free(local_cont.vn->vn_name);
        free(local_cont.vn->vn_ops);
        free(local_cont.vn);
        local_cont.callback(local_cont.file_open_token, err, NULL);
        return;
    }

    err = vfs_vnt_insert(local_cont.vn);
    if (err) {
        VOP_LASTCLOSE(local_cont.vn);
        free(local_cont.vn->vn_name);
        free(local_cont.vn->vn_ops);
        free(local_cont.vn);
        local_cont.callback(local_cont.file_open_token, err, NULL);
        return;
    }

    local_cont.vn->vn_opencount = 1;
    local_cont.callback(local_cont.file_open_token, 0, local_cont.vn);
}

static void
vfs_open_2_create_vnode(char *path, int openflags, file_open_cb_t callback, void *file_open_token) {
    printf("vfs_open_2 called\n");
    int err;

    struct vnode *vn = malloc(sizeof(struct vnode));
    if (vn == NULL) {
        callback(file_open_token, ENOMEM, NULL);
        return;
    }

    int path_len = strlen(path);    // use strlen as path is a trustworthy string
    vn->vn_name = (char*)malloc(path_len+1);
    if (vn->vn_name == NULL) {
        free(vn);
        callback(file_open_token, ENOMEM, NULL);
        return;
    }
    strcpy(vn->vn_name, path);

    vn->vn_ops = malloc(sizeof(struct vnode_ops));
    if (vn->vn_ops == NULL) {
        free(vn->vn_name);
        free(vn);
        callback(file_open_token, ENOMEM, NULL);
        return;
    }

    vn->initialised = false;

    cont_vfs_open_t *cont = malloc(sizeof(cont_vfs_open_t));
    if (cont == NULL) {
        free(vn->vn_name);
        free(vn->vn_ops);
        free(vn);
        callback(file_open_token, ENOMEM, NULL);
        return;
    }
    cont->callback        = callback;
    cont->file_open_token = file_open_token;
    cont->vn              = vn;
    cont->openflags       = openflags;

    if (strcmp(path, "console") == 0) {
        err = con_init(vn);
        if (err) {
            free(vn->vn_name);
            free(vn->vn_ops);
            free(vn);
            vn = NULL;
        }
        vfs_open_3_end_create((void*)cont, 0);
        return;
    }

    nfs_dev_init(vn, vfs_open_3_end_create, (void*)cont);
}

void vfs_open(char *path, int openflags, file_open_cb_t callback, void *file_open_token) {
    printf("vfs_open called\n");
    int err;

    struct vnode *vn;
    vn = vfs_vnt_lookup(path);
    if (vn != NULL) {
        err = VOP_EACHOPEN(vn, openflags);
        if (err) {
            vn = NULL;
        } else {
            VOP_INCOPEN(vn);
        }
        callback((void*)file_open_token, err, vn);
        return;
    }

    vfs_open_2_create_vnode(path, openflags, callback, file_open_token);
}

void vfs_close(struct vnode *vn, uint32_t flags) {
    printf("vfs_close called\n");
    VOP_EACHCLOSE(vn, flags);
    //VOP_EACHOPEN(vn, flags);
    //vn->vn_ops->vop_eachclose(vn, flags);
    printf("vfs_close 1\n");
    VOP_DECOPEN(vn);
    printf("vfs_close 2\n");
}

struct vnode* vfs_vnt_lookup(const char *path) {
    if (path == NULL) {
        return NULL;
    }
    //printf("vnode_table_head = %p\n", vnode_table_head);
    struct vnode_table_entry *vte = vnode_table_head;
    while (vte != NULL) {
        //printf("vte->vte_vn->vn_name = %s\n", vte->vte_vn->vn_name);
        if (strcmp(path, vte->vte_vn->vn_name) == 0) {
            break;
        }
        vte = vte->next;
    }
    return (vte == NULL) ? NULL : vte->vte_vn;
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
