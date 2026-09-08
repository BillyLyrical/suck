/*
 * suck - monitor data streams and reap stagnant processes
 * 
 * Copyright 2026 Billy Lyrical
 * made with the help of Google Gemini
 * License: GPL
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <signal.h>
#include <regex.h>

// Configuration states
static int silence_timeout = 0;
static int hard_timeout    = 0;
static int quiet           = 0;
static int ring_size       = 100;
static int verbose         = 0;
static int discard_stderr  = 0;
static const char *stderr_log = NULL;
static int panic_on_stderr = 0;
static int non_interactive = 0;
static const char *ignore_pattern = NULL;

// Runtime state mechanics
static pid_t child_pid     = 0;
static regex_t preg;
static int has_regex       = 0;
static char **ring         = NULL;
static int ring_head       = 0;
static int ring_count      = 0;
static char partial_line[8192] = {0};

void usage(int status);
void ring_add(const char *line);
void ring_dump(void);
int process_stream(int fd, char *partial, size_t *p_len, int is_stderr);
void monitor_streams(int out_fd, int err_fd);
void exec_mode(char **argv_cmd);
void filter_mode(void);
void kill_child(void);
void sig_handler(int sig);

void usage(int status) {
    FILE *stream = status ? stderr : stdout;
    fprintf(stream, 
        "Usage: suck [options] [command [args...]]\n"
        "Monitor data streams and reap stagnant processes.\n\n"
        "Options:\n"
        "  -t SEC     Silence timeout. Kill if no output for SEC seconds. [0=off]\n"
        "  -T SEC     Hard timeout. Kill after SEC seconds regardless.   [0=off]\n"
        "  -q         Quiet mode. Buffer output, only show on timeout.\n"
        "  -n N       Ring buffer size for -q mode.                      [100]\n"
        "  -E         Discard command's STDERR (redirect to /dev/null).\n"
        "  -e FILE    Redirect command's STDERR to a dedicated log file.\n"
        "  -P         Fast-fail panic. Kill immediately if any data hits STDERR.\n"
        "  -I         Non-interactive mode. Redirect STDIN from /dev/null.\n"
        "  -i REGEX   Ignore pattern. Lines matching REGEX do not reset silence timer.\n"
        "  -v         Verbose status to stderr.\n\n"
        "Exit codes:\n"
        "  0    Normal completion\n"
        "  124  Hard timeout\n"
        "  125  Silence timeout\n"
        "  126  Fast-fail panic\n"
        "  127  Exec failed\n"
    );
    
    exit(status);
}

int main(int argc, char **argv) {
    int opt;
    while ((opt = getopt(argc, argv, "t:T:qn:Ee:PIi:vh")) != -1) {
        switch (opt) {
            case 't': silence_timeout = atoi(optarg); break;
            case 'T': hard_timeout    = atoi(optarg); break;
            case 'q': quiet           = 1; break;
            case 'n': ring_size       = atoi(optarg); break;
            case 'E': discard_stderr  = 1; break;
            case 'e': stderr_log      = optarg; break;
            case 'P': panic_on_stderr = 1; break;
            case 'I': non_interactive = 1; break;
            case 'i': ignore_pattern  = optarg; break;
            case 'v': verbose         = 1; break;
            case 'h': usage(0); break;
            default:  usage(1); break;
        }
    }

    if (ignore_pattern) {
        if (regcomp(&preg, ignore_pattern, REG_EXTENDED | REG_NOSUB) != 0) {
            fprintf(stderr, "suck: invalid regex pattern\n");
            exit(1);
        }
        has_regex = 1;
    }

    if (quiet) {
        ring = calloc(ring_size, sizeof(char *));
    }

    if (optind < argc) {
        exec_mode(&argv[optind]);
    } else {
        filter_mode();
    }

    if (has_regex) regfree(&preg);
    if (ring) free(ring);
    return 0;
}

void ring_add(const char *line) {
    if (ring_count < ring_size) {
        ring[(ring_head + ring_count) % ring_size] = strdup(line);
        ring_count++;
    } else {
        free(ring[ring_head]);
        ring[ring_head] = strdup(line);
        ring_head = (ring_head + 1) % ring_size;
    }
}

void ring_dump(void) {
    if (ring_count == 0 && partial_line[0] == '\0') return;
    int total_lines = ring_count + (partial_line[0] != '\0');
    fprintf(stderr, "--- last %d lines ---\n", total_lines);
    for (int i = 0; i < ring_count; i++) {
        int idx = (ring_head + i) % ring_size;
        fprintf(stderr, "%s\n", ring[idx]);
        free(ring[idx]);
    }
    if (partial_line[0] != '\0') {
        fprintf(stderr, "%s\n", partial_line);
    }
}

int process_stream(int fd, char *partial, size_t *p_len, int is_stderr) {
    char buf[8192];
    ssize_t bytes = read(fd, buf, sizeof(buf) - 1);
    if (bytes <= 0) return -1;

    // Fast path optimization for standard streaming configurations
    if (!ignore_pattern) {
        if (!quiet && !is_stderr) {
            return write(STDOUT_FILENO, buf, bytes) > 0 ? 1 : 0;
        } else if (is_stderr) {
            return write(STDERR_FILENO, buf, bytes) > 0 ? 1 : 0;
        }
    }

    // Line transformation layer for ring buffer tracking and regular expressions
    int active = 0;
    for (ssize_t i = 0; i < bytes; i++) {
        if (buf[i] == '\n') {
            partial[*p_len] = '\0';
            int ignore = 0;
            if (has_regex && regexec(&preg, partial, 0, NULL, 0) == 0) {
                ignore = 1;
            }
            if (!ignore) active = 1;

            if (is_stderr) {
                fprintf(stderr, "%s\n", partial);
            } else {
                if (quiet) {
                    ring_add(partial);
                } else {
                    printf("%s\n", partial);
                    fflush(stdout);
                }
            }
            *p_len = 0;
        } else if (*p_len < 8191) {
            partial[(*p_len)++] = buf[i];
        }
    }
    return active;
}

void monitor_streams(int out_fd, int err_fd) {
    time_t start_time  = time(NULL);
    time_t last_output = time(NULL);
    
    char out_partial[8192] = {0}; size_t out_p_len = 0;
    char err_partial[8192] = {0}; size_t err_p_len = 0;

    int out_eof = (out_fd < 0);
    int err_eof = (err_fd < 0);

    if (verbose) {
        fprintf(stderr, "suck: pid=%d (silence=%ds hard=%ds)\n", 
                child_pid ? child_pid : getpid(), silence_timeout, hard_timeout);
    }

    while (!out_eof || !err_eof) {
        time_t now = time(NULL);
        
        if (hard_timeout > 0 && (now - start_time >= hard_timeout)) {
            if (verbose) fprintf(stderr, "suck: hard timeout after %ds\n", hard_timeout);
            kill_child();
            strncpy(partial_line, out_partial, sizeof(partial_line) - 1);
            ring_dump();
            exit(124);
        }
        if (silence_timeout > 0 && (now - last_output >= silence_timeout)) {
            if (verbose) fprintf(stderr, "suck: silence timeout after %ds\n", silence_timeout);
            kill_child();
            strncpy(partial_line, out_partial, sizeof(partial_line) - 1);
            ring_dump();
            exit(125);
        }

        struct pollfd fds[2];
        int nfds = 0;
        int o_idx = -1, e_idx = -1;

        if (!out_eof) { fds[nfds].fd = out_fd; fds[nfds].events = POLLIN; o_idx = nfds++; }
        if (!err_eof) { fds[nfds].fd = err_fd; fds[nfds].events = POLLIN; e_idx = nfds++; }

        int ret = poll(fds, nfds, 1000);
        if (ret < 0) break;
        if (ret == 0) continue;

        for (int i = 0; i < nfds; i++) {
            if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                if (o_idx == i) {
                    int stat = process_stream(out_fd, out_partial, &out_p_len, 0);
                    if (stat < 0) {
                        out_eof = 1;
                        strncpy(partial_line, out_partial, sizeof(partial_line) - 1);
                    } else if (stat > 0) {
                        last_output = time(NULL);
                    }
                }
                else if (e_idx == i) {
                    if (panic_on_stderr) {
                        char buf[1024];
                        ssize_t b = read(err_fd, buf, sizeof(buf) - 1);
                        if (b > 0) {
                            buf[b] = '\0';
                            if (verbose) fprintf(stderr, "suck: panic triggered by data on stderr\n");
                            write(STDERR_FILENO, buf, b);
                        }
                        kill_child();
                        exit(126);
                    }
                    int stat = process_stream(err_fd, err_partial, &err_p_len, 1);
                    if (stat < 0) {
                        err_eof = 1;
                    } else if (stat > 0) {
                        last_output = time(NULL);
                    }
                }
            }
        }
    }
}

void exec_mode(char **argv_cmd) {
    int out_pipe[2], err_pipe[2];
    if (pipe(out_pipe) < 0 || pipe(err_pipe) < 0) {
        perror("suck: pipe");
        exit(1);
    }

    child_pid = fork();
    if (child_pid < 0) {
        perror("suck: fork");
        exit(1);
    }

    if (child_pid == 0) {
        close(out_pipe[0]);
        close(err_pipe[0]);

        if (non_interactive) {
            int dev_null = open("/dev/null", O_RDONLY);
            if (dev_null >= 0) {
                dup2(dev_null, STDIN_FILENO);
                close(dev_null);
            }
        }

        dup2(out_pipe[1], STDOUT_FILENO);
        close(out_pipe[1]);

        if (stderr_log) {
            int log_fd = open(stderr_log, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (log_fd >= 0) {
                dup2(log_fd, STDERR_FILENO);
                close(log_fd);
            }
            close(err_pipe[1]);
        } else if (discard_stderr) {
            int dev_null = open("/dev/null", O_WRONLY);
            if (dev_null >= 0) {
                dup2(dev_null, STDERR_FILENO);
                close(dev_null);
            }
            close(err_pipe[1]);
        } else {
            dup2(err_pipe[1], STDERR_FILENO);
            close(err_pipe[1]);
        }

        execvp(argv_cmd[0], argv_cmd);
        fprintf(stderr, "suck: exec %s failed\n", argv_cmd[0]);
        exit(127);
    }

    close(out_pipe[1]);
    close(err_pipe[1]);

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    monitor_streams(out_pipe[0], err_pipe[0]);

    close(out_pipe[0]);
    close(err_pipe[0]);

    int status;
    waitpid(child_pid, &status, 0);
    if (WIFEXITED(status)) {
        exit(WEXITSTATUS(status));
    }
    exit(1);
}

void filter_mode(void) {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    monitor_streams(STDIN_FILENO, -1);
}

void kill_child(void) {
    if (child_pid <= 0) return;
    kill(child_pid, SIGTERM);
    int status;
    for (int i = 0; i < 50; i++) {
        if (waitpid(child_pid, &status, WNOHANG) > 0) return;
        usleep(100000);
    }
    kill(child_pid, SIGKILL);
    waitpid(child_pid, &status, 0);
}

void sig_handler(int sig) {
    kill_child();
    exit(130);
}

// thankyouverymuchgoodnight
