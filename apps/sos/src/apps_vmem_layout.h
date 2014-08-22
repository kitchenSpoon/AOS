#ifndef _APPS_MEM_LAYOUT_H_
#define _APPS_MEM_LAYOUT_H_

/* The start of memory to be used to store pagetables */
#define PAGE_VSTART         (0xC0000000)

#define PROCESS_STACK_TOP   (0x90000000)
#define PROCESS_IPC_BUFFER  (0xA0000000)

#endif /* _APPS_MEM_LAYOUT_H_ */
