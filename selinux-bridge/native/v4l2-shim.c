/*
 * v4l2-shim -- present the phone's camera as a real V4L2 capture node
 * (/dev/video0) to any unmodified Linux application.
 *
 * Why this exists
 * ---------------
 * The bridge can already deliver live camera frames into this container, but
 * only to something willing to read a FIFO. Every ordinary application --
 * ffmpeg, mpv, OBS, Cheese, Chromium -- opens a V4L2 device node instead.
 * The usual way to manufacture one is v4l2loopback, which is a kernel module,
 * which needs CAP_SYS_MODULE and a module tree. This container has neither
 * (CapEff is 0, /lib/modules does not exist), so that route is closed.
 *
 * But an application does not talk to a kernel module. It talks to libc:
 * open(), ioctl(), mmap(), read(), close(). Those are ordinary dynamic symbols
 * and can be interposed with LD_PRELOAD. So rather than create a device node,
 * this answers for one. The kernel is never involved and never needs to be.
 *
 * The real /dev/video0 exists on this device but is SELinux-denied to us, so
 * the path is a safe one to claim: nothing here can open it anyway, and the
 * fact that it exists means applications that glob /dev/video* still find it.
 * stat() and access() are interposed too, so it looks readable and looks like
 * a character device, which is what such scans check before opening.
 *
 * Design notes
 * ------------
 * The fd handed back to the application is the read end of a real pipe. That
 * is deliberate: it makes poll(), select() and epoll work correctly for free,
 * with no interception, because a pollable fd is genuinely what the caller
 * holds. One byte is written to that pipe per completed frame, and DQBUF
 * consumes one byte. Applications that block in poll() therefore wake exactly
 * when a frame is ready, and O_NONBLOCK behaves properly because the pipe
 * itself carries the flag.
 *
 * Geometry is discovered, not assumed. The HAL rounds a requested size to one
 * it actually supports, and the bridge's rotation fix (gap #19) swaps width
 * and height for a sensor mounted at 90 degrees -- ask for 640x480 and you get
 * 480x640. So the true geometry is read from the capture's own Y4M header and
 * cached on disk per requested size; S_FMT then reports what the camera will
 * really produce. V4L2 explicitly allows a driver to change a requested
 * format, and callers are required to honour what comes back.
 *
 * Environment:
 *   BRIDGE_V4L2_DEVICE   path to claim            (default /dev/video0)
 *   BRIDGE_V4L2_SIZE     requested WxH            (default 1280x720)
 *   BRIDGE_V4L2_FPS      frames per second        (default 30)
 *   BRIDGE_V4L2_CAMERA   camera index             (default 0)
 *   BRIDGE_CLIENT        path to bridge_client
 *   BRIDGE_V4L2_DEBUG    1 to trace to stderr
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <poll.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dlfcn.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <linux/videodev2.h>

#ifndef KERNEL_VERSION
#define KERNEL_VERSION(a, b, c) (((a) << 16) + ((b) << 8) + (c))
#endif

#define MAX_BUFS 8
#define DEFAULT_DEV "/dev/video0"

/*
 * Where to find the bridge client if BRIDGE_CLIENT is not set. This is not
 * a convenience default: an application that rewrites its environment loses
 * the variable, so for those this compiled-in path is the only way the shim
 * can still find the client. The Makefile fills it in from the checkout.
 */
#ifndef BRIDGE_CLIENT_DEFAULT
#define BRIDGE_CLIENT_DEFAULT "/usr/local/lib/selinux-bridge/bridge_client"
#endif

static int (*real_open)(const char *, int, ...);
static int (*real_open64)(const char *, int, ...);
static int (*real_openat)(int, const char *, int, ...);
static int (*real_close)(int);
static int (*real_ioctl)(int, unsigned long, ...);
static ssize_t (*real_read)(int, void *, size_t);
static void *(*real_mmap)(void *, size_t, int, int, int, off_t);
static void *(*real_mmap64)(void *, size_t, int, int, int, off_t);
static int (*real_munmap)(void *, size_t);
static int (*real_stat)(const char *, struct stat *);
static int (*real_access)(const char *, int);

/*
 * Nothing here may be used before the constructor has run. That is not a
 * theoretical concern: dlsym() can allocate, allocation can call mmap(), and
 * mmap() is interposed below -- so the very act of resolving the real symbols
 * can re-enter this library while the function pointers are still NULL.
 * Every interposed entry point therefore falls back to a raw syscall when its
 * pointer is not resolved yet, and `inited` keeps the fd-matching logic from
 * claiming fd 0 (which a zero-initialised C.fd would otherwise alias).
 */
static volatile int inited;

static const char *dev_path;
static int req_w = 1280, req_h = 720, req_fps = 30, cam_index;
static const char *client_path;
static int debug_on;
static int disabled;

static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;

/* Which thread holds lk, and where it took it. A shim that stops responding
 * looks identical from outside to an application that stopped calling, so
 * record enough to tell those apart without a debugger. */
/*
 * Re-entrancy guard.
 *
 * An interposed library is not allowed to assume that the code it calls will
 * stay out of the symbols it interposes. Chromium replaces the global
 * allocator with PartitionAlloc, and PartitionAlloc releases memory through
 * the *public* munmap -- which is us. So free(), called from REQBUFS while we
 * hold `lk`, re-entered our own munmap wrapper, which tried to take `lk`
 * again, and a non-recursive mutex duly deadlocked the video capture thread
 * for good. (glibc's own malloc calls a hidden __munmap alias that cannot be
 * interposed, which is exactly why ffmpeg never showed this and Chromium
 * always did.)
 *
 * The rule that removes the whole class of bug: while this thread is already
 * inside the shim, it is not an application making a V4L2 call, so every
 * entry point behaves as a plain pass-through.
 */
static __thread int shim_depth;
#define SHIM_BUSY() (shim_depth > 0)
#define SHIM_ENTER() (shim_depth++)
#define SHIM_LEAVE() (shim_depth--)

static pthread_cond_t stop_cv = PTHREAD_COND_INITIALIZER;
static int stopping;
static volatile pid_t lk_owner;
static volatile int lk_line;

#define LOCK() do { pthread_mutex_lock(&lk); \
                    lk_owner = (pid_t)syscall(SYS_gettid); lk_line = __LINE__; } while (0)
#define UNLOCK() do { lk_owner = 0; lk_line = 0; \
                      pthread_mutex_unlock(&lk); } while (0)

static struct {
    int fd;                      /* app-facing fd (pipe read end), -1 = closed */
    int notify_w;
    int width, height;
    size_t framesz, bufsz;
    int nbufs;
    unsigned char *bufs[MAX_BUFS];
    int qstate[MAX_BUFS];        /* 0 app-owned, 1 queued, 2 done */
    int ready[MAX_BUFS];
    struct timeval bufts[MAX_BUFS];
    int rhead, rtail, rcount;
    int streaming, failed;
    pid_t child;
    int childfd;
    pthread_t pump;
    int pump_running;
    unsigned long seq;
} C;

/*
 * getenv() returns a pointer into the process environment block, and some
 * programs rewrite that block after our constructor has already run.
 * Chromium is one: it scrubs its environment before forking workers, so a
 * pointer saved here later refers to memory that has been emptied or reused.
 *
 * The failure is confusing out of proportion to its cause -- the saved path
 * silently becomes "", and the only evidence is posix_spawn() reporting
 * "Exec format error" for a binary that is perfectly valid. Copying the
 * value at start-up removes the dependency on the caller's environment
 * staying still.
 */
