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
#include "proc/proc.h"

#define verbose 0
#include <sys/debug.h>

#define MAX_IO_BUF 0x1000
#define MAX_SERIAL_SEND 100


static int _con_eachopen(struct vnode *file, int flags);
static int _con_eachclose(struct vnode *file, uint32_t flags);
static int _con_lastclose(struct vnode *file);
static void _con_read(struct vnode *file, char* buf, size_t nbytes, size_t offset,
                     vop_read_cb_t callback, void *token);
static void _con_write(struct vnode *file, const char* buf, size_t nbytes, size_t offset,
               vop_write_cb_t callback, void *token);

struct console{
    char buf[MAX_IO_BUF];
    int buf_size;
    int start;
    int end;
    struct serial * serial;
} console;

struct _con_read_state{
    bool opened_for_reading;
    int is_blocked;
    char* buf;
    size_t nbytes;
    size_t offset;
    pid_t cur_proc_pid;
    vop_read_cb_t callback;
    void* token;
    struct vnode *file;
} _con_read_state;

/**********************************************************************
 * Console init
 **********************************************************************/

int
con_init(struct vnode *con_vn) {
    dprintf(3, "con_init called\n");
    assert(con_vn != NULL);

    con_vn->vn_ops->vop_eachopen  = _con_eachopen;
    con_vn->vn_ops->vop_eachclose = _con_eachclose;
    con_vn->vn_ops->vop_lastclose = _con_lastclose;
    con_vn->vn_ops->vop_read      = _con_read;
    con_vn->vn_ops->vop_write     = _con_write;
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
    _con_read_state.cur_proc_pid = PROC_NULL;
    _con_read_state.opened_for_reading = 0;

    return 0;
}

/**********************************************************************
 * Read handlers
 **********************************************************************/

static void
_read_handler(struct serial * serial , char c){
    dprintf(3, "_con_read handler called by %d\n", proc_get_id());
    //set_cur_proc(_con_read_state.cur_proc_pid);
    //dprintf(3, "_read_handler called, c = %d\n", (int)c);
    if(console.buf_size < MAX_IO_BUF){
        console.buf[console.end++] = c;
        console.end %= MAX_IO_BUF;
        console.buf_size++;
    }

    if(_con_read_state.is_blocked && c == '\n'){
        if (is_proc_alive(_con_read_state.cur_proc_pid)) {
            set_cur_proc(_con_read_state.cur_proc_pid);
            struct _con_read_state *s = &_con_read_state;
            _con_read(s->file, s->buf, s->nbytes, s->offset, s->callback, s->token);
        } else {
            _con_read_state.cur_proc_pid = PROC_NULL;
            _con_read_state.is_blocked = false;
        }
    }
}

/**********************************************************************
 * Console Open
 **********************************************************************/

static int
_con_eachopen(struct vnode *file, int flags){
    dprintf(3, "con_open called by %d\n", proc_get_id());
    int err;

    if(flags == O_RDWR || flags == O_RDONLY){
        if(!_con_read_state.opened_for_reading){
            err = serial_register_handler(console.serial, _read_handler);
            if(err){
            dprintf(3, "con_open _con_read cant register\n");
                return EFAULT;
            }
            _con_read_state.cur_proc_pid = proc_get_id();
            _con_read_state.opened_for_reading = 1;
        } else {
            dprintf(3, "con_open _con_read opened for reading\n");
            return EFAULT;
        }
    }

    dprintf(3, "con_open succeed\n");
    return 0;
}

/**********************************************************************
 * Console Close
 **********************************************************************/

static int
_con_eachclose(struct vnode *file, uint32_t flags){
    dprintf(3, "_con_eachclose\n");
    if(flags == O_RDWR || flags == O_RDONLY) {
        if(console.serial == NULL) {
            return EFAULT;
        }
        int err = serial_register_handler(console.serial, NULL);
        if(err){ // should not happen
            return EFAULT;
        }

        console.buf_size = 0;
        _con_read_state.cur_proc_pid = PROC_NULL;
        _con_read_state.opened_for_reading = 0;
    }
    return 0;
}

static int
_con_lastclose(struct vnode *con_vn) {
    /* If any of these fails, that means we have a bug */
    dprintf(3, "_con_lastclose\n");
    assert(con_vn->vn_ops != NULL);
    assert(con_vn->vn_opencount == 1);

    console.serial    = NULL;
    console.buf_size  = 0;
    console.start     = 0;
    console.end       = 0;

    return 0;
}

