/*
 * netlink-shim.c - let udev-based device monitoring start.
 *
 * Android denies this container a kernel uevent socket:
 *
 *     socket(PF_NETLINK, SOCK_RAW, NETLINK_KOBJECT_UEVENT) -> EACCES
 *
 * libudev needs that socket for udev_monitor_new_from_netlink(), which
 * therefore returns NULL. Chromium's UdevWatcher::Create() treats a NULL
 * monitor as fatal and returns no watcher at all, which disables device
 * monitoring -- and with it the whole media device list. The visible result
 * is a browser that reports zero cameras *and* zero microphones, even though
 * both are present and working for every other application:
 *
 *     Failed to initialize a udev monitor.
 *     navigator.mediaDevices.enumerateDevices() -> 0 video, 0 audio
 *     getUserMedia() -> NotFoundError: Requested device not found
 *
 * Note this is not a camera problem. The microphone disappears the same way,
 * which is what shows that the blockage is in monitoring rather than in any
 * particular device.
 *
 * What this does: when the kernel refuses that socket, hand back one that
 * behaves like a uevent socket on a machine where nothing is ever plugged in
 * or unplugged. It can be bound, configured and polled; it simply never
 * reports an event.
 *
 * That is an honest answer here rather than a convenient one. This is a
 * phone: there is no hot-pluggable hardware behind the bridge, so a monitor
 * that reports no changes is describing the machine accurately. Applications
 * enumerate devices the ordinary way once monitoring has started, and that
 * enumeration is real -- it finds the camera on /dev/video0 and the
 * microphone through PulseAudio.
 *
 * Both ends of the pair are kept open on purpose. Closing the write end
 * would make the read end report end-of-file, so a poll() loop would see the
 * socket as permanently readable and spin. With both ends held, a read simply
 * never completes, which is what an idle monitor looks like.
 *
 * BRIDGE_NETLINK_DISABLE=1 turns this off. A socket the kernel is willing to
 * create is always passed through untouched, as is any failure other than a
 * permission denial, so a genuinely broken system still reports the truth.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <linux/netlink.h>

#define MAX_FAKE 16

static int (*real_socket)(int, int, int);
static int (*real_bind)(int, const struct sockaddr *, socklen_t);
static int (*real_setsockopt)(int, int, int, const void *, socklen_t);
static int (*real_getsockname)(int, struct sockaddr *, socklen_t *);
static int (*real_close)(int);

static int fake_fds[MAX_FAKE];
static int fake_n;
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;

static int inited;
static int disabled;

__attribute__((constructor)) static void nl_shim_init(void)
{
    const char *off = getenv("BRIDGE_NETLINK_DISABLE");
    disabled = (off && *off && strcmp(off, "0") != 0);

    real_socket = dlsym(RTLD_NEXT, "socket");
    real_bind = dlsym(RTLD_NEXT, "bind");
    real_setsockopt = dlsym(RTLD_NEXT, "setsockopt");
    real_getsockname = dlsym(RTLD_NEXT, "getsockname");
    real_close = dlsym(RTLD_NEXT, "close");

    for (int i = 0; i < MAX_FAKE; i++)
        fake_fds[i] = -1;
    inited = 1;
}

static int is_fake(int fd)
{
    if (!inited || fd < 0)
        return 0;
    int found = 0;
    pthread_mutex_lock(&lk);
    for (int i = 0; i < fake_n; i++)
        if (fake_fds[i] == fd) { found = 1; break; }
    pthread_mutex_unlock(&lk);
    return found;
}

static void remember(int fd)
{
    pthread_mutex_lock(&lk);
    if (fake_n < MAX_FAKE)
        fake_fds[fake_n++] = fd;
    pthread_mutex_unlock(&lk);
}

static void forget(int fd)
{
    pthread_mutex_lock(&lk);
    for (int i = 0; i < fake_n; i++) {
        if (fake_fds[i] == fd) {
            fake_fds[i] = fake_fds[--fake_n];
            break;
        }
    }
    pthread_mutex_unlock(&lk);
}

int socket(int domain, int type, int protocol)
{
    if (!inited || !real_socket)
        return (int)syscall(SYS_socket, domain, type, protocol);

    int fd = real_socket(domain, type, protocol);
    if (fd >= 0 || disabled)
        return fd;
    if (domain != AF_NETLINK || protocol != NETLINK_KOBJECT_UEVENT)
        return fd;
    if (errno != EACCES && errno != EPERM)
        return fd;

    /* AF_UNIX has no SOCK_RAW, so the pair is a datagram socket; only the
       behavioural flags the caller asked for are carried across. */
    int sv[2];
    int flags = type & (SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (socketpair(AF_UNIX, SOCK_DGRAM | flags, 0, sv) != 0) {
        errno = EACCES;
        return -1;
    }

    /* sv[1] is deliberately never closed: see the file comment. */
    remember(sv[0]);
    return sv[0];
}

int bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    if (is_fake(fd))
        return 0;
    if (!inited || !real_bind)
        return (int)syscall(SYS_bind, fd, addr, len);
    return real_bind(fd, addr, len);
}

int setsockopt(int fd, int level, int optname, const void *val, socklen_t len)
{
    /* Accept buffer sizes and credential passing without complaint; none of
       it matters for a socket that will never carry a message. */
    if (is_fake(fd))
        return 0;
    if (!inited || !real_setsockopt)
        return (int)syscall(SYS_setsockopt, fd, level, optname, val, len);
    return real_setsockopt(fd, level, optname, val, len);
}

int getsockname(int fd, struct sockaddr *addr, socklen_t *len)
{
    if (is_fake(fd)) {
        struct sockaddr_nl nl;
        memset(&nl, 0, sizeof nl);
        nl.nl_family = AF_NETLINK;
        nl.nl_pid = (unsigned)getpid();
        socklen_t n = *len < (socklen_t)sizeof nl ? *len : (socklen_t)sizeof nl;
        memcpy(addr, &nl, n);
        *len = sizeof nl;
        return 0;
    }
    if (!inited || !real_getsockname)
        return (int)syscall(SYS_getsockname, fd, addr, len);
    return real_getsockname(fd, addr, len);
}

int close(int fd)
{
    if (inited)
        forget(fd);
    if (!real_close)
        return (int)syscall(SYS_close, fd);
    return real_close(fd);
}