static char *dup_env(const char *name)
{
    const char *v = getenv(name);
    if (!v || !*v)
        return NULL;
    size_t n = strlen(v) + 1;
    char *copy = malloc(n);
    if (!copy)
        return NULL;
    memcpy(copy, v, n);
    return copy;
}

static void dbg(const char *fmt, ...)
{
    if (!debug_on) return;

    /* One write() of a stack buffer, rather than fprintf.
     *
     * stdio takes a per-FILE lock and can allocate, so tracing through it
     * competes for exactly the resources a stuck shim is most likely to be
     * stuck on -- and then the trace stops too, which hides the very thing
     * it was added to show. A single write() to fd 2 is also atomic enough
     * that lines from different threads do not interleave. */
    char buf[512];
    int n = snprintf(buf, sizeof buf, "[v4l2-shim] ");
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof buf - (size_t)n - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 2) n = (int)sizeof buf - 2;
    /* Callers are inconsistent about trailing newlines; normalise here. */
    while (n > 0 && buf[n - 1] == '\n') n--;
    buf[n++] = '\n';
    ssize_t rc = write(2, buf, (size_t)n);
    (void)rc;
}

__attribute__((constructor))
static void shim_init(void)
{
    real_open    = dlsym(RTLD_NEXT, "open");
    real_open64  = dlsym(RTLD_NEXT, "open64");
    real_openat  = dlsym(RTLD_NEXT, "openat");
    real_close   = dlsym(RTLD_NEXT, "close");
    real_ioctl   = dlsym(RTLD_NEXT, "ioctl");
    real_read    = dlsym(RTLD_NEXT, "read");
    real_mmap    = dlsym(RTLD_NEXT, "mmap");
    real_mmap64  = dlsym(RTLD_NEXT, "mmap64");
    real_munmap  = dlsym(RTLD_NEXT, "munmap");
    real_stat    = dlsym(RTLD_NEXT, "stat");
    real_access  = dlsym(RTLD_NEXT, "access");

    dev_path = dup_env("BRIDGE_V4L2_DEVICE");
    if (!dev_path || !*dev_path) dev_path = DEFAULT_DEV;

    const char *s = getenv("BRIDGE_V4L2_SIZE");
    if (s) {
        int w, h;
        if (sscanf(s, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
            req_w = w;
            req_h = h;
        }
    }
    const char *f = getenv("BRIDGE_V4L2_FPS");
    if (f && atoi(f) > 0) req_fps = atoi(f);
    const char *ci = getenv("BRIDGE_V4L2_CAMERA");
    if (ci) cam_index = atoi(ci);

    client_path = dup_env("BRIDGE_CLIENT");
    if (!client_path || !*client_path)
        client_path = BRIDGE_CLIENT_DEFAULT;

    /* Level 1 traces setup and control ioctls; level 2 adds the per-frame
     * QBUF/DQBUF pair, which is far too noisy to leave on but is the only
     * way to tell "streaming started but no frame ever moved" apart from
     * "the application never asked for one". */
    const char *dbgv = getenv("BRIDGE_V4L2_DEBUG");
    debug_on = dbgv ? atoi(dbgv) : 0;

    /* An escape hatch worth having when this is preloaded system-wide: with
     * it set the library is inert and every call falls through to libc, so a
     * misbehaving application can be excluded without editing the profile
     * or rebuilding anything. */
    disabled = getenv("BRIDGE_V4L2_DISABLE") && *getenv("BRIDGE_V4L2_DISABLE") == '1';

    C.fd = -1;
    C.childfd = -1;
    C.notify_w = -1;
    inited = 1;
}

static int is_our_path(const char *p)
{
    return inited && !disabled && p && dev_path && strcmp(p, dev_path) == 0;
}

/* Secondary handles on the same node.
 *
 * A real V4L2 driver lets any number of processes open the device; what is
 * exclusive is streaming, not the file. Applications rely on that: Chromium
 * opens every camera it can see to read its capabilities, and it does so
 * while other tabs may already be capturing. Answering EBUSY to that probe
 * makes Chromium conclude the device has gone away and report
 * NotFoundError, so the honest-looking "busy" answer is in fact the wrong
 * one. Secondary handles therefore answer every descriptive ioctl and are
 * refused only where the kernel would refuse them: at REQBUFS and STREAMON,
 * which is where a second streamer would actually collide. */
#define MAX_EXTRA_FDS 8
static int extra_fds[MAX_EXTRA_FDS];
static int n_extra;

static int is_extra_fd(int fd)
{
    for (int i = 0; i < n_extra; i++)
        if (extra_fds[i] == fd) return 1;
    return 0;
}

/* C.fd is -1 when closed, so every one of these must reject fd < 0 first:
 * a plain close(-1) would otherwise compare equal and tear down state that
 * a live capture is using. */
static int is_primary_fd(int fd)
{
    return inited && fd >= 0 && C.fd == fd;
}

static int is_our_fd(int fd)
{
    if (!inited || fd < 0) return 0;
    return C.fd == fd || is_extra_fd(fd);
}

/* ---------- geometry discovery ---------- */

static int read_line(int fd, char *out, size_t max)
{
    size_t n = 0;
    while (n + 1 < max) {
        char c;
        ssize_t r = real_read(fd, &c, 1);
        if (r <= 0) return -1;
        if (c == '\n') break;
        out[n++] = c;
    }
    out[n] = 0;
    return (int)n;
}

static int read_full(int fd, void *buf, size_t n)
{
    unsigned char *p = buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = real_read(fd, p + got, n - got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

/*
 * Spawn bridge_client streaming Y4M on its stdout.
 *
 * posix_spawn rather than fork()+exec(), and that is not a stylistic choice:
 * the caller is a threaded application (ffmpeg, Chromium), and between fork()
 * and exec() a child may only call async-signal-safe functions. execl() is
 * not one -- it allocates -- so if another thread held the malloc lock at the
 * moment of the fork, the child deadlocks before it ever reaches the exec.
 * That is exactly what happened here: the forked children sat forever, still
 * showing the parent's name in ps. posix_spawn uses CLONE_VFORK internally
 * and never touches the parent's allocator.
 */
static int spawn_client(int frames, pid_t *pid_out, int *fd_out)
{
    int pfd[2];
    if (pipe(pfd) != 0) return -1;

    static char w[16], h[16], fps[16], nf[16], idx[16];
    snprintf(w, sizeof w, "%d", req_w);
    snprintf(h, sizeof h, "%d", req_h);
    snprintf(fps, sizeof fps, "%d", req_fps);
    snprintf(nf, sizeof nf, "%d", frames);
    snprintf(idx, sizeof idx, "%d", cam_index);

    char *argv[] = { (char *)"bridge_client", (char *)"camera", (char *)"-i", idx,
                     w, h, fps, nf, (char *)"-", NULL };

    /* Hand the child an environment with LD_PRELOAD removed, so this library
     * never interposes bridge_client's own open()/read() calls. */
    int n = 0;
    while (environ && environ[n]) n++;
    char **env = malloc((size_t)(n + 1) * sizeof *env);
    if (!env) {
        real_close(pfd[0]);
        real_close(pfd[1]);
        return -1;
    }
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (strncmp(environ[i], "LD_PRELOAD=", 11) == 0) continue;
        env[m++] = environ[i];
    }
    env[m] = NULL;

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pfd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&fa, pfd[0]);
    posix_spawn_file_actions_addclose(&fa, pfd[1]);
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (!debug_on)
        posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    pid_t pid = -1;
    int rc = posix_spawn(&pid, client_path, &fa, NULL, argv, env);
    posix_spawn_file_actions_destroy(&fa);
    free(env);
    real_close(pfd[1]);

    if (rc != 0) {
        real_close(pfd[0]);
        dbg("posix_spawn(%s) failed: %s", client_path, strerror(rc));
        return -1;
    }
    *pid_out = pid;
    *fd_out = pfd[0];
    return 0;
}

static void geom_cache_path(char *out, size_t n)
{
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(out, n, "%s/.cache/selinux-bridge/v4l2-geom-%dx%d-cam%d",
             home, req_w, req_h, cam_index);
}

/* Parse "YUV4MPEG2 W480 H640 F30:1 ..." */
static int parse_y4m_header(const char *line, int *w, int *h)
{
    if (strncmp(line, "YUV4MPEG2", 9) != 0) return -1;
    const char *p = line;
    int gw = 0, gh = 0;
    while (*p) {
        if (*p == ' ') {
            p++;
            if (*p == 'W') gw = atoi(p + 1);
            else if (*p == 'H') gh = atoi(p + 1);
            continue;
        }
        p++;
    }
    if (gw <= 0 || gh <= 0) return -1;
    *w = gw;
    *h = gh;
    return 0;
}

/*
 * Learn the geometry the camera really produces. Cached on disk, because the
 * only way to find out is to open the camera, which blinks the privacy
 * indicator and costs a couple of seconds.
 */
static int ensure_geom(void)
{
    if (C.width > 0 && C.height > 0) return 0;

    char cache[512];
    geom_cache_path(cache, sizeof cache);

    FILE *cf = fopen(cache, "r");
    if (cf) {
        int w = 0, h = 0;
        if (fscanf(cf, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
            fclose(cf);
            C.width = w;
            C.height = h;
            C.framesz = (size_t)w * h * 3 / 2;
            dbg("geometry %dx%d (cached)", w, h);
            return 0;
        }
        fclose(cf);
    }

    pid_t pid;
    int fd;
    if (spawn_client(2, &pid, &fd) != 0) {
        dbg("probe: could not spawn bridge_client");
        return -1;
    }

    char line[256];
    int ok = -1, w = 0, h = 0;
    if (read_line(fd, line, sizeof line) > 0 && parse_y4m_header(line, &w, &h) == 0)
        ok = 0;

    real_close(fd);
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);

    if (ok != 0) {
        dbg("probe: no usable Y4M header (is the bridge running and camera permission granted?)");
        return -1;
    }

    C.width = w;
    C.height = h;
    C.framesz = (size_t)w * h * 3 / 2;
    dbg("geometry %dx%d (probed)", w, h);

    char dirbuf[512];
    const char *home = getenv("HOME");
    snprintf(dirbuf, sizeof dirbuf, "%s/.cache/selinux-bridge", home ? home : "/tmp");
    mkdir(dirbuf, 0755);
    cf = fopen(cache, "w");
    if (cf) {
        fprintf(cf, "%dx%d\n", w, h);
        fclose(cf);
    }
    return 0;
}

/* ---------- streaming ---------- */

static void push_ready(int idx)
{
    if (C.rcount >= C.nbufs) return;
    C.ready[C.rtail] = idx;
    C.rtail = (C.rtail + 1) % MAX_BUFS;
    C.rcount++;
}

static int pop_ready(void)
{
    if (C.rcount <= 0) return -1;
    int idx = C.ready[C.rhead];
    C.rhead = (C.rhead + 1) % MAX_BUFS;
    C.rcount--;
    return idx;
}

static void *pump_thread(void *arg)
{
    (void)arg;
    char line[256];

    /* Consume the Y4M stream header once. */
    if (read_line(C.childfd, line, sizeof line) <= 0) {
        LOCK();
        C.failed = 1;
        UNLOCK();
        if (C.notify_w >= 0) { char b = 1; ssize_t rc = write(C.notify_w, &b, 1); (void)rc; }
        return NULL;
    }

    unsigned char *scratch = malloc(C.framesz);
    if (!scratch) return NULL;

    for (;;) {
        if (read_line(C.childfd, line, sizeof line) < 0) break;
        if (strncmp(line, "FRAME", 5) != 0) break;
        if (read_full(C.childfd, scratch, C.framesz) != 0) break;

        LOCK();
        if (!C.streaming) { UNLOCK(); break; }
        int target = -1;
        for (int i = 0; i < C.nbufs; i++) {
            if (C.qstate[i] == 1) { target = i; break; }
        }
        if (target >= 0) {
            memcpy(C.bufs[target], scratch, C.framesz);
            /* A capture timestamp is not decoration. Without it every frame
             * arrives at the same instant as far as libavformat is concerned,
             * and the muxer drops all but the first few -- which presents as
             * a capture that runs at full speed and still produces a file
             * three frames long. */
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            C.bufts[target].tv_sec = now.tv_sec;
            C.bufts[target].tv_usec = now.tv_nsec / 1000;
            C.qstate[target] = 2;
            C.seq++;
            push_ready(target);
        }
        int nw = C.notify_w;
        UNLOCK();

        /* Only wake a waiter if a buffer was actually filled; dropping a
         * frame because the application has not queued one is normal. */
        if (target >= 0 && nw >= 0) {
            char b = 0;
            ssize_t rc = write(nw, &b, 1);
            (void)rc;
        }
    }

    free(scratch);
    LOCK();
    C.failed = 1;
    int nw = C.notify_w;
    UNLOCK();
    if (nw >= 0) { char b = 1; ssize_t rc = write(nw, &b, 1); (void)rc; }
    return NULL;
}

static int start_stream(void)
{
    if (C.streaming) return 0;
    if (ensure_geom() != 0) return -1;

    pid_t pid;
    int fd;
    if (spawn_client(0, &pid, &fd) != 0) return -1;

    C.child = pid;
    C.childfd = fd;
    C.streaming = 1;
    C.failed = 0;
    C.seq = 0;
    C.rhead = C.rtail = C.rcount = 0;

    if (pthread_create(&C.pump, NULL, pump_thread, NULL) != 0) {
        C.streaming = 0;
        real_close(fd);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return -1;
    }
    C.pump_running = 1;
    dbg("streaming started (%dx%d)", C.width, C.height);
    return 0;
}

static void stop_stream(void)
{
    /* Teardown has to be serialised and it has to be idempotent.
     *
     * Two different callers reach here: VIDIOC_STREAMOFF, and VIDIOC_REQBUFS
     * when an application releases its buffers. Chromium issues both, from
     * two threads, a few microseconds apart. The guard used to be on
     * C.streaming alone, so the second caller saw streaming already clear
     * and returned immediately -- while the pump thread was still alive and
     * still memcpy()ing into the buffers that the very next line then
     * free()d. The result is a corrupted heap, which does not announce
     * itself: the next allocation from any thread simply never returns, so
     * the shim goes silent and the application appears to have stopped
     * asking for frames. Waiting for the in-progress teardown, and gating on
     * the pump rather than on the stream flag, removes both halves of that.
     *
     * Two threads must also never join the same pump thread, which is
     * undefined behaviour in its own right; the second one waits here. */
    while (stopping) {
        pthread_cond_wait(&stop_cv, &lk);
        lk_owner = (pid_t)syscall(SYS_gettid);
        lk_line = __LINE__;
    }
    if (!C.streaming && !C.pump_running && C.child <= 0) return;

    stopping = 1;
    C.streaming = 0;

    if (C.child > 0) {
        kill(C.child, SIGTERM);
    }
    if (C.childfd >= 0) {
        real_close(C.childfd);
        C.childfd = -1;
    }

    if (C.pump_running) {
        UNLOCK();
        pthread_join(C.pump, NULL);
        LOCK();
        C.pump_running = 0;
    }
    if (C.child > 0) {
        waitpid(C.child, NULL, 0);
        C.child = -1;
    }
    C.rhead = C.rtail = C.rcount = 0;

    /* Nudge any caller already waiting for a frame so it notices at once
     * that streaming has ended, instead of waiting out its poll timeout. */
    if (C.notify_w >= 0) {
        char b = 1;
        ssize_t rc = write(C.notify_w, &b, 1);
        (void)rc;
    }
    dbg("streaming stopped, camera released (%lu frames delivered)", C.seq);

    stopping = 0;
    pthread_cond_broadcast(&stop_cv);
}

static void free_buffers(void)
{
    for (int i = 0; i < MAX_BUFS; i++) {
        if (C.bufs[i]) {
            if (debug_on > 1) dbg("free_buffers: freeing %d (%p)", i, C.bufs[i]);
            free(C.bufs[i]);
            C.bufs[i] = NULL;
        }
        C.qstate[i] = 0;
    }
    C.nbufs = 0;
}

/* ---------- interposed entry points ---------- */

static int open_fake(int flags)
{
    if (debug_on > 1) dbg("open_fake: waiting for lock (held by tid %d at line %d)", (int)lk_owner, lk_line);
    SHIM_ENTER();
    LOCK();
    if (C.fd >= 0) {
        /* Someone already holds the capture handle. Hand out a descriptive
         * secondary handle rather than EBUSY (see is_extra_fd above). It is
         * a real fd on /dev/null so that close(), poll() and fstat() behave,
         * but it never carries frames. */
        if (n_extra >= MAX_EXTRA_FDS) {
            UNLOCK();
            SHIM_LEAVE();
            errno = EBUSY;
            return -1;
        }
        int nfd = real_open ? real_open("/dev/null", O_RDONLY, 0)
                            : (int)syscall(SYS_openat, AT_FDCWD, "/dev/null", O_RDONLY, 0);
        if (nfd < 0) {
            UNLOCK();
            SHIM_LEAVE();
            errno = EBUSY;
            return -1;
        }
        extra_fds[n_extra++] = nfd;
        UNLOCK();
        SHIM_LEAVE();
        dbg("open(%s) -> fd %d (secondary, describe-only)", dev_path, nfd);
        return nfd;
    }
    int pfd[2];
    if (pipe(pfd) != 0) {
        UNLOCK();
        SHIM_LEAVE();
        errno = ENOMEM;
        return -1;
    }
    if (flags & O_NONBLOCK) {
        int fl = fcntl(pfd[0], F_GETFL, 0);
        fcntl(pfd[0], F_SETFL, fl | O_NONBLOCK);
    }
    C.fd = pfd[0];
    C.notify_w = pfd[1];
    C.width = C.height = 0;
    C.framesz = C.bufsz = 0;
    C.streaming = 0;
    C.failed = 0;
    UNLOCK();
    SHIM_LEAVE();
    dbg("open(%s) -> fd %d", dev_path, pfd[0]);
    return pfd[0];
}

int open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap);
    }
    if (!SHIM_BUSY() && is_our_path(path)) return open_fake(flags);
    if (!real_open) return (int)syscall(SYS_openat, AT_FDCWD, path, flags, mode);
    return real_open(path, flags, mode);
}

