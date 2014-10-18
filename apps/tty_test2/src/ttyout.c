/*
 * Copyright 2014, NICTA
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(NICTA_BSD)
 */

/****************************************************************************
 *
 *      $Id:  $
 *
 *      Description: Simple milestone 0 code.
 *      		     Libc will need sos_write & sos_read implemented.
 *
 *      Author:      Ben Leslie
 *
 ****************************************************************************/

#include <stdarg.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "ttyout.h"

#include <sel4/sel4.h>

#define min(a, b) ((a) < (b)) ? (a) : (b)
#define FAIL_TOLERANCE 10

//static struct serial * serial;
void ttyout_init(void) {
    /* Perform any initialisation you require here */
    //serial_init();
}

/*
static size_t sos_debug_print(const void *vData, size_t count) {
    size_t i;
    const char *realdata = vData;
    for (i = 0; i < count; i++)
        seL4_DebugPutChar(realdata[i]);
    return count;
}
*/

//static size_t sos_call_print(const void *vData, size_t count) {
//    const char *realdata = vData;
//    int tot_sent = 0;
//    /*
//     * We break the data into messages of size maximum seL4_MsgMaxLength-1
//     * The first slot is used to indicate syscall number
//     */
//    int tries = 0;
//    int expected_sending = count / (seL4_MsgMaxLength-1) + 1;
//    while (tot_sent < count && tries < expected_sending*FAIL_TOLERANCE) {
//        int len = min(count - tot_sent, seL4_MsgMaxLength-1);
//        seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, 1+len);
//        seL4_SetTag(tag);
//        seL4_SetMR(0, 0); // Call syscall 0
//
//        for (int i=0; i<len; i++) {
//            seL4_SetMR(i+1, realdata[i+tot_sent]);
//        }
//        seL4_MessageInfo_t message = seL4_Call(SYSCALL_ENDPOINT_SLOT, tag);
//        int err = (int)seL4_MessageInfo_get_label(message);
//        (void)err;
//        int sent = seL4_GetMR(0);
//
//        if (sent < len) { /* some error handling? */ }
//        tot_sent += sent;
//        tries++;
//    }
//
//    return tot_sent;
//}
//
//size_t sos_write(void *vData, size_t count) {
//    return sos_call_print(vData, count);
//}
//
//size_t sos_read(void *vData, size_t count) {
//    //implement this to use your syscall
//    return 0;
//}
//
