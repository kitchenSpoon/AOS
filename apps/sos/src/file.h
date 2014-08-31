#ifndef _SOS_FILE_H_
#define _SOS_FILE_H_

#define PROCESS_MAX_FILES 16
/*** openfile section ***/

/* 
 * openfile struct 
 * note that there's not too much to keep track of, since the vnode does most
 * of that.  note that it does require synchronization, because a single
 * openfile can be shared between processes (filetable inheritance).
 */
struct openfile {
	struct vnode *of_vnode;
	
	off_t of_offset;
	int of_accmode;	/* from open: O_RDONLY, O_WRONLY, or O_RDWR */
	int of_refcount;
};

/*** file table section ***/

/*
 * filetable struct
 * just an array of open files.  nice and simple.  doesn't require
 * synchronization, because a table can only be owned by a single process (on
 * inheritance in fork, the table is copied).
 */
struct filetable {
	struct openfile *ft_openfiles[PROCESS_MAX_FILE];
};

/* these all have an implicit arg of the curthread's filetable */
int filetable_init(const char *inpath, const char *outpath, 
		   const char *errpath);

#endif /* _SOS_FILE_H_ */
