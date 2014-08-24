#include "proc.h"

extern process_t tty_test_process;

//TODO: hacking before having cur_proc() function
process_t* cur_proc(void) {
    return &tty_test_process;
}

addrspace_t* proc_getas(void) {
    return (cur_proc()->as);
}

seL4_ARM_PageDirectory proc_getvroot(void) {
    return (cur_proc()->vroot);
}
cspace_t* proc_getcroot(void) {
    return (cur_proc()->croot);
}
