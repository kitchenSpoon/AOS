#ifndef _LIBOS_ELF_H_
#define _LIBOS_ELF_H_

#include <sel4/sel4.h>
#include <cspace/cspace.h>
#include "proc/proc.h"
#include "vm/addrspace.h"

#define SEL4_N_PAGETABLES       (1<<12)

#define INDEX_1_MASK        (0xffc00000)
#define INDEX_2_MASK        (0x003ff000)
#define PT_L1_INDEX(a)      (((a) & INDEX_1_MASK) >> 22)
#define PT_L2_INDEX(a)      (((a) & INDEX_2_MASK) >> 12)

#define PTE_IN_USE_BIT          (1)
#define PTE_SWAPPED             (1<<1)
#define PTE_SWAP_OFFSET         (2)
#define PTE_SWAP_MASK           (0xfffffffc)
#define PTE_KVADDR_MASK         (0xfffff000)
/*
 * Functions in elf.c
 *    elf_load - load an ELF user program executable into the current
 *               address space. (i.e. the only one address space )
 */
typedef void (*elf_load_cb_t)(void *token, int err, seL4_Word elf_entry);
void elf_load(addrspace_t *as, char* elf_file, process_t* proc, elf_load_cb_t callback, void* token);

#endif /* _LIBOS_ELF_H_ */