int open64(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap);
    }
    if (!SHIM_BUSY() && is_our_path(path)) return open_fake(flags);
    if (real_open64) return real_open64(path, flags, mode);
    if (real_open) return real_open(path, flags, mode);
    return (int)syscall(SYS_openat, AT_FDCWD, path, flags, mode);
}

int openat(int dirfd, const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap);
    }
    if (!SHIM_BUSY() && is_our_path(path)) return open_fake(flags);
    if (!real_openat) return (int)syscall(SYS_openat, dirfd, path, flags, mode);
    return real_openat(dirfd, path, flags, mode);
}

int openat64(int dirfd, const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap);
    }
    if (!SHIM_BUSY() && is_our_path(path)) return open_fake(flags);
    static int (*real_openat64)(int, const char *, int, ...);
    if (!real_openat64) real_openat64 = dlsym(RTLD_NEXT, "openat64");
    if (real_openat64) return real_openat64(dirfd, path, flags, mode);
    if (real_openat) return real_openat(dirfd, path, flags, mode);
    return (int)syscall(SYS_openat, dirfd, path, flags, mode);
}

int close(int fd)
{
    int ours = 0;
    if (inited && !SHIM_BUSY()) {
        SHIM_ENTER();
        if (debug_on > 1 && fd >= 0 && (fd == C.fd || is_extra_fd(fd)))
            dbg("close(%d): waiting for lock (held by tid %d at line %d)", fd, (int)lk_owner, lk_line);
        LOCK();
        if (fd >= 0 && is_extra_fd(fd)) {
            /* A secondary handle owns no capture state, so closing it must
             * not tear down the stream the primary handle is running. */
            for (int i = 0; i < n_extra; i++) {
                if (extra_fds[i] == fd) {
                    extra_fds[i] = extra_fds[--n_extra];
                    break;
                }
            }
            ours = 1;
        } else if (is_primary_fd(fd)) {
            ours = 1;
            stop_stream();
            free_buffers();
            if (C.notify_w >= 0) { real_close(C.notify_w); C.notify_w = -1; }
            C.fd = -1;
        }
        UNLOCK();
        SHIM_LEAVE();
    }
    if (ours) dbg("close(%d)", fd);
    if (!real_close) return (int)syscall(SYS_close, fd);
    return real_close(fd);
}

