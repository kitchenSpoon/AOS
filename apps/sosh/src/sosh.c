/*
 * Copyright 2014, NICTA
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(NICTA_BSD)
 */

/* Simple shell to run on SOS */

#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <utils/time.h>

/* Your OS header file */
#include <sos.h>

#define BUF_SIZ   6000
#define MAX_ARGS   32

static int in;
static sos_stat_t sbuf;

static int benchmark();
static int benchmark2();
static int thrash();

static void prstat(const char *name) {
    /* print out stat buf */
    printf("%c%c%c%c %8zu %8lu %s\n",
            sbuf.st_type == ST_SPECIAL ? 's' : '-',
            sbuf.st_mode & S_IRUSR ? 'r' : '-',
            sbuf.st_mode & S_IWUSR ? 'w' : '-',
            sbuf.st_mode & S_IXUSR ? 'x' : '-',
            sbuf.st_size, sbuf.st_mtime.seconds/60, name);
}

static int cat(int argc, char **argv) {
    int fd;
    char buf[BUF_SIZ];
    int num_read, stdout_fd, num_written = 0;


    if (argc != 2) {
        printf("Usage: cat filename\n");
        return 1;
    }

    printf("<%s>\n", argv[1]);

    fd = open(argv[1], O_RDONLY);
    assert(fd >= 0);
    stdout_fd = open("console", O_WRONLY);


    while ((num_read = read(fd, buf, BUF_SIZ)) > 0) {
    printf("cat, num_read = %d, num_write = %d\n",num_read,num_written);
        num_written = write(stdout_fd, buf, num_read);
    }
    printf("cat, num_read = %d, num_write = %d\n",num_read,num_written);
    close(stdout_fd);
    close(fd);

    if (num_read == -1 || num_written == -1) {
        printf("error on write\n");
        return 1;
    }

    return 0;
}

static int cp(int argc, char **argv) {
    int fd, fd_out;
    char *file1, *file2;
    char buf[BUF_SIZ];
    int num_read, num_written = 0;

    if (argc != 3) {
        printf("Usage: cp from to\n");
        return 1;
    }

    file1 = argv[1];
    file2 = argv[2];

    fd = open(file1, O_RDONLY);
    fd_out = open(file2, O_WRONLY);

    assert(fd >= 0);

    while ((num_read = read(fd, buf, BUF_SIZ)) > 0)
        num_written = write(fd_out, buf, num_read);

    close(fd);
    close(fd_out);
    printf("read = %d, write = %d\n", num_read, num_written);
    if (num_read == -1 || num_written == -1) {
        printf("error on cp\n");
        return 1;
    }

    return 0;
}

#define MAX_PROCESSES 10

static int ps(int argc, char **argv) {
    sos_process_t *process;
    int i, processes;

    process = malloc(MAX_PROCESSES * sizeof(*process));

    if (process == NULL) {
        printf("%s: out of memory\n", argv[0]);
        return 1;
    }

    processes = sos_process_status(process, MAX_PROCESSES);

    printf("      TID SIZE   STIME   CTIME COMMAND\n");

    for (i = 0; i < processes; i++) {
        printf("%8d %4x %7d   %s\n", process[i].pid, process[i].size,
                process[i].stime, process[i].command);
    }

    free(process);

    return 0;
}

static int exec(int argc, char **argv) {
    pid_t pid;
    int r;
    int bg = 0;

    if (argc < 2 || (argc > 2 && argv[2][0] != '&')) {
        printf("Usage: exec filename [&]\n");
        return 1;
    }

    if ((argc > 2) && (argv[2][0] == '&')) {
        bg = 1;
    }

    if (bg == 0) {
        r = close(in);
        assert(r == 0);
    }

    pid = sos_process_create(argv[1]);
    if (pid >= 0) {
        printf("Child pid=%d\n", pid);
        if (bg == 0) {
            sos_process_wait(pid);
        }
    } else {
        printf("Failed!\n");
    }
    if (bg == 0) {
        in = open("console", O_RDONLY);
        assert(in >= 0);
    }
    //while(1){
    //    printf("this is sosh!\n");
    //    sleep(6);
    //}
    return 0;
}

