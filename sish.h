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

typedef struct {
    enum { DEFAULT = 0, PIPE, A_FILE, APPEND_FILE } type;
    char *filename;
} stream;

typedef struct args {
    struct args *next;
    char *str;
} args;

typedef struct cmd {
    args *exe;
    stream out;
    stream in;
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
