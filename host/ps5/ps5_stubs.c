/* (PS5) What host/ and gpu/vk call that the console does not have: the parts of upstream's SDL layer that
   host/ps5/hos_sdl_ps5.c does not stand in for, the control bridge (host/control.c is not built: it is a local socket
   for a desktop's tools), and a few calls of Linux's C library. Each answers "not there", which every caller takes. */
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

/* ---- host/hos_sdl.h: no keyboard, mouse, clipboard or Steam on the console ---- */
int hos_sdl_hint_override(const char *name, const char *value) { (void)name, (void)value; return 0; }
void hos_sdl_mouse_position(float *x, float *y) { if (x) *x = 0; if (y) *y = 0; }
void hos_sdl_text_input(void *window, int on) { (void)window, (void)on; }
char *hos_sdl_clipboard(void) { return NULL; }
void hos_sdl_free(void *p) { (void)p; }
void hos_sdl_pad_log_connection(void *pad, int motion, int steam) { (void)pad, (void)motion, (void)steam; }
int hos_sdl_knows_steam_pads(void) { return 0; }
uint16_t hos_sdl_pad_vendor(void *pad) { (void)pad; return 0; }
uint64_t hos_sdl_pad_steam_handle(void *pad) { (void)pad; return 0; }

/* ---- host/control.h: the bridge is off ---- */
volatile int hos_control_on;
void hos_control_start(const char *version) { (void)version; }
int hos_control_input(uint64_t *buttons, int32_t stick[4]) { (void)buttons, (void)stick; return 0; }
void hos_control_log(const char *line) { (void)line; }

/* ---- Linux's C library ---- */
struct passwd *getpwuid(uid_t uid) { (void)uid; return NULL; }   /* (no user database: callers fall back to the game folder) */
int posix_fadvise(int fd, off_t offset, off_t len, int advice) { (void)fd, (void)offset, (void)len, (void)advice; return 0; }
int syscall(int number, ...) { (void)number; errno = ENOSYS; return -1; }   /* (pidfd_open, close_range: the crash dialog's, which starts no program here) */
int pipe2(int fds[2], int flags) {
    if (pipe(fds)) return -1;
    for (int i = 0; i < 2; i++) {
        if (flags & O_CLOEXEC) fcntl(fds[i], F_SETFD, FD_CLOEXEC);
        if (flags & O_NONBLOCK) fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL) | O_NONBLOCK);
    }
    return 0;
}
/* (host/hos_report_run.c: a run's random name) the kernel's random device; -1 where it does not open, which the caller
   takes as "no random bytes" and names the run by the clock */
ssize_t getrandom(void *buf, size_t n, unsigned flags) {
    (void)flags;
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) { errno = ENOSYS; return -1; }
    const size_t got = fread(buf, 1, n, f);
    fclose(f);
    return (ssize_t)got;
}

/* ---- the end ---- */
/* _Exit and _exit are a crash report on the console, and what the title logged last is still in the platform layer's pipe
   to klog when they run: a GPU stall's report (gpu/vk/vk_replay.cpp replay_fatal, which ends with _Exit) lost all but its
   first lines (2026-11-15). The pipe is given half a second, then the shell closes the title (host/ps5/win32_ps5.c). */
__attribute__((noreturn)) void hos_ps5_exit(int code);
__attribute__((noreturn)) void _Exit(int code) {
    fflush(NULL);
    usleep(500000);
    hos_ps5_exit(code);
}
__attribute__((noreturn)) void _exit(int code) { _Exit(code); }
