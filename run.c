#include "sish.h"

int run(cmd *c, bool tracing) {
    bool bg;

    bg = false;

    if (tracing) {
        for (cmd *cur = c; cur; cur = cur->next) {
            fprintf(stderr, "+ %s", cur->exe->str);
            for (args *arg = cur->exe->next; arg; arg = arg->next)
                fprintf(stderr, " %s", arg->str);
            fprintf(stderr, "\n");
        }
    }

    for (cmd *cur = c; cur; cur = cur->next)
        bg = bg || cur->bg;

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
    } else if (strcmp(c->exe->str, "echo") == 0 && !c->next) { // don't use this if we have pipes
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
        int pipefd[2] = {-1, -1};
        int exitstatus = 0;
        for (cmd *cur = c; cur; cur = cur->next) {
            int outfd = -1;
            int infd = -1;
            int parent_close = -1;
            int child_close = -1;

            // We need to close the appropriate pipe fds in the shell process
            // and first child because otherwise, one process won't get EOF
            // when the other one has exited. Thus the need for
            // {parent,child}_close, and calls to close() below.

            if (cur->in.type == PIPE) {
                infd = pipefd[READ];

                close(pipefd[WRITE]);
            } else if (cur->in.type == A_FILE) {
                if ((infd = open(cur->in.filename, O_RDONLY)) < 0) {
                    perror(cur->in.filename);
                    continue;
                }
            }

            if (cur->out.type == PIPE) {
                parent_close = pipefd[READ]; // close the READ end after exec'ing the child

                if (pipe(pipefd) == -1) {
                    perror("pipe");
                    break;
                }
                outfd = pipefd[WRITE];

                child_close = pipefd[READ];
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
                setpgid(0, c->pid == 0 ? getpid() : c->pid);

                if (child_close != -1)
                    close(child_close);

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
                if (execvp(argv[0], argv) == -1) {
                    // bash and sh exit with 127 if the command is not found or
                    // file is not executable
                    if (errno == ENOENT)
                        errx(127, "%s: command not found", argv[0]);
                    else
                        err(127, "%s", argv[0]);
                }
            } else {
                if (parent_close != -1) {
                    close(parent_close);
                }

                setpgid(cur->pid, c->pid); // race with child

                if (!bg && cur == c) { // do this only for the first process
                    // set the foreground process group for the controlling terminal so that signals are delivered
                    if (tcsetpgrp(STDOUT_FILENO, c->pid) == -1)
                        perror("tcsetpgrp");
                }

                if (bg) {
                    // BUG: race condition with SIGCHLD handler, will cause issues if signal is delivered while this line is executing
                    bgpids[n_bg++] = cur->pid;
                }
            }
        }

        if (pipefd[READ] != -1) {
            close(pipefd[READ]);
        }

        if (!bg) {
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

                if (!cur->next)
                    exitstatus = ret;
            }

            // if we try to configure the terminal frmo the background
            // process group, we get SIGTTOU, so ignore it
            signal(SIGTTOU, SIG_IGN);
            if (tcsetpgrp(STDOUT_FILENO, getpid()) == -1)
                perror("tcsetpgrp");
            signal(SIGTTOU, SIG_DFL);
        }

        return exitstatus;
    }
}

