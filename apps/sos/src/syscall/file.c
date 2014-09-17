/* This file is mostly copied from OS161 */

#include <errno.h>
#include <string.h>
#include <fcntl.h>

#include "vfs/vfs.h"
#include "vfs/vnode.h"
#include "proc/proc.h"
#include "syscall/syscall.h"
#include "syscall/file.h"
#include "dev/console.h"

/*** openfile functions ***/

//TODO: This needs to handle concurrency when we have multiple processes
//      Especially when they open the same file

typedef struct {
    serv_sys_open_cb_t callback;
    void *token;
    int flags;
} cont_file_open_t;

/*
 * file_open
 * opens a file, places it in the filetable, sets RETFD to the file
 * descriptor. the pointer arguments must be kernel pointers.
 */
static void file_open_end(void *token, int err, struct vnode *vn) {
    printf("file_open_end called\n");
    assert(token != NULL);

    cont_file_open_t *cont = (cont_file_open_t*)token;
    cont_file_open_t local_cont = *cont;
    free(cont);

    if (err) {
        local_cont.callback(local_cont.token, err, -1);
        return;
    }

    struct openfile *file;
    int fd;

    file = malloc(sizeof(struct openfile));
    printf("created an openfile at %p\n", file);
    if (file == NULL) {
        local_cont.callback(local_cont.token, ENOMEM, -1);
        return;
    }

    file->of_offset = 0;
    file->of_accmode = local_cont.flags & O_ACCMODE;
    file->of_refcount = 1;
    file->of_vnode = vn;

    /* place the file in the filetable, getting the file descriptor */
    err = filetable_placefile(file, &fd);
    if (err) {
        free(file);
        vfs_close(vn, local_cont.flags);
        local_cont.callback(local_cont.token, err, -1);
        return;
    }

    local_cont.callback(local_cont.token, 0, fd);
}

void
file_open(char *filename, int flags, serv_sys_open_cb_t callback, void *token)
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

/*
 * file_close
 * knock off the refcount, freeing the memory if it goes to 0.
 */
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
    curproc->p_filetable->ft_openfiles[fd] = NULL;
    printf("file_close_out\n");

    return 0;
}

/*** filetable functions ***/

/*
 * filetable_init
 * pretty straightforward -- allocate the space, initialize to NULL.
 */
int
filetable_init(const char *inpath, const char *outpath,
               const char *errpath) {
    /* the filenames come from the kernel; assume reasonable length */
    int fd;

    /* catch memory leaks, repeated calls */
    assert(curproc->p_filetable == NULL);

    curproc->p_filetable = malloc(sizeof(struct filetable));
    if (curproc->p_filetable == NULL) {
        return ENOMEM;
    }

    /* NULL-out the table */
    for (fd = 0; fd < PROCESS_MAX_FILES; fd++) {
        curproc->p_filetable->ft_openfiles[fd] = NULL;
    }

    /* Initialise stdin, stdout & stderr */
    //TODO: Change these numbers to use constants
    curproc->p_filetable->ft_openfiles[0] = (struct openfile *)1;
    curproc->p_filetable->ft_openfiles[1] = (struct openfile *)1;
    curproc->p_filetable->ft_openfiles[2] = (struct openfile *)1;


    return 0;
}

/*
 * filetable_findfile
 * verifies that the file descriptor is valid and actually references an
 * open file, setting the FILE to the file at that index if it's there.
 */
int
filetable_findfile(int fd, struct openfile **file)
{
    struct filetable *ft = curproc->p_filetable;

    if (fd < 0 || fd >= PROCESS_MAX_FILES) {
        return EBADF;
    }

    *file = ft->ft_openfiles[fd];
    if (*file == NULL) {
        return EBADF;
    }

    return 0;
}

/*
 * filetable_placefile
 * finds the smallest available file descriptor, places the file at the point,
 * sets FD to it.
 */
int
filetable_placefile(struct openfile *file, int *fd)
{
    printf("openfile1 still at %p\n", file);
    struct filetable *ft = curproc->p_filetable;
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

/*
 * filetable_destroy
 * closes the files in the file table, frees the table.
 */
void
filetable_destroy(struct filetable *ft)
{
    int fd, result;

    assert(ft != NULL);

    for (fd = 0; fd < PROCESS_MAX_FILES; fd++) {
        struct openfile *file = ft->ft_openfiles[fd];
        if (file) {
            result = file_doclose(file, file->of_accmode);
            assert(result == 0);
        }
    }

    free(ft);
}
