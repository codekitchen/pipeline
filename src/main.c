#define _XOPEN_SOURCE // required by linux

#include "config.h"
#include <getopt.h>
#include <locale.h>
#include <math.h>
#include <ncurses.h>
#include <readline/readline.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <term.h>
#include <unistd.h>
#include <wchar.h>

// this ifdef is a hacky way to detect whether we're using readline or
// libedit, I'd love to find something cleaner.
#ifdef RL_STATE_NONE
#define USING_READLINE 1
#endif

#define PROGRAM_NAME "pipeline"
char *program_name = PROGRAM_NAME;
const char *prompt = "pipeline> ";
const size_t prompt_width = 10;

bool truncate_lines = false;
#define DEFAULT_SHELL "/bin/sh"
const char *shell = DEFAULT_SHELL;
const char *KNOWN_SHELLS[] = {"sh", "bash", "zsh", "fish", NULL};

int s_lines, s_cols;

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

void cleanup_abort(int ecode) {
    printf("\n");
    termput0("cd");
    rl_deprep_terminal();
    exit(ecode);
}

int abort_ltz(int res) {
    if (res < 0) {
        perror(NULL);
        cleanup_abort(res);
    }
    return res;
}

int abort_nz(int res) {
    if (res != 0) {
        perror(NULL);
        cleanup_abort(res);
    }
    return res;
}

#define abort_null(ptr)                                                        \
    ({                                                                         \
        typeof(ptr) _ptr = ptr;                                                \
        if (!_ptr) {                                                           \
            perror(NULL);                                                      \
            cleanup_abort(-1);                                                 \
        }                                                                      \
        _ptr;                                                                  \
    })

// Read a single line from s and print it out. Once max_display_len is reached,
// keep scanning for the rest of the line but don't print anymore.
ssize_t read_line(FILE *s, size_t max_display_len) {
    ssize_t display_len = 0; // number of printed columns
    ssize_t len = 0; // number of chars read
    for (;;) {
        wchar_t c = fgetwc(s);
        if (c == WEOF) {
            abort_nz(ferror(s));
            if (len == 0)
                return -1;
            break;
        }
        ++len;
        if (c == L'\n')
            break;
        int c_len = wcwidth(c);
        if ((display_len + c_len) > max_display_len)
            continue;
        putwchar(c);
        display_len += c_len;
    }
    if (max_display_len > 0)
        putwchar(L'\n');
    return display_len;
}

// Read the file stream and show the first page of output.
// `count` is the number of logical lines shown on screen.
// `shown` is the number of actual screen lines printed to,
//   which can be more than `count` when long logical lines wrap to multiple lines on-screen.
// `total` is the total number of logical lines in the output.
// (where "logical lines" means lines separated by \n chars)
void read_show_output(size_t max_to_show, FILE *s, size_t *count, size_t *shown, size_t *total) {
    termput0("cd");
    int lines_left = max_to_show;
    for (;;) {
        size_t max_display_len;
        if (truncate_lines)
            max_display_len = lines_left > 0 ? s_cols : 0;
        else
            max_display_len = s_cols * lines_left;
        ssize_t display_len = read_line(s, max_display_len);
        if (display_len < 0)
            break;
        *total += 1;
        if (lines_left > 0) {
            *count += 1;
            int nlines = 1;
            // If we wrapped, find the actual number of screen lines printed to.
            if (display_len > s_cols)
                nlines = (int)ceil((double)display_len / s_cols);
            lines_left -= nlines;
            *shown += nlines;
        }
    }
}

// Fork the child process, run the given command in shell. Prints the first page
// of stdout on success, or stderr on failure.
int read_command(size_t max_to_show, const char *command, size_t *count, size_t *shown, size_t *total) {
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
        execl(shell, shell, "-c", command, NULL);
    }

    // parent
    close(child_stdout[1]);
    close(child_stderr[1]);
    FILE *c_stdout = abort_null(fdopen(child_stdout[0], "r"));
    FILE *c_stderr = abort_null(fdopen(child_stderr[0], "r"));
    *count = *shown = *total = 0;

    read_show_output(max_to_show, c_stdout, count, shown, total);

    fclose(c_stdout);
    int status = 0;
    waitpid(pid, &status, 0);
    if (status != 0) {
        // show the stderr instead
        if ((*shown) > 0)
            termput1("UP", *shown);
        *count = *shown = *total = 0;
        read_show_output(max_to_show, c_stderr, count, shown, total);
    }
    fclose(c_stderr);

    return WEXITSTATUS(status);
}

