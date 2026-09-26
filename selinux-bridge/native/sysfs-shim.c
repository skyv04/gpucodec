/*
 * sysfs-shim.c -- give /sys/class/video4linux a readable entry for the
 *                 bridge camera.
 *
 * WHY THIS EXISTS
 *
 * v4l2-shim.c answers for /dev/video0, which is enough for anything that
 * opens the device node directly (ffmpeg, VLC, OpenCV). It is not enough for
 * the libraries that enumerate devices first -- libudev, GStreamer's
 * v4l2src, and every Chromium-derived browser -- because those do not look
 * in /dev at all. They walk /sys/class/video4linux, read `name` and `dev`
 * out of each entry, and only then open the node they were told about. With
 * no sysfs entry the device list is empty and the camera does not exist, no
 * matter how well /dev/video0 behaves.
 *
 * Inside this container /sys is traversable but not listable: the directories
 * carry x without r, so opendir() fails with EACCES while open() of a known
 * path underneath still works. That rules out "read the real sysfs" and it
 * also rules out mounting anything, since the container has no privileges.
 *
 * THE OVERLAY RULE
 *
 * A path under /sys is redirected to the synthetic tree only when the
 * synthetic tree actually has something at that path; otherwise the real
 * path is used unchanged. Redirecting /sys wholesale would be wrong and not
 * merely incomplete -- glibc itself reads /sys/devices/system/cpu/online to
 * size its per-CPU caches, and pthread reads the CPU topology, so a blanket
 * redirect breaks the C library before any application code runs.
 *
 * Set BRIDGE_SYSFS_DISABLE=1 to turn the overlay off without relinking, and
 * BRIDGE_SYSFS_ROOT to point it at a different tree. `hw-enable install`
 * generates the default tree under ~/.cache/selinux-bridge/sysfs.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdarg.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#ifndef BRIDGE_SYSFS_ROOT_DEFAULT
#define BRIDGE_SYSFS_ROOT_DEFAULT ""
#endif

static char root[512];
static size_t root_len;
static int enabled;

__attribute__((constructor))
static void sysov_init(void)
{
    const char *d = getenv("BRIDGE_SYSFS_DISABLE");
    if (d && *d && strcmp(d, "0") != 0) return;

    /* getenv() returns a pointer into the environment block, and Chromium
     * rewrites that block in place after the constructor has run. Copying the
     * string is the difference between a path and a dangling pointer. */
    const char *r = getenv("BRIDGE_SYSFS_ROOT");
    if (!r || !*r) r = BRIDGE_SYSFS_ROOT_DEFAULT;
    if (!r || !*r) {
        const char *home = getenv("HOME");
        if (!home || !*home) return;
        snprintf(root, sizeof root, "%s/.cache/selinux-bridge/sysfs", home);
    } else {
        snprintf(root, sizeof root, "%s", r);
    }
    root_len = strlen(root);
    while (root_len > 1 && root[root_len - 1] == '/') root[--root_len] = 0;

    struct stat st;
    if (syscall(SYS_newfstatat, AT_FDCWD, root, &st, 0) != 0) return;
    enabled = 1;
}

/*
 * Rotating scratch buffers: a single one would be corrupted by any call that
 * rewrites two paths, and by a signal handler that ran between the rewrite
 * and its use.
 */
/* Separate helper for the *at() family: those declare their path argument
 * nonnull, so testing it for NULL at the call site draws a warning even
 * though callers in the wild do pass NULL with AT_EMPTY_PATH. */
static const char *Rabs(const char *p);

#define NBUF 4
static __thread char pbuf[NBUF][1024];
static __thread unsigned pnext;

static const char *R(const char *p)
{
    if (!enabled || !p) return p;
    if (strncmp(p, "/sys", 4) != 0) return p;
    if (p[4] && p[4] != '/') return p;                 /* /sysfoo is not /sys */

    char *b = pbuf[pnext++ % NBUF];
    int n = snprintf(b, sizeof pbuf[0], "%s%s", root, p + 4);
    if (n <= 0 || (size_t)n >= sizeof pbuf[0]) return p;

    /* Raw syscall, so that testing for the overlay entry cannot re-enter this
     * shim through the stat wrappers below. */
    struct stat st;
    if (syscall(SYS_newfstatat, AT_FDCWD, b, &st, AT_SYMLINK_NOFOLLOW) == 0)
        return b;
    return p;
}

static const char *Rabs(const char *p)
{
    if (p == NULL || p[0] != '/') return p;
    return R(p);
}

int open(const char *p, int f, ...)
{
    static int (*r)(const char *, int, ...);
    if (!r) r = dlsym(RTLD_NEXT, "open");
    mode_t m = 0;
    if (f & O_CREAT) { va_list a; va_start(a, f); m = va_arg(a, mode_t); va_end(a); }
    return r(R(p), f, m);
}

