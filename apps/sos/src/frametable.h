#ifndef _LIBOS_FRAMETABLE_H_
#define _LIBOS_FRAMETABLE_H_

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
 *
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

/*
 * Get capability given the id returned by frame_alloc()
 */
seL4_CPtr frame_get_cap(int id);

/*
 * Allocate N frames.
 *
 * Return the new address if successful, or NULL otherwise
 */
seL4_Word alloc_kpages(int n);

/*
 *TODO 
 */ 
int free_kpages(int n);

#endif /* _LIBOS_FRAMETABLE_H_ */