static int kill(int argc, char **argv) {
    pid_t pid;

    if (argc != 2) {
        printf("Usage: %s <pid>\n", argv[0]);
        return 1;
    }

    pid = (pid_t)atoi(argv[1]);
    if (sos_process_delete(pid)) {
        printf("invalid process\n");
        return 1;
    }

    return 0;
}

static int dir(int argc, char **argv) {
    int i = 0, r;
    char buf[BUF_SIZ];

    if (argc > 2) {
        printf("usage: %s [file]\n", argv[0]);
        return 1;
    }

    if (argc == 2) {
        r = sos_stat(argv[1], &sbuf);
        if (r < 0) {
            printf("stat(%s) failed: %d\n", argv[1], r);
            return 0;
        }
        prstat(argv[1]);
        return 0;
    }

    while (1) {
        r = sos_getdirent(i, buf, BUF_SIZ);
        if (r < 0) {
            printf("dirent(%d) failed: %d\n", i, r);
            break;
        } else if (!r) {
            break;
        }
        printf("dirent(%d): \"%s\"\n", i, buf);
        r = sos_stat(buf, &sbuf);
        if (r < 0) {
            printf("stat(%s) failed: %d\n", buf, r);
            break;
        }
        prstat(buf);
        i++;
    }
    return 0;
}

static int second_sleep(int argc,char *argv[]) {
    if (argc != 2) {
        printf("Usage %s seconds\n", argv[0]);
        return 1;
    }
    sleep(atoi(argv[1]));
    return 0;
}

static int milli_sleep(int argc,char *argv[]) {
    struct timespec tv;
    uint64_t nanos;
    if (argc != 2) {
        printf("Usage %s milliseconds\n", argv[0]);
        return 1;
    }
    nanos = (uint64_t)atoi(argv[1]) * NS_IN_MS;
    /* Get whole seconds */
    tv.tv_sec = nanos / NS_IN_S;
    /* Get nanos remaining */
    tv.tv_nsec = nanos % NS_IN_S;
    nanosleep(&tv, NULL);
    return 0;
}

static int second_time(int argc, char *argv[]) {
    printf("%d seconds since boot\n", (int)time(NULL));
    return 0;
}

static int micro_time(int argc, char *argv[]) {
    struct timeval time;
    gettimeofday(&time, NULL);
    uint64_t micros = (uint64_t)time.tv_sec * US_IN_S + (uint64_t)time.tv_usec;
    printf("%llu microseconds since boot\n", micros);
    return 0;
}

static uint64_t getmicro_time() {
    struct timeval time;
    gettimeofday(&time, NULL);
    uint64_t micros = (uint64_t)time.tv_sec * US_IN_S + (uint64_t)time.tv_usec;
    return micros;
}

static int whoami(int argc, char **argv) {
    pid_t pid;

    pid = sos_my_id();
    printf("pid = %d\n", pid);
    return 0;
}

struct command {
    char *name;
    int (*command)(int argc, char **argv);
};

struct command commands[] = { { "dir", dir }, { "ls", dir }, { "cat", cat },
    { "cp", cp }, { "ps", ps }, { "exec", exec }, {"sleep",second_sleep},
    {"msleep",milli_sleep}, {"time", second_time}, {"mtime", micro_time},
    {"kill", kill}, {"bm", benchmark}, {"bm2", benchmark2}, {"thrash", thrash},
    {"whoami", whoami} };

