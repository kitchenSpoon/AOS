#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include "vfs/vfs.h"
#include "dev/nfs_dev.h"
#include "dev/console.h"

/****************************************************************
 * VFS Open
 ***************************************************************/

typedef struct {
    vfs_open_cb_t callback;
    void *token;
    struct vnode *vn;
    int openflags;
} cont_vfs_open_t;

static void _vfs_open_3_end_create(void* vfs_open_token, int err);
static void _vfs_open_2_create_vnode(char *path, int openflags, vfs_open_cb_t callback, void *token);

void vfs_open(char *path, int openflags, vfs_open_cb_t callback, void *token) {
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
        callback((void*)token, err, vn);
        return;
    }

    _vfs_open_2_create_vnode(path, openflags, callback, token);
}

static void
_vfs_open_2_create_vnode(char *path, int openflags, vfs_open_cb_t callback, void *token) {
    printf("vfs_open_2 called\n");
    int err;

    struct vnode *vn = malloc(sizeof(struct vnode));
    if (vn == NULL) {
        callback(token, ENOMEM, NULL);
        return;
    }

    int path_len = strlen(path);    // use strlen as path is a trustworthy string
    vn->vn_name = (char*)malloc(path_len+1);
    if (vn->vn_name == NULL) {
        free(vn);
        callback(token, ENOMEM, NULL);
        return;
    }
    strcpy(vn->vn_name, path);

    vn->vn_ops = malloc(sizeof(struct vnode_ops));
    if (vn->vn_ops == NULL) {
        free(vn->vn_name);
        free(vn);
        callback(token, ENOMEM, NULL);
        return;
    }

    vn->initialised = false;

    cont_vfs_open_t *cont = malloc(sizeof(cont_vfs_open_t));
    if (cont == NULL) {
        free(vn->vn_name);
        free(vn->vn_ops);
        free(vn);
        callback(token, ENOMEM, NULL);
        return;
    }
    cont->callback        = callback;
    cont->token           = token;
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
        _vfs_open_3_end_create((void*)cont, 0);
        return;
    }

    nfs_dev_init(vn, _vfs_open_3_end_create, (void*)cont);
}

static void
_vfs_open_3_end_create(void* vfs_open_token, int err) {
    printf("_vfs_open_3_end_create called\n");
    //assert(vfs_open_token != NULL);

    cont_vfs_open_t *cont = (cont_vfs_open_t*)vfs_open_token;

    if (err || VOP_EACHOPEN(cont->vn, cont->openflags)) {
        free(cont->vn->vn_name);
        free(cont->vn->vn_ops);
        free(cont->vn);
        cont->callback(cont->token, err, NULL);
        free(cont);
        return;
    }

    err = vfs_vnt_insert(cont->vn);
    if (err) {
        VOP_LASTCLOSE(cont->vn);
        free(cont->vn->vn_name);
        free(cont->vn->vn_ops);
        free(cont->vn);
        cont->callback(cont->token, err, NULL);
        free(cont);
        return;
    }

    cont->vn->vn_opencount = 1;
    cont->callback(cont->token, 0, cont->vn);
    free(cont);
}

/****************************************************************
 * VFS Close
 ***************************************************************/

void vfs_close(struct vnode *vn, uint32_t flags) {
    VOP_EACHCLOSE(vn, flags);
    VOP_DECOPEN(vn);
}

/****************************************************************
 * VFS Stat
 ***************************************************************/
 
 typedef struct {
    vfs_stat_cb_t callback;
    void *token;
} cont_vfs_stat_t;

static void _vfs_stat_end(void *token, int err);

void vfs_stat(char* path, size_t path_len, sos_stat_t *buf, vfs_stat_cb_t callback, void *token){
    printf("vfs_stat called\n");
    int err = 0;

    cont_vfs_stat_t *cont = malloc(sizeof(cont_vfs_stat_t));
    if (cont == NULL) {
        callback(token, ENOMEM);
        return;
    }
    cont->callback = callback;
    cont->token = token;

    struct vnode *vn = vfs_vnt_lookup(path);
    if (vn != NULL) {
    printf("Going to call nfs_dev_getstat 1\n");
        err = VOP_STAT(vn, buf, _vfs_stat_end, (void*)cont);
        if(err){
            _vfs_stat_end((void*)cont, err);
        }
        return;
    }
    printf("Going to call nfs_dev_getstat 2\n");
    nfs_dev_getstat(path, path_len, buf, _vfs_stat_end, (void*)cont);
}

static void 
_vfs_stat_end(void *token, int err) {
    printf("_vfs_stat_end called\n");
    cont_vfs_stat_t *cont = (cont_vfs_stat_t*)token;
    printf("_vfs_stat_end called 2\n");
    //assert(cont != NULL);
    
    printf("_vfs_stat_end called 3\n");
    if(cont->token == NULL || cont->callback == NULL){
        printf("_vfs_stat_end called 3.25\n");
    }
    printf("_vfs_stat_end called 3.5\n");
    cont->callback(cont->token, err);
    printf("_vfs_stat_end called 4\n");
    free(cont);
    printf("_vfs_stat_end called 5\n");
}

/****************************************************************
 * VFS Vnode table functions
 ***************************************************************/

struct vnode* vfs_vnt_lookup(const char *path) {
    if (path == NULL) {
        return NULL;
    }
    //printf("vfs_vnt_lookup path = %s\n", path);
    //printf("vnode_table_head = %p\n", vnode_table_head);
    struct vnode_table_entry *vte = vnode_table_head;
    while (vte != NULL) {
        //printf("vte->vte_vn->vn_name = %s\n", vte->vte_vn->vn_name);
        if (strcmp(path, vte->vte_vn->vn_name) == 0) {
            break;
        }
        vte = vte->next;
    }
    //printf("vfs_vnt_lookup %s\n", vte == NULL ? "failed" : "succeed");
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