/* Caller has already established that fd is our primary handle and has
 * entered the shim, so every exit here is a plain return. */
static ssize_t read_frame(int fd, void *buf, size_t count)
{
    LOCK();
    if (!C.streaming) {
        if (C.nbufs == 0) {
            /* read() mode never calls REQBUFS, so make our own ring. */
            if (ensure_geom() != 0) {
                UNLOCK();
                errno = EIO;
                return -1;
            }
            C.nbufs = 2;
            for (int i = 0; i < C.nbufs; i++) {
                C.bufs[i] = malloc(C.framesz);
                if (!C.bufs[i]) { UNLOCK(); errno = ENOMEM; return -1; }
                C.qstate[i] = 1;
            }
        }
        if (start_stream() != 0) {
            UNLOCK();
            errno = EIO;
            return -1;
        }
    }
    UNLOCK();

    char tok;
    ssize_t r = real_read(fd, &tok, 1);
    if (r <= 0) return r;

    LOCK();
    int idx = pop_ready();
    if (idx < 0) {
        UNLOCK();
        errno = C.failed ? EIO : EAGAIN;
        return -1;
    }
    size_t n = count < C.framesz ? count : C.framesz;
    memcpy(buf, C.bufs[idx], n);
    C.qstate[idx] = 1;
    UNLOCK();
    return (ssize_t)n;
}

