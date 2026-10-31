/*
 * myshell.c - A small Unix shell.
 *
 * Features: command execution (fork/exec), pipes (a | b | c),
 * redirection (<, >, >>), background jobs (&), built-ins (cd, exit, help),
 * Ctrl+C handling (kills the foreground job, not the shell),
 * and zombie reaping for background jobs.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <limits.h>

#define MAX_ARGS 64
#define MAX_CMDS 16

struct command {
    char *argv[MAX_ARGS];
    int   argc;
    char *infile;
    char *outfile;
    int   append;           /* 1 for >> */
};

static void on_sigint(int sig)
{
    (void)sig;
    ssize_t r = write(STDOUT_FILENO, "\n", 1);   /* async-signal-safe */
    (void)r;
}

/* Reap finished background jobs so they don't stay zombies. */
static void reap_background(void)
{
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
        printf("[done] pid %d\n", pid);
}

/*
 * Split a line into commands separated by '|'.
 * Returns the number of commands, or -1 on a syntax error.
 * Sets *background if the line ends with '&'.
 */
static int parse_line(char *line, struct command *cmds, int *background)
{
    int n = 0;
    char *save = NULL;
    *background = 0;
    memset(&cmds[0], 0, sizeof(cmds[0]));

    for (char *tok = strtok_r(line, " \t\r\n", &save); tok;
         tok = strtok_r(NULL, " \t\r\n", &save)) {

        struct command *c = &cmds[n];

        if (strcmp(tok, "|") == 0) {
            if (c->argc == 0) { fprintf(stderr, "syntax error near '|'\n"); return -1; }
            if (++n >= MAX_CMDS) { fprintf(stderr, "too many pipes\n"); return -1; }
            memset(&cmds[n], 0, sizeof(cmds[n]));
        } else if (strcmp(tok, "<") == 0 || strcmp(tok, ">") == 0 ||
                   strcmp(tok, ">>") == 0) {
            char *file = strtok_r(NULL, " \t\r\n", &save);
            if (!file) { fprintf(stderr, "syntax error: missing file after '%s'\n", tok); return -1; }
            if (tok[0] == '<') c->infile = file;
            else { c->outfile = file; c->append = (tok[1] == '>'); }
        } else if (strcmp(tok, "&") == 0) {
            *background = 1;
        } else {
            if (c->argc >= MAX_ARGS - 1) { fprintf(stderr, "too many arguments\n"); return -1; }
            c->argv[c->argc++] = tok;
            c->argv[c->argc] = NULL;
        }
    }

    if (cmds[n].argc == 0)
        return n == 0 ? 0 : (fprintf(stderr, "syntax error: missing command after '|'\n"), -1);
    return n + 1;
}

/* Returns 1 if handled as a built-in. */
static int run_builtin(struct command *c)
{
    if (strcmp(c->argv[0], "exit") == 0) {
        exit(c->argc > 1 ? atoi(c->argv[1]) : 0);
    }
    if (strcmp(c->argv[0], "cd") == 0) {
        const char *dir = c->argc > 1 ? c->argv[1] : getenv("HOME");
        if (!dir || chdir(dir) != 0)
            perror("cd");
        return 1;
    }
    if (strcmp(c->argv[0], "help") == 0) {
        puts("myshell built-ins: cd [dir], exit [code], help\n"
             "Supports: pipes (|), redirection (< > >>), background (&)");
        return 1;
    }
    return 0;
}

static void exec_child(struct command *c, int in_fd, int out_fd,
                       int *pipes, int npipe_fds, int background)
{
    /* restore default signal behaviour so Ctrl+C kills this job */
    signal(SIGINT, SIG_DFL);
    if (background)
        setpgid(0, 0);      /* own process group: Ctrl+C won't hit it */

    if (in_fd != STDIN_FILENO)   dup2(in_fd, STDIN_FILENO);
    if (out_fd != STDOUT_FILENO) dup2(out_fd, STDOUT_FILENO);
    for (int i = 0; i < npipe_fds; i++)
        close(pipes[i]);

    if (c->infile) {
        int fd = open(c->infile, O_RDONLY);
        if (fd < 0) { perror(c->infile); _exit(1); }
        dup2(fd, STDIN_FILENO);
        close(fd);
    }
    if (c->outfile) {
        int flags = O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC);
        int fd = open(c->outfile, flags, 0644);
        if (fd < 0) { perror(c->outfile); _exit(1); }
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }

    execvp(c->argv[0], c->argv);
    fprintf(stderr, "%s: %s\n", c->argv[0], strerror(errno));
    _exit(127);
}

static void run_pipeline(struct command *cmds, int n, int background)
{
    int pipes[2 * (MAX_CMDS - 1)];
    pid_t pids[MAX_CMDS];

    for (int i = 0; i < n - 1; i++) {
        if (pipe(&pipes[2 * i]) < 0) { perror("pipe"); return; }
    }

    for (int i = 0; i < n; i++) {
        int in_fd  = (i == 0)     ? STDIN_FILENO  : pipes[2 * (i - 1)];
        int out_fd = (i == n - 1) ? STDOUT_FILENO : pipes[2 * i + 1];

        pids[i] = fork();
        if (pids[i] < 0) { perror("fork"); break; }
        if (pids[i] == 0)
            exec_child(&cmds[i], in_fd, out_fd, pipes, 2 * (n - 1), background);
    }

    /* parent must close all pipe ends or readers never see EOF */
    for (int i = 0; i < 2 * (n - 1); i++)
        close(pipes[i]);

    if (background) {
        printf("[bg] pid %d\n", pids[n - 1]);
        return;
    }
    for (int i = 0; i < n; i++) {
        if (pids[i] > 0) {
            int status;
            while (waitpid(pids[i], &status, 0) < 0 && errno == EINTR)
                ;
        }
    }
}

int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;          /* no SA_RESTART: fgets gets interrupted */
    sigaction(SIGINT, &sa, NULL);

    char line[4096];
    struct command cmds[MAX_CMDS];

    for (;;) {
        reap_background();

        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "?");
        printf("myshell:%s$ ", cwd);
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            if (errno == EINTR) { clearerr(stdin); continue; }   /* Ctrl+C */
            printf("\nexit\n");                                  /* Ctrl+D */
            break;
        }

        int bg;
        int n = parse_line(line, cmds, &bg);
        if (n <= 0) continue;

        if (n == 1 && run_builtin(&cmds[0]))
            continue;

        run_pipeline(cmds, n, bg);
    }
    return 0;
}
