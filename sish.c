#include <sys/ioctl.h>

#include <err.h>
#include <errno.h>
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
                if (line[1] == '>')
                    st->type = APPEND_FILE;
                else
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
    return c;
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

    /*
    parse("<asdf cat | wc -l", false);
    parse("wc -l <b", false);
    parse("echo $?", false);
    parse("ls", false);
    parse("ls | wc -l", false);
    parse("echo $$", false);
    parse("find / >/dev/null &", false);
    parse("aed -e <file >file.enc", false);
    parse("cmd | sort | uniq -c | sort -n", false);
    parse("something", false);
    parse("rm /etc/passwd", false);
    parse("exit", false);
    parse("cd /tmp", false);
    parse("pwd", false);
    */

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

    printf("line = %s, tracing = %d\n", line, tracing);
    free(line);

    signal(SIGINT, SIG_IGN);

    printf("sish$ ");

    while ((len = getline(&line, &capacity, stdin)) != -1) {
        // remove newline, I don't want to deal with parsing it
        line[len - 1] = '\0';

        // printf("you wrote [%s] (len %zd)\n", line, len);

        cmd *c = parse(line, false);
        (void)c;
        // if (validate(c))
        //     run(c);

        free(line);
        line = NULL;
        capacity = 0;

        printf("sish$ ");
    }

    if (errno)
        perror("getline");
}
