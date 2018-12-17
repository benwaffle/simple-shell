#include "sish.h"

int run(cmd *c, bool tracing);

enum { READ = 0, WRITE = 1 }; // for pipes

/**
 * Executes the cmd `c', connecting pipes, handling signals, background
 * processes, built-ins, and tracing.
 */
int
run(cmd *c, bool tracing)
{
    bool bg;
    char *home, **argv;
    struct passwd *pw;
    int pipefd[2];
    int outfd, infd, parent_close, child_close, exitstatus, argc, i, res, ret;

    outfd = infd = parent_close = child_close = pipefd[0] = pipefd[1] = -1;
    exitstatus = argc = 0;
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
            home = getenv("HOME");
            if (home == NULL) {
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
    } else if (strcmp(c->exe->str, "echo") == 0 && !c->next) {
        if (c->exe->next == NULL) {
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
        exit(exit_status);
    } else {
        for (cmd *cur = c; cur; cur = cur->next) {
            outfd = -1;
            infd = -1;
            parent_close = -1;
            child_close = -1;

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
                // close the READ end after exec'ing the child
                parent_close = pipefd[READ];

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
                if (signal(SIGINT, SIG_DFL) == SIG_ERR)
                    perror("signal");
                if (setpgid(0, c->pid == 0 ? getpid() : c->pid) == -1)
                    perror("setpgid");

                if (child_close != -1)
                    close(child_close);

                if (outfd != -1)
                    if (dup2(outfd, STDOUT_FILENO) < -1)
                        perror("dup2");

                if (infd != -1)
                    if (dup2(infd, STDIN_FILENO) < -1)
                        perror("dup2");

                argc = 0;
                for (args *arg = cur->exe; arg; arg = arg->next)
                    ++argc;

                argv = malloc(sizeof(char*) * argc + 1);
                if (argv == NULL)
                    err(1, "malloc");

                i = 0;
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

                if (setpgid(cur->pid, c->pid) == -1) // race with child
                    perror("setpgid");

                if (!bg && cur == c) { // do this only for the first process
                    // set the foreground process group for the controlling
                    // terminal so that signals are delivered
                    if (tcsetpgrp(STDOUT_FILENO, c->pid) == -1)
                        perror("tcsetpgrp");
                }

                if (bg) {
                    // BUG: race condition with SIGCHLD handler, will cause
                    // issues if signal is delivered while this line is
                    // executing
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

                if (waitpid(cur->pid, &res, 0) == -1) {
                    perror("waitpid");
                    continue;
                }

                if (WIFEXITED(res))
                    ret = WEXITSTATUS(res);
                else if (WIFSIGNALED(res))
                    ret = 0200 | WTERMSIG(res);
                else
                    err(1, "unknown exit condition %d", res);

                // the exit status for a pipe chain is the exit status of the
                // last command
                if (cur->next == NULL)
                    exitstatus = ret;
            }

            // if we try to configure the terminal frmo the background
            // process group, we get SIGTTOU, so ignore it
            if (signal(SIGTTOU, SIG_IGN) == SIG_ERR)
                perror("signal");
            if (tcsetpgrp(STDOUT_FILENO, getpid()) == -1)
                perror("tcsetpgrp");
            if (signal(SIGTTOU, SIG_DFL) == SIG_ERR)
                perror("signal");
        }

        return exitstatus;
    }
}

