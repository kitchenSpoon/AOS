#ifndef _SOS_FRAME_TABLE_H_
#define _SOS_FRAME_TABLE_H_

/*
 * Initialise frame table. Reserve memory and initialise values for frame table 
 *
 * Returns 0 if successful
 */
int frame_init(void);

/*
 * Allocate a new frame that could be used in SOS.
 *      Note: This can allocate only 1 frame
 * Returns a virtual memory that could be referenced in SOS
 */
seL4_Word frame_alloc(void);

/*
 * Free the frame correspond to ADDR
 *
 * Returns 0 only if success
 */ 
int frame_free(seL4_Word addr);

#endif /* _SOS_FRAME_TABLE_H_ */