static void test_file_syscalls(void) {
    printf("Start file syscalls test...\n");
    int fd, fd2, ret;
    char buf[] = "This is cool!!\n";
    char buf2[] = "This is even cooler!!!\n";
    char buf3[10];

    printf("Test open & write, should see: %s", buf);
    fd = open("console", O_RDWR);
    assert(fd >= 0);

    ret = write(fd, buf, sizeof(buf));
    assert(ret == sizeof(buf));
    printf("Passed!\n");

    printf("Test multiple write to the console, should see: %s", buf2);
    fd2 = open("console", O_WRONLY);
    assert(fd2 > fd && fd2 != fd);

    ret = write(fd2, buf2, sizeof(buf2));
    assert(ret == sizeof(buf2));
    printf("Passed!\n");

    printf("Closing both file descriptors %d %d\n", fd, fd2);
    ret = close(fd);
    assert(ret == 0);
    ret = close(fd2);
    assert(ret == 0);
    printf("Done\n---\n");

    /* write to a closed file */
    printf("Test writing to a closed file\n");
    ret = write(fd, buf, sizeof(buf));
    assert(ret < 0);
    printf("Failed, as expected!\n---\n");

    /* Write to a read only file */
    printf("Test writing for readonly file\n");
    fd = open("console", O_RDONLY);
    assert(fd >= 0);

    ret = write(fd, buf3, sizeof(buf3)); // should fail
    assert(ret < 0);
    printf("Failed, as expected!\n");

    printf("Closing the file\n");
    ret = close(fd);
    assert(ret == 0);
    printf("Done\n---\n");

    /* Read from write only file */
    printf("Testing reading a write only file\n");
    fd = open("console", O_WRONLY);
    assert(fd >= 0);

    ret = read(fd, buf, sizeof(buf)); // should fail
    assert(ret < 0);
    printf("Failed, as expected!\n");

    printf("Closing the file\n");
    ret = close(fd);
    assert(ret == 0);
    printf("Done\n---\n");

    /* Reading twice for console */
    printf("Test opening read two times\n");
    fd = open("console", O_RDWR);
    assert(fd > 0);
    fd2 = open("console", O_RDONLY);
    assert(fd2 < 0);
    printf("Failed, as expected!\n");


    ret = close(fd);
    assert(ret == 0);
    ret = close(fd2);
    assert(ret == -1);
    printf("Done\n");

    printf("End file syscalls test\n");
}

static void
test_dynamic_heap(void) {
    printf("Beginning dynamic heap...\n");
    void* addr;

    printf("current brk is at: 0x%08x\n", sbrk((intptr_t)0));
    printf("Mallocing 8K memory...\n");
    addr = malloc((1<<13));
    printf("Touching the address\n");
    *(int*)addr = 100;
    addr += (1<<12);
    *(int*)addr = 100;
    addr += (1<<11);
    *(int*)addr = 100;

    printf("brk is now at: 0x%08x\n", sbrk((intptr_t)0));
    printf("Mallocing 10K memory...\n");
    addr = malloc((1<<13) + (1<<11));
    printf("Touching the address\n");
    *(int*)addr = 100;
    addr += (1<<12);
    *(int*)addr = 100;
    addr += (1<<11);
    *(int*)addr = 100;
    printf("brk is now at: 0x%08x\n", sbrk((intptr_t)0));

    printf("Exiting dynamic heap test\n");
}

static void bm_read(char* filename, char* buf, size_t buf_size){
    printf("Reading with IO buf request %u\n", buf_size);

    int fd = open(filename, O_RDONLY);
    assert(fd >= 0);
    uint64_t start_time = getmicro_time();

    read(fd, buf, buf_size);

    uint64_t end_time = getmicro_time();
    printf("time taken: %lu\n", (long unsigned)(end_time - start_time));
    close(fd);
}

static void bm_write(char* filename, char* buf, size_t buf_size){
    printf("Writing with IO buf request %u\n", buf_size);

    int fd = open(filename, O_WRONLY);
    assert(fd >= 0);
    uint64_t start_time = getmicro_time();

    size_t written = 0;
    while (written < buf_size){
        written += write(fd, buf+written, buf_size-written);
    }

    uint64_t end_time = getmicro_time();
    printf("time taken: %lu\n", (long unsigned)(end_time - start_time));
    close(fd);
}

