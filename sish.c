#include "sish.h"

int exit_status = 0;

pid_t bgpids[1024]; // 1024 should be a sufficiently large size for the number of
                    // possible in-flight background processes
int n_bg = 0;

void usage() {
    fprintf(stderr,
            "Usage: %s [-x] [-c command]\n"
            "\t-x            Enable tracing mode\n"
            "\t-c command    Execute the given command\n",
            getprogname());
    exit(1);
}

// TODO: can SIGCHLD be delivered while the handler is running?
void sigchld() {
    int dummy;

    for (int i = 0; i < n_bg; ++i) {
        if (waitpid(bgpids[i], &dummy, WNOHANG) == bgpids[i]) {
            memmove(&bgpids[i], &bgpids[i+1], sizeof(bgpids[0]) * (n_bg - i - 1));
            --n_bg;
        }
    }
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

    sigset_t chld;
    sigemptyset(&chld);
    sigaddset(&chld, SIGCHLD);
    sigaction(SIGCHLD, &(struct sigaction){
        .sa_handler = sigchld,
        .sa_mask = chld,
        .sa_flags = SA_RESTART
    }, NULL);

    //signal(SIGCHLD, sigchld);
    setenv("SHELL", "/bin/sish", true);

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

        cmd *c = parse(line, false);
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
