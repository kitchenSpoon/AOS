#ifndef _SOS_VFS_H_
#define _SOS_VFS_H_

#include "vfs/vnode.h"
#include "syscall/file.h"

/******************************************************************************
 * VFS functions
 *****************************************************************************/

typedef void (*vfs_open_cb_t)(void* vfs_open_token, int err);
typedef void (*vfs_stat_cb_t)(void *token, int err);
void vfs_open(char *path, int openflags, file_open_cb_t callback, void *token);
void vfs_close(struct vnode *vn, uint32_t flags);
void vfs_stat(char* path, size_t path_len, sos_stat_t *buf, serv_sys_stat_cb_t callback, void *token);

/******************************************************************************
 * Vnode table entry part
 *****************************************************************************/
struct vnode_table_entry {
    struct vnode *vte_vn;
    struct vnode_table_entry *next;
};

struct vnode_table_entry *vnode_table_head;

/*
 * Performs a lookup in vnode table.
 * Returns the vnode correlates to the PATH if the vnode exists in the table
 */
struct vnode* vfs_vnt_lookup(const char *path);

/*
 * Create & insert new vnode to the vnode table
 * It is the caller's responsibility to ensure that a vnode of this path does
 * not already exists in the table (by calling vfs_vnt_lookup()
 */
int vfs_vnt_insert(struct vnode *vn);

/*
 * Delete the vnode with PATH in the vnode table
 */
void vfs_vnt_delete(const char *path);

#endif /* _SOS_VFS_H_ */
