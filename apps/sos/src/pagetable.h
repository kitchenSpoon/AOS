#ifndef _SOS_PAGE_TABLE_H_
#define _SOS_PAGE_TABLE_H_

#define PAGE_IS_OK         (0)
#define PAGE_IS_UNINT      (-1)
#define PAGE_IS_FAIL       (-2)

typedef struct pagetable_entry* pagetable_t;
typedef pagetable_t* pagedir_t;

/*
 * Initialise page table.
 * This needs to be called after frametable is initalised.
 *
 * Returns PAGE_IS_OK iff successful
 */
int pagetable_init(void);

/*
 * Map a page in into the page table
 * Returns PAGE_IS_OK if succesful
 */
int sos_page_map(pagedir_t* spd, seL4_Word* vaddr);

/*
 * Unmap a page in into the page table
 * Returns PAGE_IS_OK if successful
 */
int sos_page_unmap(pagedir_t* spd, seL4_Word* vaddr);
#endif /* _SOS_PAGE_TABLE_H_ */