static int
benchmark(){
    /* Reads */
    printf("Reading\n");
    /* Reading with IO request changing*/
    printf("Reading with IO request changing\n");
    char buf1000[1001];
    char buf5000[5001];
    char buf10000[10000];
    char buf50000[50000];
    char buf100000[100000];
    char buf200000[200000];
    char buf400000[400000];
    char buf800000[800000];
    char buf1600000[1600000];
    char buf3200000[3200000];
    char buf100000_2[100000];
    char buf200000_2[200000];
    char buf400000_2[400000];
    bm_read("read_test_1000", (char*)buf1000,1000);
    bm_write("write_test_1000", (char*)buf1000,1000);

    bm_read("read_test_5000", (char*)buf5000,5000);
    bm_write("write_test_5000", (char*)buf5000,5000);

    bm_read("read_test_10000", (char*)buf10000,10000);
    bm_write("write_test_10000", (char*)buf10000,10000);

    bm_read("read_test_50000", (char*)buf50000,50000);
    bm_write("write_test_50000", (char*)buf50000,50000);

    bm_read("read_test_100000", (char*)buf100000,100000);
    bm_write("write_test_100000", (char*)buf100000,100000);

    bm_read("read_test_200000", (char*)buf200000,200000);
    bm_write("write_test_200000", (char*)buf200000,200000);

    bm_read("read_test_400000", (char*)buf400000,400000);
    bm_write("write_test_400000", (char*)buf400000,400000);

    bm_read("read_test_800000", (char*)buf800000,800000);
    bm_write("write_test_800000", (char*)buf800000,800000);

    bm_read("read_test_1600000", (char*)buf1600000,1600000);
    bm_write("write_test_1600000", (char*)buf1600000, 1600000);

    bm_read("read_test_3200000", (char*)buf3200000,3200000);
    bm_write("write_test_3200000", (char*)buf3200000, 3200000);

    bm_read("read_test_100000", (char*)buf100000_2,100000);
    bm_write("write_test_100000_2", (char*)buf100000_2,100000);

    bm_read("read_test_200000", (char*)buf200000_2,200000);
    bm_write("write_test_200000_2", (char*)buf200000_2,200000);

    bm_read("read_test_400000", (char*)buf400000_2,400000);
    bm_write("write_test_400000_2", (char*)buf400000_2,400000);

    /* Reading with packet changing */

    /* Writes */
    printf("Writing\n");
    /* Writing with IO request changing*/
    /* Writing with packet changing */
    return 0;
}

static int
benchmark2() {
    char buf3200000[3200000];
    bm_read("read_test_3200000_1", (char*)buf3200000, 3200000);
    bm_write("write_test_3200000_1", (char*)buf3200000, 3200000);

    bm_read("read_test_3200000_2", (char*)buf3200000, 3200000);
    bm_write("write_test_3200000_2", (char*)buf3200000, 3200000);

    bm_read("read_test_3200000_3", (char*)buf3200000, 3200000);
    bm_write("write_test_3200000_3", (char*)buf3200000, 3200000);

    bm_read("read_test_3200000_4", (char*)buf3200000, 3200000);
    bm_write("write_test_3200000_4", (char*)buf3200000, 3200000);

    bm_read("read_test_3200000_5", (char*)buf3200000, 3200000);
    bm_write("write_test_3200000_5", (char*)buf3200000, 3200000);

    bm_read("read_test_3200000_6", (char*)buf3200000, 3200000);
    bm_write("write_test_3200000_6", (char*)buf3200000, 3200000);

    bm_read("read_test_3200000_7", (char*)buf3200000, 3200000);
    bm_write("write_test_3200000_7", (char*)buf3200000, 3200000);
    return 0;
}