ssize_t read(int fd, void *buf, size_t count)
{
    int ours;
    if (!inited || SHIM_BUSY()) {
        if (real_read) return real_read(fd, buf, count);
        return (ssize_t)syscall(SYS_read, fd, buf, count);
    }
    LOCK();
    ours = is_primary_fd(fd);
    UNLOCK();
    if (!ours) return real_read(fd, buf, count);

    /* V4L2 read() mode: hand back exactly one frame. */
    SHIM_ENTER();
    ssize_t rc = read_frame(fd, buf, count);
    int e = errno;
    SHIM_LEAVE();
    errno = e;
    return rc;
}

static void *mmap_common(void *addr, size_t length, int prot, int flags,
                         int fd, off_t offset, void *(*fallback)(void *, size_t, int, int, int, off_t))
{
    int ours;
    if (SHIM_BUSY()) {
        if (!fallback)
            return (void *)syscall(SYS_mmap, addr, length, prot, flags, fd, offset);
        return fallback(addr, length, prot, flags, fd, offset);
    }
    SHIM_ENTER();
    LOCK();
    ours = is_primary_fd(fd);
    if (ours) {
        int idx = (C.bufsz > 0) ? (int)(offset / (off_t)C.bufsz) : -1;
        if (idx >= 0 && idx < C.nbufs && C.bufs[idx]) {
            void *p = C.bufs[idx];
            UNLOCK();
            SHIM_LEAVE();
            dbg("mmap buffer %d", idx);
            return p;
        }
        UNLOCK();
        SHIM_LEAVE();
        errno = EINVAL;
        return MAP_FAILED;
    }
    UNLOCK();
    SHIM_LEAVE();
    if (!fallback)
        return (void *)syscall(SYS_mmap, addr, length, prot, flags, fd, offset);
    return fallback(addr, length, prot, flags, fd, offset);
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    if (!inited)
        return (void *)syscall(SYS_mmap, addr, length, prot, flags, fd, offset);
    return mmap_common(addr, length, prot, flags, fd, offset, real_mmap);
}

/*
 * Anything built with _FILE_OFFSET_BITS=64 -- which is most things, ffmpeg
 * included -- calls mmap64 rather than mmap. Interposing only mmap silently
 * leaves those callers on the real one, where mapping our pipe fd fails with
 * EACCES and looks exactly like a permissions problem on the device node.
 */
void *mmap64(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    if (!inited)
        return (void *)syscall(SYS_mmap, addr, length, prot, flags, fd, offset);
    return mmap_common(addr, length, prot, flags, fd, offset, real_mmap64);
}

int munmap(void *addr, size_t length)
{
    if (!inited || SHIM_BUSY()) {
        if (real_munmap) return real_munmap(addr, length);
        return (int)syscall(SYS_munmap, addr, length);
    }
    SHIM_ENTER();
    LOCK();
    for (int i = 0; i < C.nbufs; i++) {
        if (C.bufs[i] == addr) {
            UNLOCK();
            SHIM_LEAVE();
            return 0;   /* ours; freed by REQBUFS(0) or close() */
        }
    }
    UNLOCK();
    SHIM_LEAVE();
    if (!real_munmap) return (int)syscall(SYS_munmap, addr, length);
    return real_munmap(addr, length);
}

/* Caller must hold lk. */
static void fill_buf(struct v4l2_buffer *b, unsigned i)
{
    memset(b, 0, sizeof *b);
    b->index = i;
    b->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b->memory = V4L2_MEMORY_MMAP;
    b->bytesused = (unsigned)C.framesz;
    b->length = (unsigned)C.bufsz;
    b->m.offset = (unsigned)(i * C.bufsz);
    b->field = V4L2_FIELD_NONE;
    b->sequence = (unsigned)C.seq;
    b->timestamp = C.bufts[i];
    b->flags = V4L2_BUF_FLAG_DONE | V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
}

