#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <readline/readline.h>
#include <ncurses.h>
#include <term.h>

size_t MAX_LINES = 100000;

int abort_ltz(int res) {
    if (res < 0) {
        perror(NULL);
        exit(res);
    }
    return res;
}

#define abort_null(ptr)         \
    ({                          \
        typeof(ptr) _ptr = ptr; \
        if (!_ptr)              \
        {                       \
            perror(NULL);       \
            exit(-1);           \
        }                       \
        _ptr;                   \
    })

char cmdbuf[100];
void termput(char *cmd) {
    putp(tgetstr(cmd, (char**)&cmdbuf));
}

void read_show_output(FILE *s, size_t *shown, size_t *total) {
    for (;;) {
        if (*total >= MAX_LINES)
            break;
        char *line = NULL;
        size_t len = 0;
        ssize_t read = getline(&line, &len, s);
        if (read < 0)
            break;
        if (*shown < LINES - 2)
        {
            fputs(line, stdout); // TODO: truncate long lines
            *shown += 1;
        }
        *total += 1;
    }
}

int read_command(const char *command, size_t *shown, size_t *total) {
    int child_stdout[2];
    int child_stderr[2];
    abort_ltz(pipe(child_stdout));
    abort_ltz(pipe(child_stderr));

    int pid = abort_ltz(fork());
    if (pid == 0) {
        // child
        close(child_stdout[0]);
        close(child_stderr[0]);
        close(1);
        abort_ltz(dup(child_stdout[1]));
        close(2);
        abort_ltz(dup(child_stderr[1]));
        execl("/bin/sh", "sh", "-c", command, NULL);
    }

    // parent
    close(child_stdout[1]);
    close(child_stderr[1]);
    FILE *c_stdout = abort_null(fdopen(child_stdout[0], "r"));
    FILE *c_stderr = abort_null(fdopen(child_stderr[0], "r"));
    *shown = *total = 0;

    read_show_output(c_stdout, shown, total);

    fclose(c_stdout);
    int status = 0;
    waitpid(pid, &status, 0);
    // Don't treat SIGPIPE as an error if we just closed the stream because of
    // too much output.
    bool errored = status != 0 &&
        (WIFEXITED(status) ||
        (*total >= MAX_LINES && WIFSIGNALED(status) && WTERMSIG(status) != SIGPIPE));
    if (errored) {
        int sig = WTERMSIG(status);
        // show the stderr instead
        for (int i = 0; i < *shown; ++i)
            termput("up");
        *shown = *total = 0;
        read_show_output(c_stderr, shown, total);
    }
    fclose(c_stderr);

    return WEXITSTATUS(status);
}

int last_status = -1;

int show_preview(const char *a, int b) {
    printf("\n");
    termput("cd");
    size_t shown = 0;
    size_t total = 0;
    last_status = read_command(rl_line_buffer, &shown, &total);
    termput("mr");
    if (last_status == 0) {
        printf("%zu%s lines, showing %zu", total, total >= MAX_LINES ? "+" : "", shown);
    } else {
        printf("error in command: %i", last_status);
    }
    termput("me");
    for (int i = 0; i < (shown+1); ++i) {
        termput("up");
    }
    rl_redisplay();
    return 0;
}

int setup(const char *a, int b) {
    int res = rl_add_defun("pipeline-preview", show_preview, '\n');
    return 0;
}

int main(int argc, char const *argv[])
{
    setupterm(NULL, 1, NULL);
    rl_startup_hook = setup;
    char *line = readline("pipeline> ");
    if (line)
        free(line);
    return 0;
}
