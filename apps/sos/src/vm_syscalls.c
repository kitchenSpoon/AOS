#include <sel4/sel4.h>

#include "proc.h"
#include "syscall.h"
#include "addrspace.h"

void serv_sys_sbrk(seL4_CPtr reply_cap, seL4_Word newbrk) {
    seL4_MessageInfo_t reply;

    newbrk = sos_sys_brk(proc_getas(), newbrk);

    reply = seL4_MessageInfo_new(newbrk, 0, 0, 0);
    seL4_Send(reply_cap, reply);

    cspace_free_slot(cur_cspace, reply_cap);
}
