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

#define MAX_IO_BUF 0x1000
#define MAX_SERIAL_SEND 100


static int con_eachopen(struct vnode *file, int flags);
static int con_eachclose(struct vnode *file, uint32_t flags);
static int con_lastclose(struct vnode *file);
static void con_read(struct vnode *file, char* buf, size_t nbytes, size_t offset,
                     vop_read_cb_t callback, void *token);
static void con_write(struct vnode *file, const char* buf, size_t nbytes, size_t offset,
               vop_write_cb_t callback, void *token);

struct console{
    char buf[MAX_IO_BUF];
    int buf_size;
    int start;
    int end;
    struct serial * serial;
} console;

struct con_read_state{
    bool opened_for_reading;
    int is_blocked;
    struct vnode *file;
    char* buf;
    size_t nbytes;
    vop_read_cb_t callback;
    void* token;
    size_t offset;
} con_read_state;

int
con_init(struct vnode *con_vn) {
    printf("con_init called\n");
    assert(con_vn != NULL);

    con_vn->vn_ops->vop_eachopen  = con_eachopen;
    con_vn->vn_ops->vop_eachclose = con_eachclose;
    con_vn->vn_ops->vop_lastclose = con_lastclose;
    con_vn->vn_ops->vop_read      = con_read;
    con_vn->vn_ops->vop_write     = con_write;
    con_vn->vn_ops->vop_getdirent = NULL;
    con_vn->vn_ops->vop_stat      = NULL;

    con_vn->sattr.st_type = ST_FILE;
    con_vn->sattr.st_mode = S_IWUSR | S_IRUSR;
    con_vn->sattr.st_size = 0;
    con_vn->sattr.st_mtime.seconds = 0;
    con_vn->sattr.st_mtime.useconds = 0;

    con_vn->vn_data     = NULL;
    con_vn->initialised = true;

    /* initalize console buf */
    console.serial = serial_init();
    if (console.serial == NULL) {
        return EFAULT;
    }

    console.buf_size = 0;
    console.start = 0;
    console.end= 0;
    con_read_state.opened_for_reading = 0;

    return 0;
}

static void
read_handler(struct serial * serial , char c){
    //printf("read_handler called, c = %d\n", (int)c);
    if(console.buf_size < MAX_IO_BUF){
        console.buf[console.end++] = c;
        console.end %= MAX_IO_BUF;
        console.buf_size++;
    }

    if(con_read_state.is_blocked && c == '\n'){
        struct con_read_state *s = &con_read_state;
        con_read(s->file, s->buf, s->nbytes, s->offset, s->callback, s->token);
    }
}

