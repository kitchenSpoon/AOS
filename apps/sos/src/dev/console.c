#include <stdlib.h>
#include <errno.h>
#include <assert.h>

#include <serial/serial.h>
#include <sel4/sel4.h>
#include <cspace/cspace.h>
#include <fcntl.h>

#include "tool/utility.h"
#include "dev/console.h"
#include "vfs/vnode.h"
#include "vm/copyinout.h"

//#define MAX_IO_BUF 0x1000
#define MAX_IO_BUF 6000
#define MAX_SERIAL_SEND 100

struct console{
    char buf[MAX_IO_BUF];
    int buf_size;
    int start;
    int end;
    int is_init;
    struct serial * serial;
} console;

struct con_read_state{
    seL4_CPtr reply_cap;
    bool opened_for_reading;
    int is_blocked;
    struct vnode *file;
    char* buf;
    size_t nbytes;
} con_read_state;

int
con_init(void) {
    if (!console.is_init) {
        con_vnode = malloc(sizeof(struct vnode));
        if (con_vnode == NULL) {
            return ENOMEM;
        }
        con_vnode->vn_opencount  = 0;
        con_vnode->vn_data       = NULL;

        struct vnode_ops *vops = malloc(sizeof(struct vnode_ops));
        if (vops == NULL) {
            free(con_vnode);
            return ENOMEM;
        }
        vops->vop_eachopen  = con_eachopen;
        vops->vop_eachclose = con_eachclose;
        vops->vop_lastclose = con_lastclose;
        vops->vop_read  = con_read;
        vops->vop_write = con_write;

        con_vnode->vn_ops = vops;

        /* initalize console buf */
        console.serial = serial_init();
        if (console.serial == NULL) {
            free(con_vnode);
            free(vops);
            return EFAULT;
        }

        console.buf_size = 0;
        console.start = 0;
        console.end= 0;
        con_read_state.opened_for_reading = 0;

        console.is_init = 1;
    }
    return 0;
}

int
con_destroy_vnode(void) {
    if (con_vnode == NULL) {
        return EINVAL;
    }
    /* If any of these fails, that means we have a bug */
    assert(con_vnode->vn_ops != NULL);
    if (con_vnode->vn_opencount != 1) {
        return EINVAL;
    }

    console.serial = NULL;
    console.buf_size = 0;
    console.start = 0;
    console.end= 0;
    console.is_init = 0;

    free(con_vnode->vn_ops);
    free(con_vnode);
    return 0;
}

static void read_handler(struct serial * serial , char c){
    //printf("read_handler called, c = %d\n", (int)c);
    if(console.buf_size < MAX_IO_BUF){
        console.buf[console.end++] = c;
        console.end %= MAX_IO_BUF;
        console.buf_size++;
    }

    if(con_read_state.is_blocked && c == '\n'){
        struct vnode *file = con_read_state.file;
        char* buf = con_read_state.buf;
        size_t nbytes = con_read_state.nbytes;
        seL4_CPtr reply_cap = con_read_state.reply_cap;
        con_read(file, buf, nbytes, reply_cap);
    }
}

int con_eachopen(struct vnode *file, int flags){
    printf("con_open called\n");
    int err;

    if(flags == O_RDWR || flags == O_RDONLY){
        if(!con_read_state.opened_for_reading){
            err = serial_register_handler(console.serial, read_handler);
            if(err){
                return EFAULT;
            }
            con_read_state.opened_for_reading = 1;
        } else {
            return EFAULT;
        }
    }

    printf("con_open succeed\n");
    return 0;
}

int con_eachclose(struct vnode *file, uint32_t flags){
    printf("con_eachclose\n");
    if(flags == O_RDWR || flags == O_RDONLY) {
        if(console.serial == NULL) {
            return EFAULT;
        }
        int err = serial_register_handler(console.serial, NULL);
        if(err){ // should not happen
            return EFAULT;
        }

        console.buf_size = 0;
        con_read_state.opened_for_reading = 0;
    }
    return 0;
}

int con_lastclose(struct vnode *vn) {
    (void)vn;
    console.buf_size = 0;
    return 0;
}

int con_write(struct vnode *file, const char* buf, size_t nbytes, size_t *len) {
    struct serial* serial = serial_init(); //serial_init does the caching

    //TODO CHECK ME
    size_t tot_sent = 0;
    int tries = 0;
    while (tot_sent < nbytes && tries < MAX_SERIAL_SEND) {
        tot_sent += serial_send(serial, (char*)buf+tot_sent, nbytes-tot_sent);
        tries++;
    }

    *len = tot_sent;
    return 0;
}

int con_read(struct vnode *file, char* buf, size_t nbytes, seL4_CPtr reply_cap){
    //printf("con_read called\n");
    int err;

    if(console.buf_size > 0){
        size_t len = 0;
        for(size_t cur = console.start; len < nbytes && len < console.buf_size; cur++, cur%=MAX_IO_BUF){
            len++;
            if(console.buf[cur] == '\n') {
                break;
            }
        }

        //printf("copying out %d bytes, buffer size = %u\n", len, console.buf_size);

        //save the original start value, so that we can restore this when theres an error
        int console_start_ori = console.start;

        //Since we are using a circular buffer, we need to split our copy into two chunkcs
        //because our buffer may start and wrap over the buffer, also copyout should not
        //know that we are using a circular buffer.

        //copy first half of circular buffer
        int first_half_size = MIN(MAX_IO_BUF, console.start + len) - console.start;
        err = copyout((seL4_Word)buf, (seL4_Word)console.buf + console.start, first_half_size);
        console.start += first_half_size;
        console.start %= MAX_IO_BUF;

        //copy second half of circular buffer
        int second_half_size = len - first_half_size > 0 ? len - first_half_size : 0;
        int err2 = copyout((seL4_Word)buf + first_half_size, (seL4_Word)console.buf + console.start, second_half_size);
        console.start += second_half_size;
        console.start %= MAX_IO_BUF;
        if (err || err2) {
            seL4_MessageInfo_t reply = seL4_MessageInfo_new(err, 0, 0, 1);
            seL4_SetMR(0, (seL4_Word)-1); // This value can be anything
            seL4_Send(reply_cap, reply);
            cspace_free_slot(cur_cspace, reply_cap);

            console.start = console_start_ori;
            con_read_state.is_blocked = 0;
            return EFAULT;
        }

        //copy remaing buf foward
        /*for(size_t i = 0; i < console.buf_size - len; i++){
            console.buf[i] = console.buf[len+i];
        }*/

        console.buf_size -= len;

        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, (seL4_Word)len);
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);

        con_read_state.is_blocked = 0;
    } else {
        //printf("con_read: blocked\n");
        con_read_state.reply_cap = reply_cap;
        con_read_state.file = file;
        con_read_state.buf = buf;
        con_read_state.is_blocked = 1;
    }

    //printf("con_read out\n");
    return 0;
}