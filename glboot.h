// Shared headless-EGL + GL 4.3 compute bootstrap for the AGC codec tools.
// Creates a surfaceless context (no window, no swapchain) and resolves every
// GL entry point at runtime, since the system headers declare real prototypes.
#ifndef GLBOOT_H
#define GLBOOT_H

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now_s(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

#define GLFN(t, n) static t p_##n;
GLFN(PFNGLCREATESHADERPROC, glCreateShader)
GLFN(PFNGLSHADERSOURCEPROC, glShaderSource)
GLFN(PFNGLCOMPILESHADERPROC, glCompileShader)
GLFN(PFNGLGETSHADERIVPROC, glGetShaderiv)
GLFN(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)
GLFN(PFNGLDELETESHADERPROC, glDeleteShader)
GLFN(PFNGLCREATEPROGRAMPROC, glCreateProgram)
GLFN(PFNGLATTACHSHADERPROC, glAttachShader)
GLFN(PFNGLLINKPROGRAMPROC, glLinkProgram)
GLFN(PFNGLGETPROGRAMIVPROC, glGetProgramiv)
GLFN(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)
GLFN(PFNGLUSEPROGRAMPROC, glUseProgram)
GLFN(PFNGLGENBUFFERSPROC, glGenBuffers)
GLFN(PFNGLDELETEBUFFERSPROC, glDeleteBuffers)
GLFN(PFNGLBINDBUFFERPROC, glBindBuffer)
GLFN(PFNGLBUFFERDATAPROC, glBufferData)
GLFN(PFNGLBUFFERSUBDATAPROC, glBufferSubData)
GLFN(PFNGLBINDBUFFERBASEPROC, glBindBufferBase)
GLFN(PFNGLGETBUFFERSUBDATAPROC, glGetBufferSubData)
GLFN(PFNGLDISPATCHCOMPUTEPROC, glDispatchCompute)
GLFN(PFNGLMEMORYBARRIERPROC, glMemoryBarrier)
GLFN(PFNGLFINISHPROC, glFinish)
GLFN(PFNGLGETSTRINGPROC, glGetString)
GLFN(PFNGLGETERRORPROC, glGetError)
GLFN(PFNGLUNIFORM1IPROC, glUniform1i)
GLFN(PFNGLUNIFORM1UIPROC, glUniform1ui)
GLFN(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)
GLFN(PFNGLGETINTEGERI_VPROC, glGetIntegeri_v)
GLFN(PFNGLCLEARBUFFERDATAPROC, glClearBufferData)
GLFN(PFNGLCLEARBUFFERSUBDATAPROC, glClearBufferSubData)

#define glCreateShader       p_glCreateShader
#define glShaderSource       p_glShaderSource
#define glCompileShader      p_glCompileShader
#define glGetShaderiv        p_glGetShaderiv
#define glGetShaderInfoLog   p_glGetShaderInfoLog
#define glDeleteShader       p_glDeleteShader
#define glCreateProgram      p_glCreateProgram
#define glAttachShader       p_glAttachShader
#define glLinkProgram        p_glLinkProgram
#define glGetProgramiv       p_glGetProgramiv
#define glGetProgramInfoLog  p_glGetProgramInfoLog
#define glUseProgram         p_glUseProgram
#define glGenBuffers         p_glGenBuffers
#define glDeleteBuffers      p_glDeleteBuffers
#define glBindBuffer         p_glBindBuffer
#define glBufferData         p_glBufferData
#define glBufferSubData      p_glBufferSubData
#define glBindBufferBase     p_glBindBufferBase
#define glGetBufferSubData   p_glGetBufferSubData
#define glDispatchCompute    p_glDispatchCompute
#define glMemoryBarrier      p_glMemoryBarrier
#define glFinish             p_glFinish
#define glGetString          p_glGetString
#define glGetError           p_glGetError
#define glUniform1i          p_glUniform1i
#define glUniform1ui         p_glUniform1ui
#define glGetUniformLocation p_glGetUniformLocation
#define glGetIntegeri_v      p_glGetIntegeri_v
#define glClearBufferData    p_glClearBufferData
#define glClearBufferSubData p_glClearBufferSubData

static void gl_check(const char *where) {
    GLenum e = p_glGetError();
    if (e != GL_NO_ERROR) fprintf(stderr, "  !! GL error 0x%04x at %s\n", e, where);
}

static GLuint gl_build(const char *src, const char *name) {
    GLuint sh = p_glCreateShader(GL_COMPUTE_SHADER);
    p_glShaderSource(sh, 1, &src, NULL);
    p_glCompileShader(sh);
    GLint ok = 0; p_glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[8192]; p_glGetShaderInfoLog(sh, 8192, NULL, log);
        fprintf(stderr, "compile failed (%s):\n%s\n", name, log); exit(1); }
    GLuint p = p_glCreateProgram();
    p_glAttachShader(p, sh); p_glLinkProgram(p);
    p_glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[8192]; p_glGetProgramInfoLog(p, 8192, NULL, log);
        fprintf(stderr, "link failed (%s):\n%s\n", name, log); exit(1); }
    p_glDeleteShader(sh);
    return p;
}

// Brings up a surfaceless GL 4.3 core context on Turnip via Zink.
static void gl_boot(int verbose) {
    PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatDisp =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay dpy = getPlatDisp
        ? getPlatDisp(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL)
        : eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY) { fprintf(stderr, "no EGL display\n"); exit(1); }
    EGLint maj, min;
    if (!eglInitialize(dpy, &maj, &min)) { fprintf(stderr, "eglInitialize failed\n"); exit(1); }
    eglBindAPI(EGL_OPENGL_API);
    EGLint cfgAttr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                         EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE };
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(dpy, cfgAttr, &cfg, 1, &n) || n < 1) {
        fprintf(stderr, "no EGL config\n"); exit(1); }
    EGLint ctxAttr[] = { EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 3,
                         EGL_CONTEXT_OPENGL_PROFILE_MASK,
                         EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttr);
    if (ctx == EGL_NO_CONTEXT) { fprintf(stderr, "no GL 4.3 context\n"); exit(1); }
    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        fprintf(stderr, "eglMakeCurrent failed\n"); exit(1); }

#define L(n) p_##n = (void *)eglGetProcAddress(#n); \
    if (!p_##n) { fprintf(stderr, "missing GL entry point %s\n", #n); exit(1); }
    L(glCreateShader) L(glShaderSource) L(glCompileShader) L(glGetShaderiv)
    L(glGetShaderInfoLog) L(glDeleteShader) L(glCreateProgram) L(glAttachShader)
    L(glLinkProgram) L(glGetProgramiv) L(glGetProgramInfoLog) L(glUseProgram)
    L(glGenBuffers) L(glBindBuffer) L(glBufferData) L(glBufferSubData)
    L(glBindBufferBase) L(glGetBufferSubData) L(glDispatchCompute)
    L(glDeleteBuffers)
    L(glMemoryBarrier) L(glFinish) L(glGetString) L(glGetError)
    L(glUniform1i) L(glUniform1ui) L(glGetUniformLocation) L(glGetIntegeri_v)
    L(glClearBufferData)
    L(glClearBufferSubData)
#undef L

    if (verbose) {
        printf("    GPU: %s\n", p_glGetString(GL_RENDERER));
        printf("    GL : %s\n", p_glGetString(GL_VERSION));
    }
}

#endif
