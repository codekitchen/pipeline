#define _XOPEN_SOURCE // required by linux

#include "config.h"
#include <getopt.h>
#include <locale.h>
#include <math.h>
#include <ncurses.h>
#include <readline/readline.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <term.h>
#include <unistd.h>
#include <wchar.h>

#define PROGRAM_NAME "pipeline"
char *program_name = PROGRAM_NAME;

bool truncate_lines = false;

int abort_ltz(int res) {
    if (res < 0) {
        perror(NULL);
        exit(res);
    }
    return res;
}

int abort_nz(int res) {
    if (res != 0) {
        perror(NULL);
        exit(res);
    }
    return res;
}

#define abort_null(ptr)                                                        \
    ({                                                                         \
        typeof(ptr) _ptr = ptr;                                                \
        if (!_ptr) {                                                           \
            perror(NULL);                                                      \
            exit(-1);                                                          \
        }                                                                      \
        _ptr;                                                                  \
    })

// valid terminfo short commands are available at `man 5 terminfo`
// http://man7.org/linux/man-pages/man5/terminfo.5.html
char cmdbuf[100];
void termput0(char *cmd) {
    char *b = cmdbuf;
    putp(tgetstr(cmd, &b));
}
void termput1(char *cmd, int arg1) {
    char *b = cmdbuf;
    char *o = tgetstr(cmd, &b);
    putp(tgoto(o, 0, arg1));
}

void str_append(wchar_t **line, ssize_t *len, size_t *cap, wchar_t c) {
    if (*len == *cap) {
        *cap = *cap < 255 ? 255 : *cap * 2;
        *line = abort_null(realloc(*line, *cap * sizeof(wchar_t)));
    }
    (*line)[(*len)++] = c;
}

// Follows the same general pattern as libc's getline(), but reads into a
// wchar_t string and throws away the rest of the line once max_display_len is
// reached.
// Note this is *display* length, not character length. A single unicode character
// doesn't always display as a single visual character on the terminal.
// Returns the calculated display_length, or -1 on EOF/error.
ssize_t read_line(FILE *s, wchar_t **line, size_t *cap, size_t max_display_len) {
    ssize_t display_len = 0;
    ssize_t len = 0;
    for (;;) {
        wchar_t c = fgetwc(s);
        if (c == WEOF) {
            abort_nz(ferror(s));
            if (len == 0)
                return -1;
            break;
        }
        if (c == L'\n') {
            // don't append the newline
            break;
        }
        int c_len = wcwidth(c);
        if ((display_len + c_len) > max_display_len) {
            continue;
        }
        str_append(line, &len, cap, c);
        display_len += c_len;
    }
    str_append(line, &len, cap, 0);
    return display_len;
}

// Read the file stream and show the first page of output.
void read_show_output(FILE *s, size_t *count, size_t *shown, size_t *total) {
    termput0("cd");
    wchar_t *line = NULL;
    size_t cap = 0;
    ssize_t display_len;
    while ((display_len = read_line(s, &line, &cap, truncate_lines ? COLS : SIZE_MAX)) >= 0) {
        *total += 1;
        // if we haven't filled the screen yet, display this line.
        if (*shown < LINES - 2) {
            printf("%ls\n", line);
            *count += 1;
            *shown += (int)ceil((double)display_len / COLS);
        }
    }
    free(line);
}

// Fork the child process, run the given command in shell. Prints the first page
// of stdout on success, or stderr on failure.
int read_command(const char *command, size_t *count, size_t *shown, size_t *total) {
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
    *count = *shown = *total = 0;

    read_show_output(c_stdout, count, shown, total);

    fclose(c_stdout);
    int status = 0;
    waitpid(pid, &status, 0);
    if (status != 0) {
        // show the stderr instead
        termput1("UP", (*shown) - 1);
        *shown = *total = 0;
        read_show_output(c_stderr, count, shown, total);
    }
    fclose(c_stderr);

    return WEXITSTATUS(status);
}

// Run the current command string and display the first page of results.
// Args are passed in by readline and ignored.
int show_preview(const char *a, int b) {
    printf("\n");
    size_t count = 0;
    size_t shown = 0;
    size_t total = 0;
    int last_status = read_command(rl_line_buffer, &count, &shown, &total);
    termput0("mr");
    int statsize = 0;
    if (last_status == 0) {
        statsize = printf(" %zu lines, showing %zu ", total, count);
    } else {
        statsize = printf(" error in command: %i ", last_status);
    }
    printf("%*s", COLS - statsize, "");
    termput0("me");
    termput1("UP", shown + 1);
    // moving the cursor fully left, necessary for libreadline but not libedit
    termput1("LE", 9999);
    rl_forced_update_display();
    return 0;
}

// Called on readline startup to inject the newline hook.
int setup() {
    // libedit wants '\n' but libreadline wants '\r'.
    //
    // this ifdef is a hacky way to detect whether we're using readline or
    // libedit, I'd love to find something cleaner.
    //
    // also note : rl_bind_key seems to be totally broken with libedit, at least
    // on MacOS. I have to use rl_add_defun to see any effect in that
    // environment.
#ifdef RL_STATE_NONE
    abort_nz(rl_add_defun("pipeline-preview", (rl_command_func_t *)show_preview, '\r'));
#else
    abort_nz(rl_add_defun("pipeline-preview", (Function *)show_preview, '\n'));
#endif
    return 0;
}

static struct option const long_options[] = {
    {"truncate", no_argument, NULL, 't'},
    {"help", no_argument, NULL, 'h'},
    {"version", no_argument, NULL, 'v'},
    {NULL, 0, NULL, 0}};

void usage(int status) {
    printf("Usage: %s [OPTIONS]\n", program_name);
    printf("\n");
    printf("    -t, --truncate    Truncate long lines rather than  wrapping.\n");
    exit(status);
}

void version() {
    printf("%s\n", PACKAGE_STRING);
    exit(EXIT_SUCCESS);
}

int main(int argc, char *const *argv) {
    if (argc)
        program_name = argv[0];
    char *locale = setlocale(LC_ALL, "");
    int c;
    while ((c = getopt_long(argc, argv, "thv", long_options, NULL)) != -1) {
        switch (c) {
            case 't':
                truncate_lines = true;
                break;
            case 'h':
                usage(EXIT_SUCCESS);
                break;
            case 'v':
                version();
                break;
            default:
                usage(EXIT_FAILURE);
        }
    }

    setupterm(NULL, 1, NULL);
    rl_startup_hook = setup;
    char *line = readline("pipeline> ");
    free(line);
    return 0;
}
