/* This file is mostly copied from OS161 */

#include <errno.h>
#include <string.h>
#include <fcntl.h>

#include "vfs/vfs.h"
#include "vfs/vnode.h"
#include "proc/proc.h"
#include "syscall/file.h"
#include "dev/console.h"

/*** openfile functions ***/

//TODO: This needs to handle concurrency when we have multiple processes
//      Especially when they open the same file

typedef struct {
    file_open_cb_t callback;
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

    if (err) {
        cont->callback(cont->token, err, -1);
        free(cont);
        return;
    }

    // Compare openning permission with file permission
    int accmode = cont->flags & O_ACCMODE;
    printf("------accmode = %s\n", accmode == O_RDONLY ? "O_RDONLY" : "O_WRONLY");
    bool file_readable = vn->sattr.st_mode & S_IRUSR;
    bool file_writable = vn->sattr.st_mode & S_IWUSR;
    printf("file_readable = %d, file_writable = %d\n", (int)file_readable, (int)file_writable);
    if ((accmode == O_RDONLY && !file_readable) ||
        (accmode == O_WRONLY && !file_writable) ||
        (accmode == O_RDWR && !(file_readable && file_writable)))
    {
        cont->callback(cont->token, EINVAL, -1);
        free(cont);
        return;
    }
    struct openfile *file;
    int fd;

    file = malloc(sizeof(struct openfile));
    printf("created an openfile at %p\n", file);
    if (file == NULL) {
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
    CURPROC->p_filetable->ft_openfiles[fd] = NULL;
    printf("file_close_out\n");

    return 0;
}

/*** filetable functions ***/

/*
 * filetable_init
 * pretty straightforward -- allocate the space, initialize to NULL.
 */
int
filetable_init(struct filetable *filetable, const char *inpath, const char *outpath, const char *errpath) {
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
    //TODO: Change these numbers to use constants
    printf("filetable open something \n");
    filetable->ft_openfiles[0] = (struct openfile *)1;
    filetable->ft_openfiles[1] = (struct openfile *)1;
    filetable->ft_openfiles[2] = (struct openfile *)1;

    printf("filetable done \n");

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

/*
 * filetable_placefile
 * finds the smallest available file descriptor, places the file at the point,
 * sets FD to it.
 */
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

/*
 * filetable_destroy
 * closes the files in the file table, frees the table.
 */
void
filetable_destroy(struct filetable *ft)
{
    int fd, result;

    assert(ft != NULL);

    //TODO: hack because filetable_init is not opening stdout & stderr properly
    for (fd = 3; fd < PROCESS_MAX_FILES; fd++) {
        struct openfile *file = ft->ft_openfiles[fd];
        if (file != NULL) {
            result = file_doclose(file, file->of_accmode);
            assert(result == 0);
        }
    }

    free(ft);
}
