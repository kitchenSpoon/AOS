#include <errno.h>
#include <string.h>
#include <fcntl.h>

#include "proc.h"
#include "file.h"
#include "vnode.h"
#include "console.h"

/*** openfile functions ***/

/*
 * file_open
 * opens a file, places it in the filetable, sets RETFD to the file
 * descriptor. the pointer arguments must be kernel pointers.
 * NOTE -- the passed in filename must be a mutable string.
 */
int
file_open(char *filename, int flags, int *retfd)
{
	struct vnode *vn;
	struct openfile *file;
	int err;
	
    if (strcmp(filename, "console") == 0) {
        if (con_vnode == NULL) {
            err = create_con_vnode(con_vnode);
        }
        vn = con_vnode;
    } else {
        // Don't handle it for now
        vn = NULL;
        return EFAULT;
        //err = vfs_open(filename, flags, mode, &vn);
        //if (err) {
        //    return err;
        //}
    }

	file = malloc(sizeof(struct openfile));
	if (file == NULL) {
        vnode_decref(vn);
		//vfs_close(vn);
		return ENOMEM;
	}

	file->of_vnode = vn;
	file->of_offset = 0;
	file->of_accmode = flags & O_ACCMODE;
	file->of_refcount = 1;

	/* vfs_open checks for invalid access modes */
	assert(file->of_accmode==O_RDONLY ||
		file->of_accmode==O_WRONLY ||
		file->of_accmode==O_RDWR);

	/* place the file in the filetable, getting the file descriptor */
	err = filetable_placefile(file, retfd);
	if (err) {
		free(file);
        vnode_decref(vn);
		//vfs_close(vn);
		return err;
	}

	return 0;
}

/*
 * file_doclose
 * shared code for file_close and filetable_destroy
 */
static
int
file_doclose(struct openfile *file)
{
    /* if this is the last close of this file, free it up */
    if (file->of_refcount == 1) {
        vnode_decref(file->of_vnode);
        free(file);
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
    struct openfile *file;
    int result;

    /* find the file in the filetable */
    result = filetable_findfile(fd, &file);
    if (result) {
        return result;
    }

    result = file_doclose(file);
    if (result) {
        /* leave file open for possible retry */
        return result;
    }
    curproc->p_filetable->ft_openfiles[fd] = NULL;

    return 0;
}

/*** filetable functions ***/

/*
 * filetable_init
 * pretty straightforward -- allocate the space, initialize to NULL.
 * Note: Not initializing stdin, stdout & stderr
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
	struct filetable *ft = curproc->p_filetable;
	int i;
	
	for (i = 0; i < PROCESS_MAX_FILES; i++) {
		if (ft->ft_openfiles[i] == NULL) {
			ft->ft_openfiles[i] = file;
			*fd = i;
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
        if (ft->ft_openfiles[fd]) {
            result = file_doclose(ft->ft_openfiles[fd]);
            assert(result == 0);
        }
    }

    free(ft);
}	
