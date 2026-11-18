#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <spawn.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "proc.h"

extern char **environ;

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int zos_run(const char *const argv[], char *out, size_t cap, int timeout_ms)
{
    int p[2];
    if (pipe2(p, O_CLOEXEC) != 0) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, p[1], 1);
    posix_spawn_file_actions_adddup2(&fa, p[1], 2);
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], &fa, NULL, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(p[1]);
    if (rc != 0) {
        close(p[0]);
        if (out && cap) snprintf(out, cap, "%s: %s", argv[0], strerror(rc));
        return -1;
    }
    size_t len = 0;
    long deadline = now_ms() + (timeout_ms > 0 ? timeout_ms : 30000);
    char sink[512];
    for (;;) {
        long left = deadline - now_ms();
        if (left <= 0) break;
        struct pollfd pf = {.fd = p[0], .events = POLLIN};
        if (poll(&pf, 1, (int)left) <= 0) continue;
        ssize_t r;
        if (out && len + 1 < cap) r = read(p[0], out + len, cap - 1 - len);
        else r = read(p[0], sink, sizeof sink);
        if (r <= 0) break;
        if (out && len + 1 < cap) len += (size_t)r;
    }
    close(p[0]);
    if (out && cap) out[len < cap ? len : cap - 1] = 0;
    int status = 0;
    long end = now_ms() + 2000;
    while (waitpid(pid, &status, WNOHANG) == 0) {
        if (now_ms() > deadline || now_ms() > end) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return -1;
        }
        usleep(10000);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void zos_run_bg(const char *const argv[])
{
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    posix_spawnp(&pid, argv[0], &fa, NULL, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
}
