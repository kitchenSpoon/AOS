#ifndef _SOS_FILE_H_
#define _SOS_FILE_H_

#include "vfs/vnode.h"
#include "proc/proc.h"

#define PROCESS_MAX_FILES   16
#define STDOUT_FD           1
#define STDERR_FD           2

/******************************************************************************
 * openfile section
 *****************************************************************************/

/*
 * openfile struct
 * note that there's not too much to keep track of, since the vnode does most
 * of that.  note that it does require synchronization, because a single
 * openfile can be shared between processes (filetable inheritance).
 */
struct openfile {
    struct vnode *of_vnode;

    uint64_t of_offset;
    int of_accmode;	/* from open: O_RDONLY, O_WRONLY, or O_RDWR */
    int of_refcount;
};

/* Callback for file_open */
typedef void (*file_open_cb_t)(void *token, int err, int fd) ;

/*
 * file_open
 * opens a file, places it in the filetable.
 * Return filedesc through the callback
 */
void file_open(char *filename, int flags, file_open_cb_t callback, void *token);

/*
 * file_close
 * closes a file
 * desc refcount of openfile, freeing the memory if it goes to 0.
 */
int file_close(int fd);


/******************************************************************************
 * file table section
 *****************************************************************************/
/*
 * filetable struct
 * just an array of open files.  nice and simple.  doesn't require
 * synchronization, because a table can only be owned by a single process (on
 * inheritance in fork, the table is copied).
 */
struct filetable {
    struct openfile *ft_openfiles[PROCESS_MAX_FILES];
};

typedef void (*filetable_init_cb_t)(void *token, int err);
/*
 * filetable_init
 * pretty straightforward -- allocate the space, initialize to NULL.
 */
int filetable_init(struct filetable *filetable, filetable_init_cb_t callback, void *token);

/*
 * filetable_destroy
 * closes the files in the file table, frees the table.
 */
void filetable_destroy(struct filetable *ft);

/*
 * filetable_findfile
 * verifies that the file descriptor is valid and actually references an
 * open file, setting the FILE to the file at that index if it's there.
 */
int filetable_findfile(int fd, struct openfile **file);

/*
 * filetable_placefile
 * finds the smallest available file descriptor, places the file at the point,
 * sets FD to it.
 */
int filetable_placefile(struct openfile *file, int *fd);

#endif /* _SOS_FILE_H_ */
