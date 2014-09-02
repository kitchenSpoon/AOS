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
con_create_vnode(void) {
    if (con_vnode == NULL) {
        con_vnode = malloc(sizeof(struct vnode));
        if (con_vnode == NULL) {
            return ENOMEM;
        }
        con_vnode->vn_refcount  = 1;
        con_vnode->vn_opencount = 1;
        con_vnode->vn_data      = NULL;

        struct vnode_ops *vops = malloc(sizeof(struct vnode_ops));
        if (vops == NULL) {
            free(con_vnode);
            return ENOMEM;
        }
        vops->vop_open  = con_open;
        vops->vop_close = con_close;
        vops->vop_read  = con_read;
        vops->vop_write = con_write;

        con_vnode->vn_ops = vops;

        //initalize console buf
        console.buf_size = 0;
        console.is_init = 0;
        console.serial = NULL;
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
    if (con_vnode->vn_refcount != 1 || con_vnode->vn_opencount != 1) {
        return EINVAL;
    }

    free(con_vnode->vn_ops);
    free(con_vnode);
    return 0;
}

static void read_handler(struct serial * serial , char c){
    printf("read_handler called\n");
    if(console.buf_size < MAX_IO_BUF){
        console.buf[console.buf_size++] = c;
    }
}

int con_open(struct vnode *file, int flags){
    printf("con_open called\n");
    struct serial* serial = serial_init();
    if(!console.is_init) {
        console.serial = serial_init();
        if(console.serial == NULL){
            return EFAULT;
        }

        int err = serial_register_handler(serial, read_handler);
        if(err){
            console.serial = NULL;
            return EFAULT;
        }
        console.is_init = 1;
    }

    return 0;
}

int con_close(struct vnode *file){
    console.serial = NULL;
    console.buf_size = 0;
    console.is_init = 0;
    return 0;
}

int con_write(struct vnode *file, const char* buf, size_t nbytes, size_t *len) {
    struct serial* serial = serial_init(); //serial_init does the cacheing

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

int con_read(struct vnode *file, char* buf, size_t nbytes, size_t *len){
    size_t bytes_left = nbytes;
    printf("bytes_left = %d\n", bytes_left);
    printf("con_read called\n");
    /*
     * while there are bytes left to be read,
     * we block waiting for more content to come
     */
    while(bytes_left > 0) {
        printf("bytes_left = %d\n", bytes_left);
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
            bytes_left -= i;
        }
    }
    printf("con_read out\n");

    return 0;
}
