/*
 * pci-shim.c - make Chromium's GPU process survive on this device.
 *
 * Chromium collects basic graphics information by dlopen()ing libpci and
 * calling pci_system_init(). libpci's proc backend reads
 * /proc/bus/pci/devices. On Android that path exists but is unreadable, so
 * the call fails, the GPU process exits rc=1, and after a handful of retries
 * the browser aborts outright:
 *
 *     pcilib: Cannot open /proc/bus/pci/devices
 *     GPU process exited unexpectedly: exit_code=256
 *     FATAL ... GPU process isn't usable. Goodbye.
 *
 * It is not a GL problem -- --use-angle=swiftshader dies the same way, and so
 * does --disable-gpu, because the info-collection step runs regardless. Every
 * Chromium- or Electron-based application on this system hits it.
 *
 * The fix is to let that one open() succeed and return nothing. An empty read
 * is a valid answer meaning "zero PCI devices", which is true here: the GPU is
 * on the SoC, not a PCI bus. /dev/null is exactly that, always readable, and
 * needs no file to be installed anywhere.
 *
 * Scope: the redirect fires only for that exact path, so a process that never
 * touches it cannot tell the library is loaded. BRIDGE_PCI_DISABLE=1 turns it
 * off without uninstalling.
 *
 * Bootstrap safety matters here because this library is loaded into every
 * process on the system. dlsym() is entitled to allocate, and an allocation
 * can open files, so a call can arrive before the constructor has resolved the
 * real symbols. Calling through a NULL pointer at that moment would abort the
 * process. Every entry point therefore falls back to the raw syscall until
 * initialisation has finished.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

static const char *TARGET = "/proc/bus/pci/devices";

static int (*real_open)(const char *, int, ...);
static int (*real_open64)(const char *, int, ...);
static int (*real_openat)(int, const char *, int, ...);
static FILE *(*real_fopen)(const char *, const char *);
static FILE *(*real_fopen64)(const char *, const char *);

static int inited;
static int disabled;

__attribute__((constructor)) static void pci_shim_init(void)
{
    const char *off = getenv("BRIDGE_PCI_DISABLE");
    disabled = (off && *off && strcmp(off, "0") != 0);

    real_open = dlsym(RTLD_NEXT, "open");
    real_open64 = dlsym(RTLD_NEXT, "open64");
    real_openat = dlsym(RTLD_NEXT, "openat");
    real_fopen = dlsym(RTLD_NEXT, "fopen");
    real_fopen64 = dlsym(RTLD_NEXT, "fopen64");

    inited = 1;
}

static const char *redir(const char *path)
{
    if (disabled || !path)
        return path;
    if (strcmp(path, TARGET) != 0)
        return path;

    /* An override is honoured mainly so the behaviour can be inspected; the
       default is the one that is always present and always empty. */
    const char *alt = getenv("PCI_DEVICES_SHIM");
    return (alt && *alt) ? alt : "/dev/null";
}

static mode_t va_mode(int flags, va_list ap)
{
    return (flags & (O_CREAT | O_TMPFILE)) ? va_arg(ap, mode_t) : 0;
}

int open(const char *path, int flags, ...)
{
    va_list ap;
    va_start(ap, flags);
    mode_t m = va_mode(flags, ap);
    va_end(ap);

    const char *p = redir(path);
    if (!inited || !real_open)
        return (int)syscall(SYS_openat, AT_FDCWD, p, flags, m);
    return real_open(p, flags, m);
}

int open64(const char *path, int flags, ...)
{
    va_list ap;
    va_start(ap, flags);
    mode_t m = va_mode(flags, ap);
    va_end(ap);

    const char *p = redir(path);
    if (!inited || !real_open64)
        return (int)syscall(SYS_openat, AT_FDCWD, p, flags | O_LARGEFILE, m);
    return real_open64(p, flags, m);
}

int openat(int dirfd, const char *path, int flags, ...)
{
    va_list ap;
    va_start(ap, flags);
    mode_t m = va_mode(flags, ap);
    va_end(ap);

    /* redir() matches the full absolute path, which no relative lookup can
       equal, so a relative openat against another directory is unaffected. */
    const char *p = redir(path);
    if (!inited || !real_openat)
        return (int)syscall(SYS_openat, dirfd, p, flags, m);
    return real_openat(dirfd, p, flags, m);
}

FILE *fopen(const char *path, const char *mode)
{
    if (!inited || !real_fopen) {
        /* Reached only if stdio is used before our constructor runs, which
           would mean another preloaded library called it from its own
           constructor. Nothing useful can be done without the real symbol. */
        real_fopen = dlsym(RTLD_NEXT, "fopen");
        if (!real_fopen)
            return NULL;
    }
    return real_fopen(redir(path), mode);
}

FILE *fopen64(const char *path, const char *mode)
{
    if (!inited || !real_fopen64) {
        real_fopen64 = dlsym(RTLD_NEXT, "fopen64");
        if (!real_fopen64)
            return NULL;
    }
    return real_fopen64(redir(path), mode);
}