static int do_ioctl(unsigned long req, void *arg)
{
    switch (req) {

    case VIDIOC_QUERYCAP: {
        struct v4l2_capability *c = arg;
        memset(c, 0, sizeof *c);
        strncpy((char *)c->driver, "selinux-bridge", sizeof c->driver - 1);
        strncpy((char *)c->card, "Phone Camera (SELinux Bridge)", sizeof c->card - 1);
        strncpy((char *)c->bus_info, "platform:selinux-bridge", sizeof c->bus_info - 1);
        c->version = KERNEL_VERSION(1, 0, 0);
        c->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING | V4L2_CAP_READWRITE;
        c->capabilities = c->device_caps | V4L2_CAP_DEVICE_CAPS;
        return 0;
    }

    case VIDIOC_ENUM_FMT: {
        struct v4l2_fmtdesc *f = arg;
        if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE || f->index != 0) { errno = EINVAL; return -1; }
        f->flags = 0;
        f->pixelformat = V4L2_PIX_FMT_YUV420;
        strncpy((char *)f->description, "Planar YUV 4:2:0", sizeof f->description - 1);
        return 0;
    }

    case VIDIOC_G_FMT:
    case VIDIOC_S_FMT:
    case VIDIOC_TRY_FMT: {
        struct v4l2_format *f = arg;
        if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) { errno = EINVAL; return -1; }
        if (ensure_geom() != 0) { errno = EIO; return -1; }
        /* Always answer with what the camera will really produce. The HAL
         * rounds sizes and the rotation fix swaps them, so a requested
         * format is a hint; V4L2 requires callers to honour the reply. */
        f->fmt.pix.width = C.width;
        f->fmt.pix.height = C.height;
        f->fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
        f->fmt.pix.field = V4L2_FIELD_NONE;
        f->fmt.pix.bytesperline = C.width;
        f->fmt.pix.sizeimage = (unsigned)C.framesz;
        f->fmt.pix.colorspace = V4L2_COLORSPACE_SMPTE170M;
        return 0;
    }

    case VIDIOC_ENUM_FRAMESIZES: {
        struct v4l2_frmsizeenum *fs = arg;
        if (fs->index != 0 || fs->pixel_format != V4L2_PIX_FMT_YUV420) { errno = EINVAL; return -1; }
        if (ensure_geom() != 0) { errno = EIO; return -1; }
        fs->type = V4L2_FRMSIZE_TYPE_DISCRETE;
        fs->discrete.width = C.width;
        fs->discrete.height = C.height;
        return 0;
    }

    case VIDIOC_ENUM_FRAMEINTERVALS: {
        struct v4l2_frmivalenum *fi = arg;
        if (fi->index != 0) { errno = EINVAL; return -1; }
        fi->type = V4L2_FRMIVAL_TYPE_DISCRETE;
        fi->discrete.numerator = 1;
        fi->discrete.denominator = req_fps;
        return 0;
    }

    case VIDIOC_REQBUFS: {
        struct v4l2_requestbuffers *r = arg;
        if (r->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) { errno = EINVAL; return -1; }
        if (r->memory != V4L2_MEMORY_MMAP && r->memory != V4L2_MEMORY_USERPTR) {
            errno = EINVAL; return -1;
        }
        if (debug_on > 1) dbg("REQBUFS enter count=%u", r->count);
        stop_stream();
        if (debug_on > 1) dbg("REQBUFS: stream stopped");
        free_buffers();
        if (debug_on > 1) dbg("REQBUFS: buffers freed");
        if (r->count == 0) return 0;              /* teardown */
        if (ensure_geom() != 0) { errno = EIO; return -1; }

        int n = (int)r->count;
        if (n > MAX_BUFS) n = MAX_BUFS;
        if (n < 2) n = 2;
        long pagesz = sysconf(_SC_PAGESIZE);
        C.bufsz = ((C.framesz + pagesz - 1) / pagesz) * pagesz;
        for (int i = 0; i < n; i++) {
            C.bufs[i] = aligned_alloc(pagesz, C.bufsz);
            if (!C.bufs[i]) { free_buffers(); errno = ENOMEM; return -1; }
            memset(C.bufs[i], 0, C.bufsz);
            C.qstate[i] = 0;
        }
        C.nbufs = n;
        r->count = n;
        dbg("REQBUFS -> %d buffers of %zu", n, C.framesz);
        return 0;
    }

    case VIDIOC_QUERYBUF: {
        struct v4l2_buffer *b = arg;
        if (b->index >= (unsigned)C.nbufs) { errno = EINVAL; return -1; }
        unsigned idx = b->index;
        memset(b, 0, sizeof *b);
        b->index = idx;
        b->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b->memory = V4L2_MEMORY_MMAP;
        b->length = (unsigned)C.bufsz;
        b->m.offset = (unsigned)(idx * C.bufsz);
        b->bytesused = (unsigned)C.framesz;
        b->flags = C.qstate[idx] == 1 ? V4L2_BUF_FLAG_QUEUED : 0;
        return 0;
    }

    case VIDIOC_QBUF: {
        struct v4l2_buffer *b = arg;
        if (b->index >= (unsigned)C.nbufs) { errno = EINVAL; return -1; }
        C.qstate[b->index] = 1;
        b->flags |= V4L2_BUF_FLAG_QUEUED;
        return 0;
    }

    case VIDIOC_DQBUF: {
        struct v4l2_buffer *b = arg;
        if (!C.streaming) { errno = EINVAL; return -1; }

        /* Wait in bounded steps rather than blocking on the pipe outright.
         *
         * The pipe byte is only a hint that something changed; correctness
         * comes from re-reading the state each time round. Blocking on the
         * read itself made this dependent on every producer remembering to
         * post a wakeup, and one missed wakeup left the caller stuck for
         * good: Chromium blocks here on its capture thread, the device
         * teardown that follows needs the same lock, and from the outside
         * the whole browser simply stops seeing the camera -- with no error
         * anywhere, because nothing had failed. Re-checking on a timer also
         * gives STREAMOFF-while-blocked the behaviour a real driver has. */
        int nonblock = (fcntl(C.fd, F_GETFL, 0) & O_NONBLOCK) != 0;

        for (;;) {
            int idx, failed, streaming;

            LOCK();
            idx = pop_ready();
            failed = C.failed;
            streaming = C.streaming;
            if (idx >= 0) {
                fill_buf(b, (unsigned)idx);
                C.qstate[idx] = 0;
                UNLOCK();
                /* Balance the wakeup the pump wrote for this frame. */
                char tok;
                ssize_t rc = real_read(C.fd, &tok, 1);
                (void)rc;
                return 0;
            }
            UNLOCK();

            if (failed) { errno = EIO; return -1; }
            if (!streaming) { errno = EINVAL; return -1; }
            if (nonblock) { errno = EAGAIN; return -1; }

            struct pollfd pfd;
            pfd.fd = C.fd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            int pr = poll(&pfd, 1, 200);
            if (pr < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            if (pr > 0 && (pfd.revents & POLLIN)) {
                char tok;
                ssize_t rc = real_read(C.fd, &tok, 1);
                if (rc == 0) { errno = EIO; return -1; }
            }
        }
    }

    case VIDIOC_STREAMON: {
        if (C.nbufs == 0) { errno = EINVAL; return -1; }
        if (start_stream() != 0) { errno = EIO; return -1; }
        return 0;
    }

    case VIDIOC_STREAMOFF: {
        stop_stream();
        for (int i = 0; i < C.nbufs; i++) C.qstate[i] = 0;
        return 0;
    }

    case VIDIOC_ENUMINPUT: {
        struct v4l2_input *in = arg;
        if (in->index != 0) { errno = EINVAL; return -1; }
        memset(in, 0, sizeof *in);
        strncpy((char *)in->name, "Phone Camera", sizeof in->name - 1);
        in->type = V4L2_INPUT_TYPE_CAMERA;
        return 0;
    }

    case VIDIOC_G_INPUT: {
        *(int *)arg = 0;
        return 0;
    }

    case VIDIOC_S_INPUT: {
        if (*(int *)arg != 0) { errno = EINVAL; return -1; }
        return 0;
    }

    case VIDIOC_G_PARM: {
        struct v4l2_streamparm *p = arg;
        if (p->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) { errno = EINVAL; return -1; }
        memset(&p->parm.capture, 0, sizeof p->parm.capture);
        p->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
        p->parm.capture.timeperframe.numerator = 1;
        p->parm.capture.timeperframe.denominator = req_fps;
        p->parm.capture.readbuffers = 2;
        return 0;
    }

    case VIDIOC_S_PARM: {
        struct v4l2_streamparm *p = arg;
        if (p->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) { errno = EINVAL; return -1; }
        p->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
        p->parm.capture.timeperframe.numerator = 1;
        p->parm.capture.timeperframe.denominator = req_fps;
        return 0;
    }

    default:
        /* A real driver answers ENOTTY for ioctls it does not implement, and
         * libv4l/ffmpeg both treat that as "feature absent" rather than an
         * error. Controls, cropping and events all land here deliberately. */
        errno = ENOTTY;
        return -1;
    }
}

int ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    /*
     * V4L2 request codes have the top bit set (VIDIOC_QUERYCAP is
     * 0x80685600), so a caller that holds one in a signed int -- which
     * Chromium does -- passes a sign-extended value here:
     * 0xFFFFFFFF80685600. The low 32 bits are the real request, and
     * _IOC_NR()/_IOC_SIZE() mask the rest away, which is why such a call
     * still decodes correctly while failing to match any case label.
     *
     * The kernel truncates to 32 bits, so doing the same is not a
     * workaround but the same rule applied in the same place. Without it
     * every ioctl from such a caller falls through to ENOTTY, and the only
     * symptom is an application that opens the device, asks one question
     * and silently gives up.
     */
    request &= 0xFFFFFFFFUL;

    int ours, secondary;
    if (!inited || SHIM_BUSY()) {
        if (real_ioctl) return real_ioctl(fd, request, arg);
        return (int)syscall(SYS_ioctl, fd, request, arg);
    }
    if (debug_on > 1 && fd >= 0 && fd == C.fd)
        dbg("ioctl nr=%lu: waiting for lock (held by tid %d at line %d)", _IOC_NR(request), (int)lk_owner, lk_line);
    LOCK();
    ours = is_our_fd(fd);
    secondary = fd >= 0 && is_extra_fd(fd);
    UNLOCK();
    if (!ours) {
        if (!real_ioctl) return (int)syscall(SYS_ioctl, fd, request, arg);
        return real_ioctl(fd, request, arg);
    }

    /* Secondary handles describe the device but cannot claim it. This is
     * where the kernel draws the line for a single-stream device, and
     * drawing it in the same place keeps capability probes working while
     * a capture is in progress. */
    if (secondary) {
        switch (request) {
        case VIDIOC_REQBUFS:
        case VIDIOC_QUERYBUF:
        case VIDIOC_QBUF:
        case VIDIOC_DQBUF:
        case VIDIOC_STREAMON:
        case VIDIOC_STREAMOFF:
            if (debug_on)
                dbg("ioctl nr=%lu on secondary handle -> EBUSY\n", _IOC_NR(request));
            errno = EBUSY;
            return -1;
        default:
            break;
        }
    }

    /* DQBUF does its own locking because it blocks. */
    if (request == VIDIOC_DQBUF) {
        SHIM_ENTER();
        int r = do_ioctl(request, arg);
        SHIM_LEAVE();
        if (debug_on > 1)
            dbg("ioctl DQBUF -> %d%s\n", r, r < 0 ? " (error)" : "");
        return r;
    }

    SHIM_ENTER();
    LOCK();
    int r = do_ioctl(request, arg);
    UNLOCK();
    SHIM_LEAVE();

    /* Per-call tracing, because the interesting failures are all of the form
       "the application asked something we answered badly and then gave up".
       Without this the only visible evidence is an open() followed by a
       close() with nothing in between. */
    if (debug_on && (debug_on > 1 || request != VIDIOC_QBUF))
        dbg("ioctl nr=%lu size=%lu -> %d%s\n", _IOC_NR(request),
            (unsigned long)_IOC_SIZE(request), r,
            r < 0 ? (errno == ENOTTY ? " (ENOTTY)" : " (error)") : "");
    return r;
}

static void fake_stat(struct stat *st)
{
    memset(st, 0, sizeof *st);
    st->st_mode = S_IFCHR | 0666;
    st->st_rdev = makedev(81, 0);
    st->st_nlink = 1;
    st->st_uid = getuid();
    st->st_gid = getgid();
}

int stat(const char *path, struct stat *st)
{
    if (is_our_path(path)) { fake_stat(st); return 0; }
    if (!real_stat) return (int)syscall(SYS_newfstatat, AT_FDCWD, path, st, 0);
    return real_stat(path, st);
}

int access(const char *path, int mode)
{
    if (is_our_path(path)) return 0;
    if (!real_access) return (int)syscall(SYS_faccessat, AT_FDCWD, path, mode);
    return real_access(path, mode);
}

/*
 * Older binaries call __xstat rather than stat. glibc 2.33+ exports stat
 * directly, but interposing both costs nothing and covers anything built
 * against an older toolchain.
 */
int __xstat(int ver, const char *path, struct stat *st)
{
    if (is_our_path(path)) { fake_stat(st); return 0; }
    static int (*real__xstat)(int, const char *, struct stat *);
    if (!real__xstat) real__xstat = dlsym(RTLD_NEXT, "__xstat");
    if (real__xstat) return real__xstat(ver, path, st);
    return real_stat(path, st);
}

/*
 * stat() is only one of several ways to ask the same question, and which one
 * a program uses is an artefact of how it was built. Coreutils' `ls` and
 * anything using glibc's newer interfaces go through statx(); GLib and
 * Chromium reach for fstatat(); scripts use faccessat(). A program that
 * checks before it opens would conclude the device is missing if any of
 * these were left uncovered, and would then never call open() at all.
 */
int lstat(const char *path, struct stat *st)
{
    if (is_our_path(path)) { fake_stat(st); return 0; }
    static int (*real_lstat)(const char *, struct stat *);
    if (!real_lstat) real_lstat = dlsym(RTLD_NEXT, "lstat");
    if (real_lstat) return real_lstat(path, st);
    return (int)syscall(SYS_newfstatat, AT_FDCWD, path, st, AT_SYMLINK_NOFOLLOW);
}

int __lxstat(int ver, const char *path, struct stat *st)
{
    if (is_our_path(path)) { fake_stat(st); return 0; }
    static int (*real__lxstat)(int, const char *, struct stat *);
    if (!real__lxstat) real__lxstat = dlsym(RTLD_NEXT, "__lxstat");
    if (real__lxstat) return real__lxstat(ver, path, st);
    return (int)syscall(SYS_newfstatat, AT_FDCWD, path, st, AT_SYMLINK_NOFOLLOW);
}

/* Absolute paths only: is_our_path() compares the whole string, which no
   relative lookup can match, so dirfd never needs to be considered. */
int fstatat(int dirfd, const char *path, struct stat *st, int flags)
{
    if (is_our_path(path)) { fake_stat(st); return 0; }
    static int (*real_fstatat)(int, const char *, struct stat *, int);
    if (!real_fstatat) real_fstatat = dlsym(RTLD_NEXT, "fstatat");
    if (real_fstatat) return real_fstatat(dirfd, path, st, flags);
    return (int)syscall(SYS_newfstatat, dirfd, path, st, flags);
}

int __fxstatat(int ver, int dirfd, const char *path, struct stat *st, int flags)
{
    if (is_our_path(path)) { fake_stat(st); return 0; }
    static int (*real__fxstatat)(int, int, const char *, struct stat *, int);
    if (!real__fxstatat) real__fxstatat = dlsym(RTLD_NEXT, "__fxstatat");
    if (real__fxstatat) return real__fxstatat(ver, dirfd, path, st, flags);
    return (int)syscall(SYS_newfstatat, dirfd, path, st, flags);
}

int statx(int dirfd, const char *path, int flags, unsigned int mask,
          struct statx *stx)
{
    if (is_our_path(path)) {
        struct stat st;
        fake_stat(&st);
        memset(stx, 0, sizeof *stx);
        /* Report only what was actually filled in, as the kernel does. */
        stx->stx_mask = STATX_TYPE | STATX_MODE | STATX_NLINK |
                        STATX_UID | STATX_GID;
        stx->stx_mode = (unsigned short)st.st_mode;
        stx->stx_nlink = st.st_nlink;
        stx->stx_uid = st.st_uid;
        stx->stx_gid = st.st_gid;
        stx->stx_rdev_major = major(st.st_rdev);
        stx->stx_rdev_minor = minor(st.st_rdev);
        stx->stx_blksize = 4096;
        return 0;
    }
    static int (*real_statx)(int, const char *, int, unsigned int,
                             struct statx *);
    if (!real_statx) real_statx = dlsym(RTLD_NEXT, "statx");
    if (real_statx) return real_statx(dirfd, path, flags, mask, stx);
    return (int)syscall(SYS_statx, dirfd, path, flags, mask, stx);
}

int faccessat(int dirfd, const char *path, int mode, int flags)
{
    if (is_our_path(path)) return 0;
    static int (*real_faccessat)(int, const char *, int, int);
    if (!real_faccessat) real_faccessat = dlsym(RTLD_NEXT, "faccessat");
    if (real_faccessat) return real_faccessat(dirfd, path, mode, flags);
    return (int)syscall(SYS_faccessat, dirfd, path, mode);
}