static int
con_eachopen(struct vnode *file, int flags){
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

static int
con_eachclose(struct vnode *file, uint32_t flags){
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

static int
con_lastclose(struct vnode *con_vn) {
    /* If any of these fails, that means we have a bug */
    assert(con_vn->vn_ops != NULL);
    assert(con_vn->vn_opencount == 1);

    console.serial    = NULL;
    console.buf_size  = 0;
    console.start     = 0;
    console.end       = 0;

    return 0;
}

static void
con_write(struct vnode *file, const char* buf, size_t nbytes, size_t offset,
          vop_write_cb_t callback, void *token)
{
    printf("conwrite\n");
    (void)offset;
    if (console.serial == NULL) {
        callback(token, EFAULT, 0);
        return;
    }
    struct serial* serial = console.serial;

    size_t tot_sent = 0;
    int tries = 0;
    while (tot_sent < nbytes && tries < MAX_SERIAL_SEND) {
        tot_sent += serial_send(serial, (char*)buf+tot_sent, nbytes-tot_sent);
        tries++;
    }

    callback(token, 0, tot_sent);
}

typedef void (*copyout_cb_t)(void* token, int err);

typedef struct {
    size_t len;
    int first_half_size;
    int second_half_size;
    char* buf;
    char* kbuf;
    //save the original start value, so that we can restore this when theres an error
    //int console_start_ori;
    vop_read_cb_t callback;
    void* token;
} con_read_cont_t;

static void con_read_end(void* token, int err){
        con_read_cont_t* cont = (con_read_cont_t*)token;

        //should not actually shift console start since this is now asynchronous
        //console.start += cont->second_half_size;
        //console.start %= MAX_IO_BUF;
        if (err) {
            //console.start = cont->console_start_ori;
            con_read_state.is_blocked = 0;
            printf("console err \n");
            cont->callback(cont->token, EFAULT, 0, false);
            return;
        }

        //Only update console.start after successfully executing all the operations
        console.start += cont->first_half_size + cont->second_half_size;
        console.start %= MAX_IO_BUF;

        console.buf_size -= cont->len;

        con_read_state.is_blocked = 0;
        printf("console read size = %d\n",cont->len);
        cont->callback(cont->token, 0, cont->len, false);
        free(cont);
}

static void con_read_part2(void* token, int err){
        con_read_cont_t* cont = (con_read_cont_t*)token;
        if (err) {
            //console.start = cont->console_start_ori;
            con_read_state.is_blocked = 0;
            printf("console err \n");
            cont->callback(cont->token, EFAULT, 0, false);
            free(cont);
            return;
        }

        //should not actually shift console start since this is now asynchronous
        int console_start = console.start + cont->first_half_size;
        console_start %= MAX_IO_BUF;
        //console.start += cont->first_half_size;
        //console.start %= MAX_IO_BUF;
        //copy second half of circular buffer
        cont->second_half_size = cont->len - cont->first_half_size > 0 ? cont->len - cont->first_half_size : 0;
        err = copyout((seL4_Word)cont->buf + cont->first_half_size,
                (seL4_Word)console.buf + console_start, cont->second_half_size,
                con_read_end, token);
        if (err) {
            con_read_end((void*)cont, err);
            return;
        }
}

static void
con_read(struct vnode *file, char* buf, size_t nbytes, size_t offset,
         vop_read_cb_t callback, void *token)
{
    printf("con_read called\n");
    (void)offset;
    int err;
    (void)err;
    printf("nbytes = %u, %d\n",nbytes,  nbytes);

    if(console.buf_size > 0){
        size_t len = 0;
        printf("console start = %d, nbytes = %u, console.buf_size = %u \n",console.start, nbytes, console.buf_size);
        for(size_t cur = console.start; len < nbytes && len < console.buf_size; cur++, cur%=MAX_IO_BUF){
            printf("\n%c\n",console.buf[cur]);
            len++;
            if(console.buf[cur] == '\n') {
                break;
            }
        }

        printf("console read size = %d\n",len);
        //printf("copying out %d bytes, buffer size = %u\n", len, console.buf_size);


        //Since we are using a circular buffer, we need to split our copy into two chunkcs
        //because our buffer may start and wrap over the buffer, also copyout should not
        //know that we are using a circular buffer.

        con_read_cont_t* cont = malloc(sizeof(con_read_cont_t));
        if (cont == NULL) {
            con_read_state.is_blocked = 0;
            callback(token, ENOMEM, 0, false);
            return;
        }
        cont->len = len;
        cont->first_half_size = MIN(MAX_IO_BUF, console.start + len) - console.start;
        cont->buf = buf;
        cont->callback = callback;
        cont->token = token;

        printf("con_read buf = %p\n",buf);
        //copy first half of circular buffer
        err = copyout((seL4_Word)buf, (seL4_Word)console.buf + console.start,
                cont->first_half_size, con_read_part2, (void*)cont);
        if (err) {
            con_read_end((void*)cont, err);
        }
        return;
    } else {
        //printf("con_read: blocked\n");
        con_read_state.file       = file;
        con_read_state.buf        = buf;
        con_read_state.is_blocked = 1;
        con_read_state.callback   = callback;
        con_read_state.token      = token;
        con_read_state.offset     = offset;
        con_read_state.nbytes     = nbytes;
        return;
    }

    //printf("con_read out\n");
}

//static void
//con_read(struct vnode *file, char* buf, size_t nbytes, size_t offset,
//         vop_read_cb_t callback, void *token)
//{
//    printf("con_read called\n");
//    (void)offset;
//    int err;
//
//    if(console.buf_size > 0){
//        size_t len = 0;
//        for(size_t cur = console.start; len < nbytes && len < console.buf_size; cur++, cur%=MAX_IO_BUF){
//            len++;
//            if(console.buf[cur] == '\n') {
//                break;
//            }
//        }
//
//        printf("console read size = %d\n",len);
//        //printf("copying out %d bytes, buffer size = %u\n", len, console.buf_size);
//
//        //save the original start value, so that we can restore this when theres an error
//        int console_start_ori = console.start;
//
//        //Since we are using a circular buffer, we need to split our copy into two chunkcs
//        //because our buffer may start and wrap over the buffer, also copyout should not
//        //know that we are using a circular buffer.
//
//        //copy first half of circular buffer
//        int first_half_size = MIN(MAX_IO_BUF, console.start + len) - console.start;
//        err = copyout((seL4_Word)buf, (seL4_Word)console.buf + console.start, first_half_size);
//        console.start += first_half_size;
//        console.start %= MAX_IO_BUF;
//
//        //copy second half of circular buffer
//        int second_half_size = len - first_half_size > 0 ? len - first_half_size : 0;
//        int err2 = copyout((seL4_Word)buf + first_half_size, (seL4_Word)console.buf + console.start, second_half_size);
//        console.start += second_half_size;
//        console.start %= MAX_IO_BUF;
//        if (err || err2) {
//            //seL4_MessageInfo_t reply = seL4_MessageInfo_new(err, 0, 0, 1);
//            //seL4_SetMR(0, (seL4_Word)-1); // This value can be anything
//            //seL4_Send(reply_cap, reply);
//            //cspace_free_slot(cur_cspace, reply_cap);
//
//            console.start = console_start_ori;
//            con_read_state.is_blocked = 0;
//            printf("console err \n");
//            callback(token, EFAULT, 0, false);
//            return;
//        }
//
//        //copy remaing buf foward
//        /*for(size_t i = 0; i < console.buf_size - len; i++){
//            console.buf[i] = console.buf[len+i];
//        }*/
//
//        console.buf_size -= len;
//
//        //seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
//        //seL4_SetMR(0, (seL4_Word)len);
//        //seL4_Send(reply_cap, reply);
//        //cspace_free_slot(cur_cspace, reply_cap);
//
//        con_read_state.is_blocked = 0;
//        printf("console read size = %d\n",len);
//        callback(token, 0, len, false);
//    } else {
//        //printf("con_read: blocked\n");
//        con_read_state.file       = file;
//        con_read_state.buf        = buf;
//        con_read_state.is_blocked = 1;
//        con_read_state.callback   = callback;
//        con_read_state.token      = token;
//        con_read_state.offset     = offset;
//    }
//
//    //printf("con_read out\n");
//}
