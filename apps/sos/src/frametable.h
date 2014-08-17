#ifndef _SOS_FRAME_TABLE_H_
#define _SOS_FRAME_TABLE_H_

/* Return status for frame table */
#define FRAME_IS_OK         (0)
#define FRAME_IS_UNINT      (-1)       // Frame is uninitialised
#define FRAME_IS_FAIL       (-2)

/*
 * Initialise frame table. Reserve memory and initialise values for frame table 
 *
 * Returns FRAME_IS_OK iff successful
 */
int frame_init(void);

/*
 * Allocate a new frame that could be used in SOS.
 *     Note: This can allocate only 1 frame
 * Returns an index of frametable that can be used with frame_free()
 *         or a negative number if failed
 */
int frame_alloc(seL4_Word* vaddr);

/*
 * Free the frame correspond to ID / "physical address"
 *
 * Returns FRAME_IS_OK only if successul
 */ 
int frame_free(int frametable_id);

#endif /* _SOS_FRAME_TABLE_H_ */