static
int thrash(int argc, char *argv[]) {
    if (argc != 3 ) {
        printf("Usage: thrash num_kilobytes start_char[a-z]\n");
        return 1;
    }
    int nbyte = atoi(argv[1])*1024;
    char ch = argv[2][0];
    if (ch < 'a' || ch > 'z') {
        return 1;
    }

    char *big_buf = malloc(nbyte);
    if(big_buf == NULL) {
        printf("thrash failed to allocate enough memory\n");
        return 1;
    }

    int mod = 'z' - 'a';

    for(int i = 0; i < nbyte; i++){
        if (i % 1024 == 0) {
            printf("filling kbyte #%d\n", i >> 10);
        }
        big_buf[i] = ch;
        ch = (ch + 1) % mod + 'a';
    }

    ch = argv[2][0];
    for (int i=0; i<nbyte; i++) {
        if (i % 1024 == 0) {
            printf("testing kbyte #%d\n", i >> 10);
        }
        assert(big_buf[i] == ch);
        ch = (ch + 1) % mod + 'a';
    }

    //free(big_buf);
    return 0;
}

int main(void) {
    char buf[BUF_SIZ];
    char *argv[MAX_ARGS];
    int i, r, done, found, new, argc;
    char *bp, *p;

/*    for (int i=0; i<3; i++) {
        r = sos_getdirent(i, buf, BUF_SIZ);
        printf("buf[%d] = %s\n", i, buf);
    }
    test_dynamic_heap();
    benchmark();
*/
//    test_file_syscalls();

    //test_swapping();

    in = open("console", O_RDONLY);
    assert(in >= 0);


    bp = buf;
    done = 0;
    new = 1;

    printf("\n[SOS Starting]\n");

    while (!done) {
        if (new) {
            printf("$ ");
        }
        new = 0;
        found = 0;

        while (!found && !done) {
            /* Make sure to flush so anything is visible while waiting for user input */
            fflush(stdout);
            r = read(in, bp, BUF_SIZ - 1 + buf - bp);
            if (r < 0) {
                done = 1;
                break;
            }
            bp[r] = 0; /* terminate */
            //printf("read: %d bytes:<%s>\n", r, bp);
            for (p = bp; p < bp + r; p++) {
                if (*p == '\03') { /* ^C */
                    printf("^C\n");
                    p = buf;
                    new = 1;
                    break;
                } else if (*p == '\04') { /* ^D */
                    p++;
                    found = 1;
                } else if (*p == '\010' || *p == 127) {
                    /* ^H and BS and DEL */
                    if (p > buf) {
                        printf("\010 \010");
                        p--;
                        r--;
                    }
                    p--;
                    r--;
                } else if (*p == '\n') { /* ^J */
                    printf("%c", *p);
                    *p = 0;
                    found = p > buf;
                    p = buf;
                    new = 1;
                    break;
                } else {
                    printf("%c", *p);
                }
            }
            bp = p;
            if (bp == buf) {
                break;
            }
        }

        if (!found) {
            continue;
        }

        argc = 0;
        p = buf;

        while (*p != '\0') {
            /* Remove any leading spaces */
            while (*p == ' ')
                p++;
            if (*p == '\0')
                break;
            argv[argc++] = p; /* Start of the arg */
            while (*p != ' ' && *p != '\0') {
                p++;
            }

            if (*p == '\0')
                break;

            /* Null out first space */
            *p = '\0';
            p++;
        }

        if (argc == 0) {
            continue;
        }

        found = 0;

        for (i = 0; i < sizeof(commands) / sizeof(struct command); i++) {
            if (strcmp(argv[0], commands[i].name) == 0) {
                commands[i].command(argc, argv);
                found = 1;
                break;
            }
        }

        /* Didn't find a command */
        if (found == 0) {
            /* They might try to exec a program */
            if (sos_stat(argv[0], &sbuf) != 0) {
                printf("Command \"%s\" not found\n", argv[0]);
            } else if (!(sbuf.st_mode & S_IXUSR)) {
                printf("File \"%s\" not executable\n", argv[0]);
            } else {
                /* Execute the program */
                argc = 2;
                argv[1] = argv[0];
                argv[0] = "exec";
                exec(argc, argv);
            }
        }
    }
    printf("[SOS Exiting]\n");
}
