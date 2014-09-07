#include "proc/proc.h"

extern process_t tty_test_process;

//TODO: hacking before having cur_proc() function
process_t* cur_proc(void) {
    return &tty_test_process;
}

addrspace_t* proc_getas(void) {
    return (cur_proc()->as);
}

cspace_t* proc_getcroot(void) {
    return (cur_proc()->croot);
}