/**********************************************************************
 * Console Write
 **********************************************************************/

static void
_con_write(struct vnode *file, const char* buf, size_t nbytes, size_t offset,
          vop_write_cb_t callback, void *token)
{
    dprintf(3, "conwrite\n");
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

/**********************************************************************
 * Console Read
 **********************************************************************/

typedef struct {
    size_t len;
    int first_half_size;
    int second_half_size;
    char* buf;
    char* kbuf;
    vop_read_cb_t callback;
    void* token;
} _con_read_cont_t;

typedef void (*copyout_cb_t)(void* token, int err);
static void _con_read_part2(void* token, int err);
static void _con_read_end(void* token, int err);

static void
_con_read(struct vnode *file, char* buf, size_t nbytes, size_t offset,
         vop_read_cb_t callback, void *token)
{
    dprintf(3, "_con_read called\n");
    (void)offset;
    int err;
    (void)err;
    dprintf(3, "nbytes = %u, %d\n",nbytes,  nbytes);

    if(console.buf_size > 0){
        size_t len = 0;
        dprintf(3, "console start = %d, nbytes = %u, console.buf_size = %u \n",console.start, nbytes, console.buf_size);

        for(size_t cur = console.start; len < nbytes && len < console.buf_size; cur++, cur%=MAX_IO_BUF){
            dprintf(3, "\n%c\n",console.buf[cur]);
            len++;
            if(console.buf[cur] == '\n') {
                break;
            }
        }

        dprintf(3, "console read size = %d\n",len);
        //dprintf(3, "copying out %d bytes, buffer size = %u\n", len, console.buf_size);


        //Since we are using a circular buffer, we need to split our copy into two chunkcs
        //because our buffer may start and wrap over the buffer, also copyout should not
        //know that we are using a circular buffer.

        _con_read_cont_t* cont = malloc(sizeof(_con_read_cont_t));
        if (cont == NULL) {
            _con_read_state.is_blocked = 0;
            callback(token, ENOMEM, 0, false);
            return;
        }
        cont->len = len;
        cont->first_half_size = MIN(MAX_IO_BUF, console.start + len) - console.start;
        cont->buf = buf;
        cont->callback = callback;
        cont->token = token;

        dprintf(3, "_con_read buf = %p\n",buf);
        //copy first half of circular buffer
        err = copyout((seL4_Word)buf, (seL4_Word)console.buf + console.start,
                cont->first_half_size, _con_read_part2, (void*)cont);
        if (err) {
            _con_read_end((void*)cont, err);
        }
        return;
    } else {
        //dprintf(3, "_con_read: blocked\n");
        _con_read_state.file       = file;
        _con_read_state.buf        = buf;
        _con_read_state.is_blocked = 1;
        _con_read_state.callback   = callback;
        _con_read_state.token      = token;
        _con_read_state.offset     = offset;
        _con_read_state.nbytes     = nbytes;
        return;
    }

    //dprintf(3, "_con_read out\n");
}

static void 
_con_read_part2(void* token, int err){
        _con_read_cont_t* cont = (_con_read_cont_t*)token;
        if (err) {
            //console.start = cont->console_start_ori;
            _con_read_state.is_blocked = 0;
            dprintf(3, "console err \n");
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
                _con_read_end, token);
        if (err) {
            _con_read_end((void*)cont, err);
            return;
        }
}

static void 
_con_read_end(void* token, int err){
        _con_read_cont_t* cont = (_con_read_cont_t*)token;

        //should not actually shift console start since this is now asynchronous
        //console.start += cont->second_half_size;
        //console.start %= MAX_IO_BUF;
        if (err) {
            //console.start = cont->console_start_ori;
            _con_read_state.is_blocked = 0;
            dprintf(3, "console err \n");
            cont->callback(cont->token, EFAULT, 0, false);
            return;
        }

        //Only update console.start after successfully executing all the operations
        console.start += cont->first_half_size + cont->second_half_size;
        console.start %= MAX_IO_BUF;

        console.buf_size -= cont->len;

        _con_read_state.is_blocked = 0;
        dprintf(3, "console read size = %d\n",cont->len);
        cont->callback(cont->token, 0, cont->len, false);
        free(cont);
}
