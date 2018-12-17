#include <sys/ioctl.h>
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
    enum { DEFAULT, PIPE, A_FILE, APPEND_FILE } type;
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

char *t[] = {
    "std",
    "pipe",
    "file",
    ">>file"
};

cmd *parse(char *line, bool pipe_in) {
    // printf("parsing [%s]\n", line);
    cmd *c;
    size_t end;
    args *arg;

    c = calloc(sizeof(cmd), 1);
    c->exe = &(args){
        .next = NULL,
        .str = ""
    };
    end = 0;
    if (pipe_in)
        c->in.type = PIPE;

    while (true) {
        end = strcspn(line, "<>|& \t");
        if (end > 0) {
            arg = c->exe;
            while (arg->next)
                arg = arg->next;
            arg->next = calloc(sizeof(args), 1);
            arg->next->str = strndup(line, end);
            line += end;
        }

        if (*line == '\0')
            break;

        while (*line == ' ' || *line == '\t')
            ++line;

        if (*line == '<' || *line == '>') {
            stream *st;
            if (*line == '>') {
                st = &c->out;
                if (line[1] == '>') {
                    st->type = APPEND_FILE;
                    ++line;
                } else
                    st->type = A_FILE;
            } else {
                st = &c->in;
                st->type = A_FILE;
            }

            ++line;
            end = strcspn(line, "<>|& \t");
            st->filename = strndup(line, end);
            line += end;
        } else if (*line == '|') {
            c->out.type = PIPE;
            line++;
            break;
        } else if (*line == '&') {
            c->bg = true;
            ++line;
        }
    }

    if (*line != '\0')
        c->next = parse(line, c->out.type == PIPE);

    c->exe = c->exe->next; // skip dummy empty string

#if DEBUG
    int i = 0;
    for (cmd *cur = c; cur; ++i, cur = cur->next) {
        for (int j=0;j<i;++j)
            printf("\t");
        printf("exe=[");
        arg = cur->exe;
        while (arg) {
            printf("'%s', ", arg->str);
            arg = arg->next;
        }
        printf("] ");
        switch(cur->in.type) {
            case DEFAULT:
                break;
            case PIPE:
                printf("<pipe");
                break;
            case A_FILE:
                printf("<%s", cur->in.filename);
                break;
            case APPEND_FILE:
                abort();
                break;
        }
        printf(" ");
        switch(cur->out.type) {
            case DEFAULT:
                break;
            case PIPE:
                printf(">pipe");
                break;
            case A_FILE:
                printf(">%s", cur->out.filename);
                break;
            case APPEND_FILE:
                printf(">>%s", cur->out.filename);
                break;
        }

        printf(", bg=%d\n", cur->bg);
    }
#endif

    return c;
}

bool validate(cmd *c) {
    for (cmd *cur = c; cur; cur = cur->next) {
        if (!cur->exe || strlen(cur->exe->str) == 0) {
            fprintf(stderr, "missing command\n");
            return false;
        }

        if (cur->out.type == PIPE && (!cur->next || cur->next->in.type != PIPE)) {
            fprintf(stderr, "invalid pipes\n");
            return false;
        }

        if ((cur->in.type == A_FILE || cur->in.type == APPEND_FILE) && (!cur->in.filename || strlen(cur->in.filename) == 0)) {
            fprintf(stderr, "sish: missing input filename for `%s'\n", cur->exe->str);
            return false;
        }

        if ((cur->out.type == A_FILE || cur->out.type == APPEND_FILE) && (!cur->out.filename || strlen(cur->out.filename) == 0)) {
            fprintf(stderr, "sish: missing output filename for `%s'\n", cur->exe->str);
            return false;
        }
    }

    return true;
}

int exit_status = 0;

