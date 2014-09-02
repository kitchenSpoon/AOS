#include <stdlib.h>
#include <errno.h>
#include <assert.h>

#include <serial/serial.h>

#include "console.h"
#include "vnode.h"

#define MAX_IO_BUF 0x1000
#define MAX_SERIAL_SEND 100

struct console{
    char buf[MAX_IO_BUF];
    int buf_size;
    int is_init;
    struct serial * serial;
} console;

int
create_con_vnode(struct vnode* vn) {
    if (vn == NULL) {
        vn = malloc(sizeof(struct vnode));
        if (vn == NULL) {
            return ENOMEM;
        }
        vn->vn_refcount  = 1;
        vn->vn_opencount = 1;
        vn->vn_data      = NULL;

        struct vnode_ops *vops = malloc(sizeof(struct vnode_ops));
        if (vops == NULL) {
            free(vn);
            return ENOMEM;
        }
        vops->vop_read  = NULL;
        vops->vop_write = NULL;

        vn->vn_ops = vops;

        //initalize console buf
        console.buf_size = 0;
        console.is_init = 0;
        console.serial = NULL;
    }
    return 0;
}

int
destroy_con_vnode(struct vnode *vn) {
    if (vn == NULL) {
        return EINVAL;
    }
    /* If any of these fails, that means we have a bug */
    assert(vn->vn_ops != NULL);
    if (vn->vn_refcount != 1 || vn->vn_opencount != 1) {
        return EINVAL;
    }

    free(vn->vn_ops);
    free(vn);
    return 0;
}

static void read_handler(struct serial * serial , char c){
    if(console.buf_size < MAX_IO_BUF){
        console.buf[console.buf_size++] = c;
    }
}

size_t con_open(struct vnode *file, int flags){
    struct serial* serial = serial_init();
    if(!console.is_init){
        console.serial = serial_init();
        if(console.serial == NULL){
            return -1;
        }

        int err = serial_register_handler(serial, read_handler);
        if(err){
            console.serial = NULL;
            return -1;
        }
        console.is_init = 1;
    }

    return 0;
}

size_t con_close(struct vnode *file){
    console.serial = NULL;
    console.is_init = 0;
    return 0;
}

size_t con_write(struct vnode *file, char* buf, size_t nbytes, size_t *len) {
    struct serial* serial = serial_init(); //serial_init does the cacheing

    //TODO CHECK ME
    size_t tot_sent = 0;
    int tries = 0;
    while (tot_sent < nbytes && tries < MAX_SERIAL_SEND) {
        tot_sent += serial_send(serial, buf+tot_sent, nbytes-tot_sent);
        tries++;
    }

    *len = tot_sent;
    return 0;
}

size_t con_read(struct vnode *file, char* buf, size_t nbytes, size_t *len){
    /* we need to register handler when we open console */
    size_t bytes_left = nbytes;
    /*
     * while there are bytes left to be read,
     * we block waiting for more content to come
     */
    while(bytes_left > 0){
        if(console.buf_size > 0){
            //copy console_buf to user's buf
            int i = 0;
            for(i = 0; i < bytes_left && i < console.buf_size; i++){
                buf[i] = console.buf[i];
            }

            //copy remaing buf foward
            for(int j = 0; j < console.buf_size - i; j++){
                console.buf[j] = console.buf[i+j];
            }

            //set correct buffer size
            console.buf_size -= i;
            len += i;
        }
    }

    return 0;
}