// Run the current command string and display the first page of results.
// Args are passed in by readline and ignored.
int show_preview(const char *_a, int _b) {
    // Get the current window dimensions
    struct winsize sizes;
    abort_ltz(ioctl(0, TIOCGWINSZ, &sizes));
    s_lines = sizes.ws_row;
    s_cols = sizes.ws_col;

    // Add 1 to the width for the cursor on the end, it makes the output
    // cleaner when the command is exactly at the screen width.
    size_t command_width = prompt_width + rl_end + 1;
    size_t command_lines = (size_t)ceil((double)command_width / s_cols);

    // Move the cursor to the beginning of the command line
    // so we know where we are.
    size_t cursor_pos = prompt_width + rl_point;
    while (cursor_pos >= s_cols) {
        termput0("up");
        cursor_pos -= s_cols;
    }
    if (cursor_pos > 0)
        termput1("LE", cursor_pos);

    // Now move the cursor one line below the command line.
    // Printing newlines rather than using termput("DO") here because
    // we might be at the bottom of the window, and we can't just
    // move down from there.
    for (int i = 0; i < command_lines; ++i)
        printf("\n");

    // Display the output
    size_t max_to_show = s_lines - (command_lines + 1);
    size_t count = 0;
    size_t shown = 0;
    size_t total = 0;
    int last_status = read_command(max_to_show, rl_line_buffer, &count, &shown, &total);

    // Display the status line in reversed video
    termput0("mr");
    int statsize = 0;
    if (last_status == 0) {
        statsize = printf(" %zu lines, showing %zu ", total, count);
    } else {
        statsize = printf(" error in command: %i ", last_status);
    }
    printf("%*s", s_cols - statsize, "");
    termput0("me");
    termput1("LE", s_cols);

    // Now move the cursor back to the beginning of the command line,
    // and tell readline to redraw.
    termput1("UP", shown + command_lines);
    rl_forced_update_display();

    return 0;
}

#ifdef USING_READLINE
void bind_by_keymap_name(const char* name) {
    Keymap keymap = rl_get_keymap_by_name(name);
    if (keymap) {
        abort_nz(rl_bind_key_in_map('\r', (rl_command_func_t *)show_preview, keymap));
    }
}
#endif

// Called on readline startup to inject the newline hook.
int setup() {
    // libedit wants '\n' but libreadline wants '\r'.
    //
    // also note : rl_bind_key seems to be totally broken with libedit, at least
    // on MacOS. I have to use rl_add_defun to see any effect in that
    // environment.
#ifdef USING_READLINE
    abort_nz(rl_add_defun("pipeline-preview", (rl_command_func_t *)show_preview, '\r'));
    bind_by_keymap_name("emacs");
    bind_by_keymap_name("vi-insert");
    bind_by_keymap_name("vi-command");
#else
    abort_nz(rl_add_defun("pipeline-preview", (Function *)show_preview, '\n'));
#endif
    return 0;
}

void cleanup(int sig) {
    cleanup_abort(0);
}

static struct option const long_options[] = {
    {"truncate", no_argument, NULL, 't'},
    {"shell", required_argument, NULL, 's'},
    {"help", no_argument, NULL, 'h'},
    {"version", no_argument, NULL, 'v'},
    {NULL, 0, NULL, 0}};

void usage(int status) {
    printf("Usage: %s [OPTIONS]\n", program_name);
    printf("\n");
    printf("    -t, --truncate     Truncate long lines rather than wrapping.\n");
    printf("    -s, --shell=SHELL  Use the shell at the full path specified,\n");
    printf("                       rather than reading the $SHELL env var.\n");
    exit(status);
}

void version() {
    printf("%s\n", PACKAGE_STRING);
    exit(EXIT_SUCCESS);
}

// Attempt to detect the default shell using the SHELL environment variable.
// If this doesn't work, fallback to the default of /bin/sh.
// Note that SHELL isn't always accurate, for instance if you run a different
// sub-shell from within your default shell, SHELL will still be your
// default shell.
// But we're going with this for now.
void detect_shell() {
    const char *shellvar = getenv("PIPELINE_SHELL");
    if (!shellvar)
        shellvar = getenv("SHELL");
    if (shellvar)
        shell = shellvar;
}

// We have a whitelist of shells known to work with the -c option,
// if the shell isn't in the whitelist we fallback to /bin/sh.
void validate_shell() {
    const char *basename = strrchr(shell, '/');
    if (basename) {
        for (const char **known = KNOWN_SHELLS; *known; known++) {
            if (strcmp(*known, basename+1) == 0)
                return;
        }
    }
    printf("Unknown shell '%s', falling back to '%s'\n", shell, DEFAULT_SHELL);
    shell = DEFAULT_SHELL;
}

int main(int argc, char *const *argv) {
    if (argc)
        program_name = argv[0];
    setlocale(LC_ALL, "");
    detect_shell();
    int c;
    while ((c = getopt_long(argc, argv, "ts:hv", long_options, NULL)) != -1) {
        switch (c) {
        case 't':
            truncate_lines = true;
            break;
        case 's':
            shell = optarg;
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

    validate_shell();
    setupterm(NULL, 1, NULL);
    rl_startup_hook = setup;
    signal(SIGINT, cleanup);
    char *line = readline(prompt);
    // as far as I'm aware, we'll never reach this point because
    // we always exit with Ctrl-C.
    free(line);
    return 0;
}