int run(cmd *c) {
    if (strcmp(c->exe->str, "cd") == 0) {
        if (c->exe->next == NULL) {
            char *home = getenv("HOME");
            if (!home) {
                struct passwd *pw;

                if ((pw = getpwuid(getuid())) == NULL) {
                    perror("cd");
                    return 1;
                }
            }
            if (chdir(home) == -1) {
                perror("cd");
                return 1;
            }
            return 0;
        } else {
            if (chdir(c->exe->next->str) == -1) {
                perror("cd");
                return 1;
            }
            return 0;
        }
    } else if (strcmp(c->exe->str, "echo") == 0) {
        if (!c->exe->next) {
            fprintf(stderr, "echo: missing argument\n");
            return 1;
        }

        if (strcmp(c->exe->next->str, "$$") == 0) {
            printf("%d\n", getpid());
        } else if (strcmp(c->exe->next->str, "$?") == 0) {
            printf("%d\n", exit_status);
        } else {
            printf("%s\n", c->exe->next->str);
        }

        return 0;
    } else if (strcmp(c->exe->str, "exit") == 0) {
        exit(0);
    } else {
        enum { READ = 0, WRITE = 1 };
        int pipefd[2];
        int exitstatus = 0;
        for (cmd *cur = c; cur; cur = cur->next) {
            int outfd = -1;
            int infd = -1;
            int closefd[2] = {-1,-1}; // fds that need to be closed

            if (cur->in.type == PIPE) {
                infd = pipefd[READ];

                // we need to close the pipe in the shell process because
                // otherwise, the reader won't get EOF when the writer has
                // exited
                close(pipefd[WRITE]);

                closefd[0] = pipefd[WRITE];
            } else if (cur->in.type == A_FILE) {
                if ((infd = open(cur->in.filename, O_RDONLY)) < 0) {
                    perror(cur->in.filename);
                    continue;
                }
            }

            if (cur->out.type == PIPE) {
                if (pipe(pipefd) == -1) {
                    perror("pipe");
                    break;
                }
                outfd = pipefd[WRITE];
                closefd[1] = pipefd[READ];
            } else if (cur->out.type == A_FILE || cur->out.type == APPEND_FILE) {
                if ((outfd = open(cur->out.filename,
                                  O_WRONLY | O_CREAT | ((cur->out.type == APPEND_FILE) ? O_APPEND : 0),
                                  S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)) < 0) {
                    perror(cur->out.filename);
                    continue;
                }
            }

            cur->pid = fork();
            if (cur->pid == -1) {
                perror("fork");
                return 1;
            } else if (cur->pid == 0) { // child
                signal(SIGINT, SIG_DFL);

                printf("exec [%s]<%d >%d\n",
                        cur->exe->str,
                        infd == -1 ? STDIN_FILENO : infd,
                        outfd == -1 ? STDOUT_FILENO : outfd);

                if (closefd[0] != -1)
                    close(closefd[0]);
                if (closefd[1] != -1)
                    close(closefd[1]);

                if (outfd != -1)
                    if (dup2(outfd, STDOUT_FILENO) < -1)
                        perror("dup2");

                if (infd != -1)
                    if (dup2(infd, STDIN_FILENO) < -1)
                        perror("dup2");

                int len = 0;
                for (args *arg = cur->exe; arg; arg = arg->next)
                    ++len;
                int i = 0;
                char **argv = malloc(sizeof(char*) * len + 1);
                for (args *arg = cur->exe; arg; arg = arg->next)
                    argv[i++] = arg->str;
                argv[i] = NULL;
                if (execvp(argv[0], argv) == -1)
                    // bash and sh exit with 127 if the command is not found or
                    // file is not executable
                    err(127, "%s", argv[0]);
            }
        }

        for (cmd *cur = c; cur; cur = cur->next) {
            if (cur->pid == 0)
                continue;

            int res;
            if (waitpid(cur->pid, &res, 0) == -1) {
                perror("waitpid");
                continue;
            }

            int ret;
            if (WIFEXITED(res))
                ret = WEXITSTATUS(res);
            else if (WIFSIGNALED(res))
                ret = 0200 | WTERMSIG(res);
            else
                err(1, "unknown exit condition %d", res);

            printf("exit status of %s = %d\n", cur->exe->str, ret);
            if (!cur->next)
                exitstatus = ret;
        }

        return exitstatus;
    }
}

void usage() {
    fprintf(stderr,
            "Usage: %s [-x] [-c command]\n"
            "\t-x            Enable tracing mode\n"
            "\t-c command    Execute the given command\n",
            getprogname());
    exit(1);
}

int main(int argc, char *argv[]) {
    char *line;
    ssize_t len;
    size_t capacity;
    int ch;
    bool tracing;

    line = NULL;
    capacity = 0;
    tracing = false;
    (void)tracing; // TODO remove

    while ((ch = getopt(argc, argv, "c:x")) != -1) {
        switch (ch) {
            case 'x':
                tracing = true;
                break;
            case 'c':
                if (optarg)
                    line = strdup(optarg);
                else
                    usage();
                break;
            default:
                usage();
                break;
        }
    }

    signal(SIGINT, SIG_IGN);

    if (line) {
        cmd *c = parse(line, false);
        if (validate(c))
            return run(c);
        else
            return 1;
    }

    printf("sish$ ");

    while ((len = getline(&line, &capacity, stdin)) != -1) {
        // remove newline, I don't want to deal with parsing it
        line[len - 1] = '\0';

        cmd *c = parse(line, false);
        if (validate(c))
            exit_status = run(c);

        free(line);
        line = NULL;
        capacity = 0;

        printf("sish$ ");
    }

    if (errno)
        perror("getline");
}
