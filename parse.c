#include "sish.h"

cmd *parse(char*, bool);
bool validate(cmd*);

/**
 * Parses user input `line'. When called, pipe_in should be false. Returns
 * `cmd', a linked list of commands to run (separated by pipes). May return
 * NULL.
 */
cmd *
parse(char *line, bool pipe_in)
{
    cmd *c;
    size_t end;
    args *arg;

    c = calloc(sizeof(cmd), 1);
    if (c == NULL)
        err(1, "calloc");
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
            if (arg->next == NULL)
                err(1, "calloc");
            arg->next->str = strndup(line, end);
            if (arg->next->str == NULL)
                err(1, "strndup");
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
                if (st->type != DEFAULT) {
                    fprintf(stderr, "duplicate file redirection\n");
                    return NULL;
                }

                if (line[1] == '>') {
                    st->type = APPEND_FILE;
                    ++line;
                } else {
                    st->type = A_FILE;
                }
            } else {
                st = &c->in;
                if (st->type != DEFAULT) {
                    fprintf(stderr, "invalid pipes and file redirection\n");
                    return NULL;
                }
                st->type = A_FILE;
            }

            ++line;
            end = strcspn(line, "<>|& \t");
            st->filename = strndup(line, end);
            if (st->filename == NULL)
                err(1, "strndup");
            line += end;
        } else if (*line == '|') {
            if (c->out.type != DEFAULT) {
                fprintf(stderr, "invalid pipes and file redirection\n");
                return NULL;
            }
            c->out.type = PIPE;
            line++;
            break;
        } else if (*line == '&') {
            c->bg = true;
            ++line;
        }
    }

    if (*line != '\0') {
        c->next = parse(line, c->out.type == PIPE);
        if (c->next == NULL)
            return NULL;
    }

    c->exe = c->exe->next; // skip dummy empty string

#if DEBUG
    int i = 0;
    for (cmd *cur = c; cur; ++i, cur = cur->next) {
        for (int j=0; j < i; ++j)
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

/**
 * Validates that the user has specified valid commands, pipes, and file
 * redirection. If this returns false, you should not call run() on the cmd
 */
bool
validate(cmd *c)
{
    for (cmd *cur = c; cur; cur = cur->next) {
        if (cur->exe == NULL || strlen(cur->exe->str) == 0) {
            fprintf(stderr, "missing command\n");
            return false;
        }

        if (cur->out.type == PIPE &&
                (cur->next == NULL || cur->next->in.type != PIPE)) {
            fprintf(stderr, "invalid pipes\n");
            return false;
        }

        if ((cur->in.type == A_FILE || cur->in.type == APPEND_FILE) &&
                (cur->in.filename == NULL || strlen(cur->in.filename) == 0)) {
            fprintf(stderr, "sish: missing input filename for `%s'\n",
                    cur->exe->str);
            return false;
        }

        if ((cur->out.type == A_FILE || cur->out.type == APPEND_FILE) &&
                (cur->out.filename == NULL || strlen(cur->out.filename) == 0)) {
            fprintf(stderr, "sish: missing output filename for `%s'\n",
                    cur->exe->str);
            return false;
        }
    }

    return true;
}

