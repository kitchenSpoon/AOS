/* This file is mostly copied from OS161 */

#include <errno.h>
#include <string.h>
#include <fcntl.h>

#include "vfs/vfs.h"
#include "vfs/vnode.h"
#include "proc/proc.h"
#include "syscall/file.h"
#include "dev/console.h"

/**********************************************************************
 * File Open
 **********************************************************************/

/*** openfile functions ***/

typedef struct {
    file_open_cb_t callback;
    void *token;
    int flags;
} cont_file_open_t;

static void file_open_end(void *token, int err, struct vnode *vn);

void
file_open(char *filename, int flags, file_open_cb_t callback, void *token)
{
    printf("file_open called\n");
    int accmode = flags & O_ACCMODE;
    if (!(accmode==O_RDONLY ||
          accmode==O_WRONLY ||
          accmode==O_RDWR)) {
        callback(token, EINVAL, -1);
        return;
    }

    cont_file_open_t *cont = malloc(sizeof(cont_file_open_t));
    if (cont == NULL) {
        callback(token, ENOMEM, -1);
        return;
    }
    cont->callback = callback;
    cont->token    = token;
    cont->flags    = flags;

    vfs_open(filename, flags, file_open_end, (void*)cont);
}

static void file_open_end(void *token, int err, struct vnode *vn) {
    printf("file_open_end called\n");
    assert(token != NULL);

    cont_file_open_t *cont = (cont_file_open_t*)token;

    if (err) {
        cont->callback(cont->token, err, -1);
        free(cont);
        return;
    }

    // Compare openning permission with file permission
    int accmode = cont->flags & O_ACCMODE;
    bool file_readable = vn->sattr.st_mode & S_IRUSR;
    bool file_writable = vn->sattr.st_mode & S_IWUSR;
    printf("file_readable = %d, file_writable = %d\n", (int)file_readable, (int)file_writable);
    if ((accmode == O_RDONLY && !file_readable) ||
        (accmode == O_WRONLY && !file_writable) ||
        (accmode == O_RDWR && !(file_readable && file_writable)))
    {
        vfs_close(vn, cont->flags);
        cont->callback(cont->token, EINVAL, -1);
        free(cont);
        return;
    }
    struct openfile *file;
    int fd;

    file = malloc(sizeof(struct openfile));
    printf("created an openfile at %p\n", file);
    if (file == NULL) {
        vfs_close(vn, cont->flags);
        cont->callback(cont->token, ENOMEM, -1);
        free(cont);
        return;
    }

    file->of_offset = 0;
    file->of_accmode = accmode;
    file->of_refcount = 1;
    file->of_vnode = vn;

    /* place the file in the filetable, getting the file descriptor */
    err = filetable_placefile(file, &fd);
    if (err) {
        free(file);
        vfs_close(vn, cont->flags);
        cont->callback(cont->token, err, -1);
        free(cont);
        return;
    }

    cont->callback(cont->token, 0, fd);
    free(cont);
}

/**********************************************************************
 * File Close
 **********************************************************************/

/*
 * file_doclose
 * shared code for file_close and filetable_destroy
 */
static
int
file_doclose(struct openfile *file, uint32_t flags)
{
    printf("file_doclose\n");

    if(file == NULL){
        return EINVAL;
    }

    /* if this is the last close of this file, free it up */
    if (file->of_refcount == 1) {
        vfs_close(file->of_vnode, flags);
        //printf("vfs_close_out\n");
        //printf("free openfile at %p\n", file);
        free(file);
        //printf("free_file\n");
    } else {
        assert(file->of_refcount > 1);
        file->of_refcount--;
    }

    return 0;
}

int
file_close(int fd)
{
    printf("file_close\n");
    struct openfile *file;
    int result;

    /* find the file in the filetable */
    result = filetable_findfile(fd, &file);
    if (result) {
        return result;
    }

    result = file_doclose(file, file->of_accmode);
    if (result) {
        /* leave file open for possible retry */
        return result;
    }
    CURPROC->p_filetable->ft_openfiles[fd] = NULL;
    printf("file_close_out\n");

    return 0;
}

/**********************************************************************
 * Filetable Utility Functions
 **********************************************************************/

/*** filetable functions ***/
typedef struct {
    struct filetable *filetable;
    filetable_init_cb_t callback;
    void *token;
} cont_filetable_init_t;

static void
filetable_init_end(void *token, int err, struct vnode *vn) {
    cont_filetable_init_t *cont = (cont_filetable_init_t*)token;

    struct openfile *file;

    file = malloc(sizeof(struct openfile));
    if (file == NULL) {
        vfs_close(vn, O_WRONLY);
        cont->callback(cont->token, ENOMEM);
        free(cont);
        return;
    }

    file->of_offset = 0; // console doesn't use offset
    file->of_accmode = O_WRONLY;
    file->of_refcount = 2;
    file->of_vnode = vn;

    cont->filetable->ft_openfiles[STDOUT_FD] = file;
    cont->filetable->ft_openfiles[STDERR_FD] = file;

    cont->callback(cont->token, 0);
    free(cont);
}

int
filetable_init(struct filetable *filetable, filetable_init_cb_t callback, void *token) {
    printf("in file table init\n");
    /* the filenames come from the kernel; assume reasonable length */
    int fd;

    if (filetable == NULL) {
        return EINVAL;
    }

    /* NULL-out the table */
    printf("filetable nullout something \n");
    for (fd = 0; fd < PROCESS_MAX_FILES; fd++) {
        filetable->ft_openfiles[fd] = NULL;
    }

    /* Initialise stdin, stdout & stderr */
    printf("filetable open something \n");
    cont_filetable_init_t *cont = malloc(sizeof(cont_file_open_t));
    if (cont == NULL) {
        return ENOMEM;
    }
    cont->filetable = filetable;
    cont->callback  = callback;
    cont->token     = token;

    /* Open console for stdout and stderr */
    vfs_open("console", O_WRONLY, filetable_init_end, (void*)cont);
    return 0;
}

int
filetable_findfile(int fd, struct openfile **file)
{
    struct filetable *ft = CURPROC->p_filetable;

    if (fd < 0 || fd >= PROCESS_MAX_FILES) {
        return EBADF;
    }

    *file = ft->ft_openfiles[fd];
    if (*file == NULL) {
        return EBADF;
    }

    return 0;
}

int
filetable_placefile(struct openfile *file, int *fd)
{
    printf("openfile1 still at %p\n", file);
    struct filetable *ft = CURPROC->p_filetable;
    int i;

    for (i = 0; i < PROCESS_MAX_FILES; i++) {
        if (ft->ft_openfiles[i] == NULL) {
            ft->ft_openfiles[i] = file;
            *fd = i;
            printf("openfile1 still at %p\n", file);
            return 0;
        }
    }

    return EMFILE;
}

void
filetable_destroy(struct filetable *ft)
{
    int fd, result;

    assert(ft != NULL);

    for (fd = 0; fd < PROCESS_MAX_FILES; fd++) {
        struct openfile *file = ft->ft_openfiles[fd];
        if (file != NULL) {
            result = file_doclose(file, file->of_accmode);
            assert(result == 0);
        }
    }

    free(ft);
}