int open64(const char *p, int f, ...)
{
    static int (*r)(const char *, int, ...);
    if (!r) r = dlsym(RTLD_NEXT, "open64");
    mode_t m = 0;
    if (f & O_CREAT) { va_list a; va_start(a, f); m = va_arg(a, mode_t); va_end(a); }
    return r(R(p), f, m);
}

int openat(int d, const char *p, int f, ...)
{
    static int (*r)(int, const char *, int, ...);
    if (!r) r = dlsym(RTLD_NEXT, "openat");
    mode_t m = 0;
    if (f & O_CREAT) { va_list a; va_start(a, f); m = va_arg(a, mode_t); va_end(a); }
    /* Only absolute paths can be rewritten; a relative path is interpreted
     * against dirfd, which may already be inside the overlay. */
    return r(d, Rabs(p), f, m);
}

int openat64(int d, const char *p, int f, ...)
{
    static int (*r)(int, const char *, int, ...);
    if (!r) r = dlsym(RTLD_NEXT, "openat64");
    mode_t m = 0;
    if (f & O_CREAT) { va_list a; va_start(a, f); m = va_arg(a, mode_t); va_end(a); }
    return r(d, Rabs(p), f, m);
}

DIR *opendir(const char *p)
{
    static DIR *(*r)(const char *);
    if (!r) r = dlsym(RTLD_NEXT, "opendir");
    return r(R(p));
}

int stat(const char *p, struct stat *s)
{
    static int (*r)(const char *, struct stat *);
    if (!r) r = dlsym(RTLD_NEXT, "stat");
    return r(R(p), s);
}

int stat64(const char *p, struct stat64 *s)
{
    static int (*r)(const char *, struct stat64 *);
    if (!r) r = dlsym(RTLD_NEXT, "stat64");
    return r(R(p), s);
}

int lstat(const char *p, struct stat *s)
{
    static int (*r)(const char *, struct stat *);
    if (!r) r = dlsym(RTLD_NEXT, "lstat");
    return r(R(p), s);
}

int lstat64(const char *p, struct stat64 *s)
{
    static int (*r)(const char *, struct stat64 *);
    if (!r) r = dlsym(RTLD_NEXT, "lstat64");
    return r(R(p), s);
}

int fstatat(int d, const char *p, struct stat *s, int f)
{
    static int (*r)(int, const char *, struct stat *, int);
    if (!r) r = dlsym(RTLD_NEXT, "fstatat");
    return r(d, Rabs(p), s, f);
}

int fstatat64(int d, const char *p, struct stat64 *s, int f)
{
    static int (*r)(int, const char *, struct stat64 *, int);
    if (!r) r = dlsym(RTLD_NEXT, "fstatat64");
    return r(d, Rabs(p), s, f);
}

int statx(int d, const char *p, int f, unsigned m, struct statx *s)
{
    static int (*r)(int, const char *, int, unsigned, struct statx *);
    if (!r) r = dlsym(RTLD_NEXT, "statx");
    return r(d, Rabs(p), f, m, s);
}

int access(const char *p, int m)
{
    static int (*r)(const char *, int);
    if (!r) r = dlsym(RTLD_NEXT, "access");
    return r(R(p), m);
}

int faccessat(int d, const char *p, int m, int f)
{
    static int (*r)(int, const char *, int, int);
    if (!r) r = dlsym(RTLD_NEXT, "faccessat");
    return r(d, Rabs(p), m, f);
}

ssize_t readlink(const char *p, char *b, size_t n)
{
    static ssize_t (*r)(const char *, char *, size_t);
    if (!r) r = dlsym(RTLD_NEXT, "readlink");
    return r(R(p), b, n);
}

ssize_t readlinkat(int d, const char *p, char *b, size_t n)
{
    static ssize_t (*r)(int, const char *, char *, size_t);
    if (!r) r = dlsym(RTLD_NEXT, "readlinkat");
    return r(d, Rabs(p), b, n);
}

char *realpath(const char *p, char *o)
{
    static char *(*r)(const char *, char *);
    if (!r) r = dlsym(RTLD_NEXT, "realpath");
    return r(R(p), o);
}

FILE *fopen(const char *p, const char *m)
{
    static FILE *(*r)(const char *, const char *);
    if (!r) r = dlsym(RTLD_NEXT, "fopen");
    return r(R(p), m);
}

FILE *fopen64(const char *p, const char *m)
{
    static FILE *(*r)(const char *, const char *);
    if (!r) r = dlsym(RTLD_NEXT, "fopen64");
    return r(R(p), m);
}
