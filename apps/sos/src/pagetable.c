#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <sel4/sel4.h>
#include "pagetable.h"

#define STATUS_USED     0
#define STATUS_FREE     1

typedef struct _pagetable_entry{
    int status;             // Either USED or FREE
    seL4_CPtr frame_cap;    // frame cap of the allocated frame or NULL
    int frame_id;           // index returned by frametable
} pagetable_entry_t;

static int
_map_pagetable(pagedir_t* spd, int i) {
    return 0;
}

static int
_unmap_pagetable(pagedir_t* spd, int i) {
    return 0;
}

int pagetable_init(void);

int sos_page_map(pagedir_t* spd, seL4_Word* vaddr);

int sos_page_unmap(pagedir_t* spd, seL4_Word* vaddr);
