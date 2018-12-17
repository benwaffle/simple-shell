#ifndef _SISH_H_
#define _SISH_H_

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct stream {
    char *filename;
    enum { DEFAULT = 0, PIPE, A_FILE, APPEND_FILE } type;
} stream;

typedef struct args {
    struct args *next;
    char *str;
} args;

typedef struct cmd {
    stream in;
    stream out;
    args *exe;
    struct cmd *next;
    bool bg;
    pid_t pid;
} cmd;

extern int exit_status;
extern pid_t bgpids[1024];
extern int n_bg;

cmd *parse(char *, bool);
bool validate(cmd *);
int run(cmd *, bool);

#endif // !_SISH_H_
