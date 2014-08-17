#include <assert.h>
#include <string.h>
#include <cspace/cspace.h>
#include <mapping.h>

#define FRAME_STATUS_UNTYPED     0
#define FRAME_STATUS_FREE        1
#define FRAME_STATUS_ALLOCATED   2

/* Frame table entry structure */
typedef struct {
    seL4_CPtr frame_cap;
    int status;
    seL4_ARM_PageDirectory as_root;
} frame_entry_t;


/* Keep track of initialisation status of the frame table */
static bool frame_initialised;

/*
 * Find index of the next free frame in the frame table
 * 
 * Returns index of the next free frame
 */
static int
_next_free(void) {
    return 0;
}