int euidaccess(const char *path, int mode)
{
    if (is_our_path(path)) return 0;
    static int (*real_euidaccess)(const char *, int);
    if (!real_euidaccess) real_euidaccess = dlsym(RTLD_NEXT, "euidaccess");
    if (real_euidaccess) return real_euidaccess(path, mode);
    return (int)syscall(SYS_faccessat, AT_FDCWD, path, mode);
}

/*
 * The same large-file trap that hid behind mmap64 applies to the whole stat
 * family: a program compiled with _FILE_OFFSET_BITS=64 -- which is most of
 * them, CPython and bash included -- calls stat64() rather than stat(), and
 * interposing only the latter leaves it talking to the real one. The symptom
 * is a plain "Permission denied" from a program that never reached open(),
 * which reads like a device-permissions problem and is not.
 *
 * On 64-bit Linux struct stat64 and struct stat have the same layout, so the
 * same filler serves both.
 */
int stat64(const char *path, struct stat64 *st)
{
    if (is_our_path(path)) { fake_stat((struct stat *)st); return 0; }
    static int (*real_stat64)(const char *, struct stat64 *);
    if (!real_stat64) real_stat64 = dlsym(RTLD_NEXT, "stat64");
    if (real_stat64) return real_stat64(path, st);
    return (int)syscall(SYS_newfstatat, AT_FDCWD, path, st, 0);
}

int lstat64(const char *path, struct stat64 *st)
{
    if (is_our_path(path)) { fake_stat((struct stat *)st); return 0; }
    static int (*real_lstat64)(const char *, struct stat64 *);
    if (!real_lstat64) real_lstat64 = dlsym(RTLD_NEXT, "lstat64");
    if (real_lstat64) return real_lstat64(path, st);
    return (int)syscall(SYS_newfstatat, AT_FDCWD, path, st, AT_SYMLINK_NOFOLLOW);
}

int fstatat64(int dirfd, const char *path, struct stat64 *st, int flags)
{
    if (is_our_path(path)) { fake_stat((struct stat *)st); return 0; }
    static int (*real_fstatat64)(int, const char *, struct stat64 *, int);
    if (!real_fstatat64) real_fstatat64 = dlsym(RTLD_NEXT, "fstatat64");
    if (real_fstatat64) return real_fstatat64(dirfd, path, st, flags);
    return (int)syscall(SYS_newfstatat, dirfd, path, st, flags);
}

int __xstat64(int ver, const char *path, struct stat64 *st)
{
    if (is_our_path(path)) { fake_stat((struct stat *)st); return 0; }
    static int (*real__xstat64)(int, const char *, struct stat64 *);
    if (!real__xstat64) real__xstat64 = dlsym(RTLD_NEXT, "__xstat64");
    if (real__xstat64) return real__xstat64(ver, path, st);
    return (int)syscall(SYS_newfstatat, AT_FDCWD, path, st, 0);
}

int __lxstat64(int ver, const char *path, struct stat64 *st)
{
    if (is_our_path(path)) { fake_stat((struct stat *)st); return 0; }
    static int (*real__lxstat64)(int, const char *, struct stat64 *);
    if (!real__lxstat64) real__lxstat64 = dlsym(RTLD_NEXT, "__lxstat64");
    if (real__lxstat64) return real__lxstat64(ver, path, st);
    return (int)syscall(SYS_newfstatat, AT_FDCWD, path, st, AT_SYMLINK_NOFOLLOW);
}

int __fxstatat64(int ver, int dirfd, const char *path, struct stat64 *st,
                 int flags)
{
    if (is_our_path(path)) { fake_stat((struct stat *)st); return 0; }
    static int (*real__fxstatat64)(int, int, const char *, struct stat64 *,
                                   int);
    if (!real__fxstatat64)
        real__fxstatat64 = dlsym(RTLD_NEXT, "__fxstatat64");
    if (real__fxstatat64)
        return real__fxstatat64(ver, dirfd, path, st, flags);
    return (int)syscall(SYS_newfstatat, dirfd, path, st, flags);
}

/* ---------- the raw-syscall path ----------
 *
 * Interposing libc is not enough on its own, and libv4l2 is the proof.
 *
 * libv4l2 is the userspace conversion layer that VLC, cheese, guvcview and
 * most GTK camera applications go through. It deliberately does *not* call
 * libc's open/ioctl/read/mmap: its private header defines SYS_IOCTL and
 * friends as direct syscall() invocations, because libv4l2 also ships
 * v4l2convert.so, which itself interposes those very symbols -- so calling
 * them would make it recurse into itself.
 *
 * The consequence here was a camera that ffmpeg and Chromium could both use
 * and VLC could not, failing at the first VIDIOC_QUERYCAP with EACCES -- the
 * kernel's answer to an ioctl on the pipe that backs our handle, because the
 * call never reached us at all. Nothing in the shim was wrong; it simply was
 * not being asked.
 *
 * syscall() is itself an ordinary libc function, so interposing it catches
 * that traffic and routes it back through the same implementations. The
 * passthrough deliberately uses inline asm rather than dlsym(RTLD_NEXT),
 * because syscall() can be called before the constructor has run and must
 * never depend on the dynamic linker having got there first.
 */
#if defined(__aarch64__)
static long raw_syscall(long n, long a, long b, long c, long d, long e, long f)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x5 __asm__("x5") = f;
    __asm__ volatile ("svc #0"
                      : "+r"(x0)
                      : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                      : "memory", "cc");
    return x0;
}

long syscall(long number, ...)
{
    va_list ap;
    va_start(ap, number);
    long a = va_arg(ap, long), b = va_arg(ap, long), c = va_arg(ap, long);
    long d = va_arg(ap, long), e = va_arg(ap, long), f = va_arg(ap, long);
    va_end(ap);

    /* SHIM_BUSY keeps our own internal syscall() fallbacks -- and anything
     * the allocator does underneath us -- on the direct path. */
    if (inited && !SHIM_BUSY()) {
        switch (number) {
        case SYS_ioctl:
            if (is_our_fd((int)a)) return ioctl((int)a, (unsigned long)b, (void *)c);
            break;
        case SYS_read:
            if (is_our_fd((int)a)) return read((int)a, (void *)b, (size_t)c);
            break;
        case SYS_close:
            if (is_our_fd((int)a)) return close((int)a);
            break;
        case SYS_mmap:
            if (is_our_fd((int)e)) {
                void *p = mmap((void *)a, (size_t)b, (int)c, (int)d, (int)e, (off_t)f);
                return (p == MAP_FAILED) ? -1 : (long)p;
            }
            break;
        case SYS_munmap:
            /* Only worth diverting while we have buffers that are not real
             * mappings; otherwise leave the hot path alone. */
            if (C.nbufs > 0) return munmap((void *)a, (size_t)b);
            break;
        case SYS_openat:
            if (is_our_path((const char *)b)) return openat((int)a, (const char *)b, (int)c, (mode_t)d);
            break;
        default:
            break;
        }
    }

    long r = raw_syscall(number, a, b, c, d, e, f);
    if (r < 0 && r > -4096) { errno = (int)-r; return -1; }
    return r;
}
#endif /* __aarch64__ */
