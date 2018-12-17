#include "sish.h"

void usage();
void sigchld();
int main(int, char **);

int exit_status = 0;

pid_t bgpids[1024]; // 1024 should be a sufficiently large size for the number
                    // of possible in-flight background processes
int n_bg = 0;

void
usage()
{
    fprintf(stderr,
            "Usage: %s [-x] [-c command]\n"
            "\t-x            Enable tracing mode\n"
            "\t-c command    Execute the given command\n",
            getprogname());
    exit(EXIT_FAILURE);
}

// TODO: can SIGCHLD be delivered while the handler is running?
/**
 * Handles SIGCHLD, to clean up zombie background processes
 */
void
sigchld()
{
    int dummy, res;

    for (int i = 0; i < n_bg; ++i) {
        res = waitpid(bgpids[i], &dummy, WNOHANG);
        if (res == bgpids[i]) {
            memmove(&bgpids[i], &bgpids[i+1], sizeof(bgpids[0]) * (n_bg - i - 1));
            --n_bg;
        } else if (res == -1) {
            perror("waitpid");
        }
    }
}

/**
 * sish: a very simple command-line interpreter or shell.  It is suitable to be
 * used interactively or as a login shell.  It only implements a very small
 * subset of what would usually be expected of a Unix shell, and does
 * explicitly not lend itself as a scripting language.
 */
int
main(int argc, char *argv[])
{
    char *line;
    cmd *c;
    ssize_t len;
    size_t capacity;
    int ch;
    bool tracing;

    line = NULL;
    capacity = 0;
    tracing = false;

    setprogname(argv[0]);

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

    if (signal(SIGINT, SIG_IGN) == SIG_ERR)
        err(1, "signal");

    sigset_t chld;
    sigemptyset(&chld);
    sigaddset(&chld, SIGCHLD);
    if (sigaction(SIGCHLD, &(struct sigaction){
        .sa_handler = sigchld,
        .sa_mask = chld,
        .sa_flags = SA_RESTART
    }, NULL) == -1)
        err(1, "sigaction");

    if (setenv("SHELL", "/bin/sish", true) == -1)
        err(1, "setenv");

    if (line) {
        cmd *c = parse(line, false);
        if (c && validate(c))
            return run(c, tracing);
        else
            return 1;
    }

    printf("sish$ ");

    // getline forever, and restart on interrupt
    while ((len = getline(&line, &capacity, stdin)) != -1 || errno == EINTR) {
        if (errno == EINTR) {
            errno = 0;
            continue;
        }

        // remove newline, I don't want to deal with parsing it
        line[len - 1] = '\0';

        c = parse(line, false);
        if (c && validate(c))
            exit_status = run(c, tracing);

        free(line);
        line = NULL;
        capacity = 0;

        errno = 0;

        printf("sish$ ");
    }

    if (errno)
        perror("getline");

    return exit_status;
}
