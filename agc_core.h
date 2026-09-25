// AGC-1 embeddable core: the GPU compute pipeline with the CLI stripped out.
//
// This is the same codec as agc.c, hardened for use *inside* another
// process's address space rather than as a short-lived CLI run:
//
//   * No exit() on failure. agc.c can afford to die on a shader compile
//     error because it is the whole program; a library linked into ffmpeg,
//     Shotcut or Blender must return an error code instead of taking the
//     host process down with it.
//   * No SIGINT/SIGTERM handlers. Installing one here would steal signal
//     delivery from whatever host application embeds this.
//   * The EGL context is acquired and released around each call
//     (agc_gl_lock/agc_gl_unlock) rather than left current on whatever
//     thread happens to boot it. A host application very likely has its own
//     GL/EGL context on the thread that calls into this codec (Shotcut and
//     Blender both link the same system libavcodec.so this heads into), and
//     leaving a foreign context current on that thread would corrupt the
//     host's own rendering the moment it made its next GL call.
//
// Deliberately duplicated rather than shared with agc.c: agc.c is the
// tested, working CLI tool, and this header changes the failure-handling
// contract throughout (return codes instead of exit()). Refactoring agc.c to
// share this file is future work (see README.md, "Embedding AGC-1"); until
// that happens, this is the reference for anyone embedding the codec, and
// agc.c is left exactly as it was.
//
// The compression pipeline itself -- shaders, bitstream, tables -- is
// unchanged and is documented in agc.c's own header comment.
#ifndef AGC_CORE_H
#define AGC_CORE_H

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GLFN(t, n) static t agc_p_##n;
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

#define glCreateShader       agc_p_glCreateShader
#define glShaderSource       agc_p_glShaderSource
#define glCompileShader      agc_p_glCompileShader
#define glGetShaderiv        agc_p_glGetShaderiv
#define glGetShaderInfoLog   agc_p_glGetShaderInfoLog
#define glDeleteShader       agc_p_glDeleteShader
#define glCreateProgram      agc_p_glCreateProgram
#define glAttachShader       agc_p_glAttachShader
#define glLinkProgram        agc_p_glLinkProgram
#define glGetProgramiv       agc_p_glGetProgramiv
#define glGetProgramInfoLog  agc_p_glGetProgramInfoLog
#define glUseProgram         agc_p_glUseProgram
#define glGenBuffers         agc_p_glGenBuffers
#define glDeleteBuffers      agc_p_glDeleteBuffers
#define glBindBuffer         agc_p_glBindBuffer
#define glBufferData         agc_p_glBufferData
#define glBufferSubData      agc_p_glBufferSubData
#define glBindBufferBase     agc_p_glBindBufferBase
#define glGetBufferSubData   agc_p_glGetBufferSubData
#define glDispatchCompute    agc_p_glDispatchCompute
#define glMemoryBarrier      agc_p_glMemoryBarrier
#define glFinish             agc_p_glFinish
#define glGetString          agc_p_glGetString
#define glGetError           agc_p_glGetError
#define glUniform1i          agc_p_glUniform1i
#define glUniform1ui         agc_p_glUniform1ui
#define glGetUniformLocation agc_p_glGetUniformLocation
#define glGetIntegeri_v      agc_p_glGetIntegeri_v
#define glClearBufferData    agc_p_glClearBufferData
#define glClearBufferSubData agc_p_glClearBufferSubData

static __attribute__((unused)) double agc_now_s(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static __attribute__((unused)) void agc_gl_check(const char *where) {
    GLenum e = agc_p_glGetError();
    if (e != GL_NO_ERROR) fprintf(stderr, "agc_core: GL error 0x%04x at %s\n", e, where);
}

// Returns 0 on failure instead of exiting -- this runs inside a host process.
static __attribute__((unused)) GLuint agc_gl_build(const char *src, const char *name, char *err, size_t errlen) {
    GLuint sh = agc_p_glCreateShader(GL_COMPUTE_SHADER);
    agc_p_glShaderSource(sh, 1, &src, NULL);
    agc_p_glCompileShader(sh);
    GLint ok = 0; agc_p_glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        if (err) agc_p_glGetShaderInfoLog(sh, (GLsizei)errlen, NULL, err);
        fprintf(stderr, "agc_core: shader compile failed (%s): %s\n", name, err ? err : "?");
        agc_p_glDeleteShader(sh);
        return 0;
    }
    GLuint p = agc_p_glCreateProgram();
    agc_p_glAttachShader(p, sh); agc_p_glLinkProgram(p);
    agc_p_glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        if (err) agc_p_glGetProgramInfoLog(p, (GLsizei)errlen, NULL, err);
        fprintf(stderr, "agc_core: shader link failed (%s): %s\n", name, err ? err : "?");
        agc_p_glDeleteShader(sh);
        return 0;
    }
    agc_p_glDeleteShader(sh);
    return p;
}

// Process-wide headless GL state. One boot per process, guarded by a mutex;
// every encoder/decoder instance in the process shares it, the same way
// every Plane/Progs pair in the CLI tool shares the one context main() boots.
static __attribute__((unused)) pthread_mutex_t agc_gl_mu = PTHREAD_MUTEX_INITIALIZER;
static __attribute__((unused)) int agc_gl_booted = 0;
static __attribute__((unused)) EGLDisplay agc_gl_dpy;
static __attribute__((unused)) EGLContext agc_gl_ctx;

// Call before touching any GL/agc_* function; pairs with agc_gl_unlock().
// Safe to call from any thread and to nest per encoder/decoder instance --
// it is a plain mutex, not a recursive one, so do not call it twice on the
// same thread without unlocking in between.
static __attribute__((unused)) int agc_gl_lock(char *err, size_t errlen) {
    pthread_mutex_lock(&agc_gl_mu);
    if (!agc_gl_booted) {
        PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatDisp =
            (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
        agc_gl_dpy = getPlatDisp
            ? getPlatDisp(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL)
            : eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (agc_gl_dpy == EGL_NO_DISPLAY) {
            snprintf(err, errlen, "no EGL display"); pthread_mutex_unlock(&agc_gl_mu); return -1;
        }
        EGLint maj, min;
        if (!eglInitialize(agc_gl_dpy, &maj, &min)) {
            snprintf(err, errlen, "eglInitialize failed"); pthread_mutex_unlock(&agc_gl_mu); return -1;
        }
        eglBindAPI(EGL_OPENGL_API);
        EGLint cfgAttr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                             EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE };
        EGLConfig cfg; EGLint n = 0;
        if (!eglChooseConfig(agc_gl_dpy, cfgAttr, &cfg, 1, &n) || n < 1) {
            snprintf(err, errlen, "no EGL config"); pthread_mutex_unlock(&agc_gl_mu); return -1;
        }
        EGLint ctxAttr[] = { EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 3,
                             EGL_CONTEXT_OPENGL_PROFILE_MASK,
                             EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE };
        agc_gl_ctx = eglCreateContext(agc_gl_dpy, cfg, EGL_NO_CONTEXT, ctxAttr);
        if (agc_gl_ctx == EGL_NO_CONTEXT) {
            snprintf(err, errlen, "no GL 4.3 context (needs Turnip+Zink; see gpu-env.sh)");
            pthread_mutex_unlock(&agc_gl_mu); return -1;
        }
        agc_gl_booted = 1;
    }
    if (!eglMakeCurrent(agc_gl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, agc_gl_ctx)) {
        snprintf(err, errlen, "eglMakeCurrent failed"); pthread_mutex_unlock(&agc_gl_mu); return -1;
    }
#define L(n) agc_p_##n = (void *)eglGetProcAddress(#n); \
    if (!agc_p_##n) { snprintf(err, errlen, "missing GL entry point %s", #n); \
        eglMakeCurrent(agc_gl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); \
        pthread_mutex_unlock(&agc_gl_mu); return -1; }
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
    return 0;
}

// Releases the context from this thread -- deliberately, so a host
// application's own GL/EGL state on the same thread is never left pointing
// at ours between calls. Keeps the mutex held only for the duration of one
// batch of GL work; call agc_gl_unlock() as soon as that work is done.
static __attribute__((unused)) void agc_gl_unlock(void) {
    eglMakeCurrent(agc_gl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    pthread_mutex_unlock(&agc_gl_mu);
}

// ------------------------------------------------------------ tables/prelude
#define AGC_VERSION   1
#define AGC_GRAY8     0
#define AGC_YUV420P   1
#define TILE_LOG      5
#define TILE_BLOCKS   (1 << TILE_LOG)
#define MAX_BLOCK_BITS (1 + 6 + 64 + 64 * 29)

static __attribute__((unused)) const int AGC_ZZ[64] = {
     0, 1, 8,16, 9, 2, 3,10,
    17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,
    27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,
    29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,
    53,60,61,54,47,55,62,63
};
static __attribute__((unused)) const int AGC_QT[64] = {
    16,11,10,16,24,40,51,61,
    12,12,14,19,26,58,60,55,
    14,13,16,24,40,57,69,56,
    14,17,22,29,51,87,80,62,
    18,22,37,56,68,109,103,77,
    24,35,55,64,81,104,113,92,
    49,64,78,87,103,121,120,101,
    72,92,95,98,112,100,103,99
};
static __attribute__((unused)) int agc_IZZ[64];
static __attribute__((unused)) char *agc_prelude;

// Returns 0 on success. The table is a fixed constant checked once at boot;
// failure here means a code edit broke the permutation, not a runtime
// condition, but we still return an error rather than exit() since we are a
// guest inside someone else's process.
static __attribute__((unused)) int agc_build_prelude(char *err, size_t errlen) {
    for (int i = 0; i < 64; ++i) agc_IZZ[i] = -1;
    for (int i = 0; i < 64; ++i) agc_IZZ[AGC_ZZ[i]] = i;
    for (int i = 0; i < 64; ++i)
        if (agc_IZZ[i] < 0 || AGC_ZZ[agc_IZZ[i]] != i) {
            snprintf(err, errlen, "zigzag table is not a valid permutation at %d", i);
            return -1;
        }
    char *s = malloc(8192); int n = 0;
    n += sprintf(s + n, "const int ZZ[64] = int[64](");
    for (int i = 0; i < 64; ++i) n += sprintf(s + n, "%d%s", AGC_ZZ[i], i == 63 ? ");\n" : ",");
    n += sprintf(s + n, "const int IZZ[64] = int[64](");
    for (int i = 0; i < 64; ++i) n += sprintf(s + n, "%d%s", agc_IZZ[i], i == 63 ? ");\n" : ",");
    n += sprintf(s + n, "const int QT[64] = int[64](");
    for (int i = 0; i < 64; ++i) n += sprintf(s + n, "%d%s", AGC_QT[i], i == 63 ? ");\n" : ",");
    n += sprintf(s + n, "int qval(int i, int s){ return clamp((QT[i]*s + 50)/100, 1, 255); }\n");
    agc_prelude = s;
    return 0;
}

// Returns 0 (an invalid GL program name) on failure instead of exiting.
static __attribute__((unused)) GLuint agc_build_shader(const char *body, const char *name, char *err, size_t errlen) {
    size_t len = strlen(agc_prelude) + strlen(body) + 64;
    char *src = malloc(len);
    snprintf(src, len, "#version 430\n%s%s", agc_prelude, body);
    GLuint p = agc_gl_build(src, name, err, errlen);
    free(src);
    return p;
}

// ---------------------------------------------------------------- shaders
// Verbatim from agc.c -- the compute pipeline itself is unchanged.
static __attribute__((unused)) const char *SRC_ANALYZE =
"layout(local_size_x=8, local_size_y=8) in;\n"
"layout(std430,binding=0) readonly  buffer Src  { uint px[]; };\n"
"layout(std430,binding=1) writeonly buffer Coef { int  co[]; };\n"
"layout(std430,binding=2) writeonly buffer Meta { uint meta[]; };\n"
"layout(std430,binding=3) writeonly buffer Word { uint words[]; };\n"
"uniform int uBlocksX; uniform int uWidth; uniform int uQScale;\n"
"shared float s[64];\n"
"shared int sLast[64]; shared int sCnt[64];\n"
"const float C0 = 0.35355339059;\n"
"float cf(int u){ return u==0 ? C0 : 0.5; }\n"
"void main(){\n"
"  int lx=int(gl_LocalInvocationID.x), ly=int(gl_LocalInvocationID.y);\n"
"  int gx=int(gl_WorkGroupID.x)*8+lx, gy=int(gl_WorkGroupID.y)*8+ly;\n"
"  uint w = px[(gy*uWidth+gx)>>2];\n"
"  s[ly*8+lx] = float((w >> (uint(gx&3)*8u)) & 0xFFu) - 128.0;\n"
"  barrier();\n"
"  float a=0.0;\n"
"  for(int k=0;k<8;++k) a += s[ly*8+k]*cos(3.14159265*(2.0*float(k)+1.0)*float(lx)/16.0);\n"
"  a *= cf(lx);\n"
"  barrier(); s[ly*8+lx]=a; barrier();\n"
"  float b=0.0;\n"
"  for(int k=0;k<8;++k) b += s[k*8+lx]*cos(3.14159265*(2.0*float(k)+1.0)*float(ly)/16.0);\n"
"  b *= cf(ly);\n"
"  int idx = ly*8+lx;\n"
"  int qv = int(round(b / float(qval(idx, uQScale))));\n"
"  uint blk = gl_WorkGroupID.y*uint(uBlocksX) + gl_WorkGroupID.x;\n"
"  co[blk*64u + uint(idx)] = qv;\n"
"  // Block metadata in a single parallel reduction: a coefficient's cost now\n"
"  // depends only on its own magnitude, so nothing has to wait on a block-wide\n"
"  // maximum and the second reduction disappears.\n"
"  sLast[idx] = (qv != 0) ? IZZ[idx] : -1;\n"
"  int cost = 0;\n"
"  if (qv != 0) {\n"
"    int m = abs(qv); uint sz = 1u;\n"
"    while ((1 << sz) <= m && sz < 15u) sz++;\n"
"    cost = (sz < 15u) ? int(2u*sz) : 29;\n"
"  }\n"
"  sCnt[idx] = cost;\n"
"  barrier();\n"
"  for (int st=32; st>0; st>>=1) {\n"
"    if (idx < st) {\n"
"      sLast[idx] = max(sLast[idx], sLast[idx+st]);\n"
"      sCnt[idx] += sCnt[idx+st];\n"
"    }\n"
"    barrier();\n"
"  }\n"
"  if (idx == 0) {\n"
"    int last = sLast[0];\n"
"    uint bits = (last < 0) ? 1u : 1u + 6u + uint(last+1) + uint(sCnt[0]);\n"
"    meta[blk] = (bits << 12) | uint(last & 0xFF);\n"
"    words[blk] = bits;\n"
"  }\n"
"}\n";

// Pass 2: exclusive prefix sum inside each group of 256, plus group totals.
static __attribute__((unused)) const char *SRC_SCAN1 =
"layout(local_size_x=256) in;\n"
"layout(std430,binding=0) readonly  buffer W { uint words[]; };\n"
"layout(std430,binding=1) writeonly buffer O { uint off[]; };\n"
"layout(std430,binding=2) writeonly buffer G { uint gsum[]; };\n"
"uniform int uN;\n"
"shared uint t[256];\n"
"void main(){\n"
"  uint i = gl_GlobalInvocationID.x; uint l = gl_LocalInvocationID.x;\n"
"  uint v = (i < uint(uN)) ? words[i] : 0u;\n"
"  t[l] = v;\n"
"  barrier();\n"
"  for (uint d=1u; d<256u; d<<=1u){\n"
"    uint x = (l>=d) ? t[l-d] : 0u; barrier(); t[l]+=x; barrier();\n"
"  }\n"
"  if (i < uint(uN)) off[i] = t[l] - v;\n"
"  if (l == 255u) gsum[gl_WorkGroupID.x] = t[l];\n"
"}\n";

// Pass 3: exclusive prefix sum across group totals. A single workgroup walks
// the array in chunks of 1024 carrying a running base, so this handles any
// number of groups rather than silently capping at 1024.
static __attribute__((unused)) const char *SRC_SCAN2 =
"layout(local_size_x=1024) in;\n"
"layout(std430,binding=0) readonly  buffer G { uint gsum[]; };\n"
"layout(std430,binding=1) writeonly buffer H { uint goff[]; };\n"
"uniform int uG;\n"
"shared uint t[1024];\n"
"shared uint base;\n"
"void main(){\n"
"  uint l = gl_LocalInvocationID.x;\n"
"  if (l == 0u) base = 0u;\n"
"  barrier();\n"
"  for (uint c = 0u; c < uint(uG); c += 1024u) {\n"
"    uint i = c + l;\n"
"    uint v = (i < uint(uG)) ? gsum[i] : 0u;\n"
"    t[l] = v;\n"
"    barrier();\n"
"    for (uint d=1u; d<1024u; d<<=1u){\n"
"      uint x = (l>=d) ? t[l-d] : 0u; barrier(); t[l]+=x; barrier();\n"
"    }\n"
"    uint incl = t[l], tot = t[1023];\n"
"    if (i < uint(uG)) goff[i] = base + incl - v;\n"
"    barrier();\n"
"    if (l == 0u) base += tot;\n"
"    barrier();\n"
"  }\n"
"}\n";

// Pass 4: total the bit lengths of the blocks in each tile. These counts are
// the only index the container ships; everything else is rebuilt at decode.
static __attribute__((unused)) const char *SRC_TILESUM =
"layout(local_size_x=64) in;\n"
"layout(std430,binding=0) readonly  buffer W { uint words[]; };\n"
"layout(std430,binding=1) writeonly buffer T { uint tbits[]; };\n"
"uniform int uN; uniform int uT; uniform int uK;\n"
"void main(){\n"
"  uint t = gl_GlobalInvocationID.x;\n"
"  if (t >= uint(uT)) return;\n"
"  uint b0 = t * uint(uK), s = 0u;\n"
"  for (uint j = 0u; j < uint(uK); ++j) {\n"
"    uint b = b0 + j;\n"
"    if (b < uint(uN)) s += words[b];\n"
"  }\n"
"  tbits[t] = s;\n"
"}\n";

// Pass 5: pack each block's bits at its final offset. One thread per block,
// so the 64 threads of a group cover 64 independent blocks.
static __attribute__((unused)) const char *SRC_PACK =
"layout(local_size_x=64) in;\n"
"layout(std430,binding=0) readonly  buffer Coef { int  co[]; };\n"
"layout(std430,binding=1) readonly  buffer Meta { uint meta[]; };\n"
"layout(std430,binding=2) readonly  buffer O    { uint off[]; };\n"
"layout(std430,binding=3) readonly  buffer H    { uint goff[]; };\n"
"layout(std430,binding=4) coherent  buffer BS   { uint bs[]; };\n"
"uniform int uN;\n"
"uint W_word, W_cur, W_bits;\n"
"// Blocks start at exact bit offsets, so the first and last word of a block\n"
"// are shared with its neighbours. atomicOr into a pre-cleared buffer makes\n"
"// that overlap safe without imposing any ordering between blocks.\n"
"void putInit(uint bitbase){ W_word=bitbase>>5u; W_bits=bitbase&31u; W_cur=0u; }\n"
"// writes up to 16 bits in constant time rather than bit by bit\n"
"void put(uint v, uint n){\n"
"  if (n == 0u) return;\n"
"  uint space = 32u - W_bits;\n"
"  v &= (1u<<n) - 1u;\n"
"  if (n <= space) {\n"
"    W_cur |= v << (space - n);\n"
"    W_bits += n;\n"
"    if (W_bits == 32u){ atomicOr(bs[W_word], W_cur); W_word++; W_cur=0u; W_bits=0u; }\n"
"  } else {\n"
"    uint hi = n - space;\n"
"    W_cur |= v >> hi;\n"
"    atomicOr(bs[W_word], W_cur); W_word++;\n"
"    W_cur = (v & ((1u<<hi)-1u)) << (32u - hi);\n"
"    W_bits = hi;\n"
"  }\n"
"}\n"
"void putFlush(){ if (W_bits>0u) atomicOr(bs[W_word], W_cur); }\n"
"void main(){\n"
"  uint blk = gl_GlobalInvocationID.x;\n"
"  if (blk >= uint(uN)) return;\n"
"  uint base = off[blk] + goff[blk>>8u];\n"
"  uint m = meta[blk]; int last = int(m & 0xFFu);\n"
"  putInit(base);\n"
"  put(last == 255 ? 0u : 1u, 1u);\n"
"  if (last != 255) {\n"
"    put(uint(last), 6u);\n"
"    // significance flags, batched 16 at a time\n"
"    uint acc=0u, nacc=0u;\n"
"    for (int i=0;i<=last;++i){\n"
"      acc = (acc<<1) | (co[blk*64u+uint(ZZ[i])]!=0 ? 1u:0u);\n"
"      nacc++;\n"
"      if (nacc==16u){ put(acc,16u); acc=0u; nacc=0u; }\n"
"    }\n"
"    if (nacc>0u) put(acc,nacc);\n"
"    // variable-length magnitudes: unary size prefix, then sign + low bits\n"
"    for (int i=0;i<=last;++i){\n"
"      int v = co[blk*64u+uint(ZZ[i])];\n"
"      if (v!=0){\n"
"        int m2 = abs(v); uint sz = 1u;\n"
"        while ((1 << sz) <= m2 && sz < 15u) sz++;\n"
"        uint k = sz - 1u;\n"
"        if (sz < 15u)      put(((1u<<k)-1u) << 1u, k+1u);\n"
"        else if (k > 0u)   put((1u<<k)-1u, k);\n"
"        uint sg = v<0 ? 1u:0u;\n"
"        put((sg<<k) | uint(m2 & ((1<<k)-1)), sz);\n"
"      }\n"
"    }\n"
"  }\n"
"  putFlush();\n"
"}\n";

// Pass 6a: parse the bitstream, one thread per tile. The thread seeks once to
// its tile's bit offset and then walks its 32 blocks in order -- each block's
// parse ends exactly where the next begins, so no per-block index is needed.
static __attribute__((unused)) const char *SRC_PARSE =
"layout(local_size_x=64) in;\n"
"layout(std430,binding=0) readonly  buffer BS   { uint bs[]; };\n"
"layout(std430,binding=1) readonly  buffer O    { uint off[]; };\n"
"layout(std430,binding=2) readonly  buffer H    { uint goff[]; };\n"
"layout(std430,binding=3) writeonly buffer Coef { int co[]; };\n"
"uniform int uN; uniform int uT; uniform int uK;\n"
"uint R_ptr, R_bits, R_cur;\n"
"void getInit(uint bitbase){\n"
"  R_ptr = bitbase>>5u; uint sh = bitbase & 31u;\n"
"  R_cur = bs[R_ptr] << sh; R_bits = 32u - sh; R_ptr++;\n"
"}\n"
"uint get(uint n){\n"
"  uint v=0u, need=n;\n"
"  while (need > 0u){\n"
"    if (R_bits==0u){ R_cur=bs[R_ptr]; R_ptr++; R_bits=32u; }\n"
"    uint take = min(need, R_bits);\n"
"    v = (v << take) | (R_cur >> (32u - take));\n"
"    R_cur = (take==32u) ? 0u : (R_cur << take);\n"
"    R_bits -= take; need -= take;\n"
"  }\n"
"  return v;\n"
"}\n"
"void parseBlock(uint blk){\n"
"  if (get(1u) == 0u) return;\n"
"  int last = int(get(6u));\n"
"  int n = last + 1;\n"
"  uint sigLo = 0u, sigHi = 0u;\n"
"  int rd = 0;\n"
"  while (rd < n) {\n"
"    uint chunk = uint(min(16, n - rd));\n"
"    uint c = get(chunk);\n"
"    sigHi = (sigHi << chunk) | (sigLo >> (32u - chunk));\n"
"    sigLo = (sigLo << chunk) | c;\n"
"    rd += int(chunk);\n"
"  }\n"
"  for (int i=0;i<n;++i){\n"
"    int pos = n - 1 - i;\n"
"    bool on = (pos < 32) ? (((sigLo >> uint(pos)) & 1u) != 0u)\n"
"                         : (((sigHi >> uint(pos-32)) & 1u) != 0u);\n"
"    if (on) {\n"
"      uint k = 0u;\n"
"      while (k < 14u) { if (get(1u)==0u) break; k++; }\n"
"      uint f = get(k+1u);\n"
"      int mag = int((1u<<k) | (f & ((1u<<k)-1u)));\n"
"      co[blk*64u+uint(ZZ[i])] = ((f >> k) != 0u) ? -mag : mag;\n"
"    }\n"
"  }\n"
"}\n"
"void main(){\n"
"  uint t = gl_GlobalInvocationID.x;\n"
"  if (t >= uint(uT)) return;\n"
"  getInit(off[t] + goff[t>>8u]);\n"
"  uint b0 = t * uint(uK);\n"
"  for (uint j = 0u; j < uint(uK); ++j) {\n"
"    uint blk = b0 + j;\n"
"    if (blk >= uint(uN)) break;\n"
"    parseBlock(blk);\n"
"  }\n"
"}\n";

// Pass 6b: dequantise and inverse-DCT, 64 threads per block. Results are
// assembled into whole 32-bit words in shared memory before the single store,
// so the output needs neither atomics nor a cleared destination.
static __attribute__((unused)) const char *SRC_IDCT =
"layout(local_size_x=8, local_size_y=8) in;\n"
"layout(std430,binding=0) readonly  buffer Coef { int co[]; };\n"
"layout(std430,binding=1) writeonly buffer R    { uint rec[]; };\n"
"uniform int uBlocksX; uniform int uWidth; uniform int uQScale;\n"
"shared float s[64];\n"
"shared uint  sv[64];\n"
"const float C0 = 0.35355339059;\n"
"float cf(int u){ return u==0 ? C0 : 0.5; }\n"
"void main(){\n"
"  int lx=int(gl_LocalInvocationID.x), ly=int(gl_LocalInvocationID.y);\n"
"  int idx = ly*8+lx;\n"
"  uint blk = gl_WorkGroupID.y*uint(uBlocksX) + gl_WorkGroupID.x;\n"
"  s[idx] = float(co[blk*64u+uint(idx)]) * float(qval(idx, uQScale));\n"
"  barrier();\n"
"  float a=0.0;\n"
"  for(int u=0;u<8;++u) a += cf(u)*s[ly*8+u]*cos(3.14159265*(2.0*float(lx)+1.0)*float(u)/16.0);\n"
"  barrier(); s[ly*8+lx]=a; barrier();\n"
"  float b=0.0;\n"
"  for(int v=0;v<8;++v) b += cf(v)*s[v*8+lx]*cos(3.14159265*(2.0*float(ly)+1.0)*float(v)/16.0);\n"
"  sv[idx] = uint(clamp(int(round(b + 128.0)), 0, 255));\n"
"  barrier();\n"
"  if (lx < 2) {\n"
"    int c = ly*8 + lx*4;\n"
"    uint w = sv[c] | (sv[c+1]<<8u) | (sv[c+2]<<16u) | (sv[c+3]<<24u);\n"
"    int gx0 = int(gl_WorkGroupID.x)*8 + lx*4;\n"
"    int gy  = int(gl_WorkGroupID.y)*8 + ly;\n"
"    rec[(gy*uWidth+gx0)>>2] = w;\n"
"  }\n"
"}\n";


// ------------------------------------------------------ pipeline (adapted)
// Same algorithm as agc.c's progs_init/plane_init/encode_plane/decode_plane;
// only the failure path changed (return -1 with a message in err, never exit).
typedef struct { GLuint analyze, scan1, scan2, tilesum, pack, parse, idct; } Progs;

enum { B_SRC, B_COEF, B_META, B_WORD, B_OFF, B_GSUM, B_GOFF,
       B_BS, B_REC, B_TBITS, B_TOFF, B_TGSUM, B_TGOFF, B_COUNT };

typedef struct {
    int rw, rh;                 // real dimensions
    int W, H;                   // padded up to a multiple of 8
    int bx, by, nBlk, nGroups;
    int nTiles, nTGroups;
    int nWordsPix;              // padded pixels / 4
    long bsBytes;               // capacity of the bitstream buffer
    GLuint b[B_COUNT];
} Plane;

static __attribute__((unused)) double agc_tPass[8];
static __attribute__((unused)) int agc_timing = 0;
static __attribute__((unused)) void agc_stage_sync(void) { if (agc_timing) glFinish(); }

static __attribute__((unused)) int agc_qscale_for(int quality) {
    int q = (quality < 50) ? (5000 / quality) : (200 - 2 * quality);
    return q < 1 ? 1 : q;
}

// Returns 0 on success, -1 if any shader failed to compile/link (err filled).
static __attribute__((unused)) int agc_progs_init(Progs *p, char *err, size_t errlen) {
    if (agc_build_prelude(err, errlen)) return -1;
    struct { GLuint *dst; const char *src; const char *name; } shaders[] = {
        { &p->analyze, SRC_ANALYZE, "analyze" }, { &p->scan1, SRC_SCAN1, "scan1" },
        { &p->scan2,   SRC_SCAN2,   "scan2" },   { &p->tilesum, SRC_TILESUM, "tilesum" },
        { &p->pack,    SRC_PACK,    "pack" },    { &p->parse, SRC_PARSE, "parse" },
        { &p->idct,    SRC_IDCT,    "idct" },
    };
    for (unsigned i = 0; i < sizeof(shaders)/sizeof(shaders[0]); ++i) {
        *shaders[i].dst = agc_build_shader(shaders[i].src, shaders[i].name, err, errlen);
        if (!*shaders[i].dst) return -1;
    }
    return 0;
}

static __attribute__((unused)) void agc_plane_init(Plane *pl, int rw, int rh) {
    memset(pl, 0, sizeof(*pl));
    pl->rw = rw; pl->rh = rh;
    pl->W = (rw + 7) & ~7;
    pl->H = (rh + 7) & ~7;
    pl->bx = pl->W / 8; pl->by = pl->H / 8;
    pl->nBlk = pl->bx * pl->by;
    pl->nGroups  = (pl->nBlk + 255) / 256;
    pl->nTiles   = (pl->nBlk + TILE_BLOCKS - 1) / TILE_BLOCKS;
    pl->nTGroups = (pl->nTiles + 255) / 256;
    pl->nWordsPix = (pl->W * pl->H) / 4;
    // Sized for the true worst case so pathological input cannot overrun it.
    pl->bsBytes = ((long)pl->nBlk * MAX_BLOCK_BITS + 7) / 8;
    pl->bsBytes = (pl->bsBytes + 63) & ~63L;

    glGenBuffers(B_COUNT, pl->b);
    struct { int i; long sz; } sizes[] = {
        { B_SRC,   (long)pl->nWordsPix * 4 },
        { B_COEF,  (long)pl->nBlk * 64 * 4 },
        { B_META,  (long)pl->nBlk * 4 },
        { B_WORD,  (long)pl->nBlk * 4 },
        { B_OFF,   (long)pl->nBlk * 4 },
        { B_GSUM,  (long)pl->nGroups * 4 },
        { B_GOFF,  (long)pl->nGroups * 4 },
        { B_BS,    pl->bsBytes },
        { B_REC,   (long)pl->nWordsPix * 4 },
        { B_TBITS, (long)pl->nTiles * 4 },
        { B_TOFF,  (long)pl->nTiles * 4 },
        { B_TGSUM, (long)pl->nTGroups * 4 },
        { B_TGOFF, (long)pl->nTGroups * 4 },
    };
    for (unsigned i = 0; i < sizeof(sizes)/sizeof(sizes[0]); ++i) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, pl->b[sizes[i].i]);
        glBufferData(GL_SHADER_STORAGE_BUFFER, sizes[i].sz, NULL, GL_DYNAMIC_COPY);
    }
    glFinish();
    agc_gl_check("plane buffers");
}

static __attribute__((unused)) void agc_plane_free(Plane *pl) { glDeleteBuffers(B_COUNT, pl->b); }

static __attribute__((unused)) void agc_clear_buf(GLuint buf) {
    unsigned zero = 0;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER,
                      GL_UNSIGNED_INT, &zero);
}

// Clears only the head of a buffer. The bitstream buffer is sized for the
// worst case, which is ~55x the typical stream, so clearing all of it would
// cost more than every other decode stage combined.
static __attribute__((unused)) void agc_clear_head(GLuint buf, long bytes, long cap) {
    if (bytes > cap) bytes = cap;
    bytes &= ~3L;
    if (bytes <= 0) return;
    unsigned zero = 0;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf);
    glClearBufferSubData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, 0, bytes,
                         GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
}

// Exclusive prefix sum of n elements of `in`, into `off` (+ `goff` per 256).
static __attribute__((unused)) void agc_prefix_sum(Progs *pg, GLuint in, GLuint off, GLuint gsum,
                       GLuint goff, int n, int nGroups) {
    glUseProgram(pg->scan1);
    glUniform1i(glGetUniformLocation(pg->scan1, "uN"), n);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, in);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, off);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, gsum);
    glDispatchCompute(nGroups, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    glUseProgram(pg->scan2);
    glUniform1i(glGetUniformLocation(pg->scan2, "uG"), nGroups);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, gsum);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, goff);
    glDispatchCompute(1, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
}

// ---------------------------------------------------------------- encode

typedef struct {
    unsigned *words;    // packed bitstream, 32-bit words
    int nWords;
    unsigned short *tileBits;
    int nTiles;
    long totalBits;
} Bits;

static __attribute__((unused)) void bits_free(Bits *bt) {
    free(bt->words); free(bt->tileBits);
    bt->words = NULL; bt->tileBits = NULL;
}

// `src` is W*H padded pixels. Returns 0 on success.
static __attribute__((unused)) int agc_encode_plane(Progs *pg, Plane *pl, const unsigned char *src,
                        int qscale, Bits *out) {
    double t0 = agc_now_s();
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, pl->b[B_SRC]);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (long)pl->nWordsPix * 4, src);
    agc_stage_sync();
    double tUp = agc_now_s();

    glUseProgram(pg->analyze);
    glUniform1i(glGetUniformLocation(pg->analyze, "uBlocksX"), pl->bx);
    glUniform1i(glGetUniformLocation(pg->analyze, "uWidth"), pl->W);
    glUniform1i(glGetUniformLocation(pg->analyze, "uQScale"), qscale);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, pl->b[B_SRC]);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, pl->b[B_COEF]);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, pl->b[B_META]);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, pl->b[B_WORD]);
    glDispatchCompute(pl->bx, pl->by, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT); agc_stage_sync();
    double tA = agc_now_s();

    agc_prefix_sum(pg, pl->b[B_WORD], pl->b[B_OFF], pl->b[B_GSUM], pl->b[B_GOFF],
               pl->nBlk, pl->nGroups);

    glUseProgram(pg->tilesum);
    glUniform1i(glGetUniformLocation(pg->tilesum, "uN"), pl->nBlk);
    glUniform1i(glGetUniformLocation(pg->tilesum, "uT"), pl->nTiles);
    glUniform1i(glGetUniformLocation(pg->tilesum, "uK"), TILE_BLOCKS);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, pl->b[B_WORD]);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, pl->b[B_TBITS]);
    glDispatchCompute((pl->nTiles + 63) / 64, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT); agc_stage_sync();

    // Read the tile index before packing. It is small, and knowing the exact
    // stream length lets the pack buffer be cleared over just the bytes that
    // will be written rather than its worst-case capacity.
    unsigned *tb = malloc((size_t)pl->nTiles * 4);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, pl->b[B_TBITS]);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (long)pl->nTiles * 4, tb);

    out->nTiles = pl->nTiles;
    out->tileBits = malloc((size_t)pl->nTiles * 2 + 2);
    long total = 0;
    for (int i = 0; i < pl->nTiles; ++i) {
        if (tb[i] > 0xFFFF) {   // cannot happen: 32 blocks * 1927 bits < 65536
            fprintf(stderr, "agc: tile %d overflows 16-bit length (%u)\n", i, tb[i]);
            free(tb); free(out->tileBits); out->tileBits = NULL; return -1;
        }
        out->tileBits[i] = (unsigned short)tb[i];
        total += tb[i];
    }
    free(tb);
    out->totalBits = total;
    out->nWords = (int)((total + 31) / 32);
    double tS = agc_now_s();

    agc_clear_head(pl->b[B_BS], (long)out->nWords * 4 + 64, pl->bsBytes);
    glUseProgram(pg->pack);
    glUniform1i(glGetUniformLocation(pg->pack, "uN"), pl->nBlk);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, pl->b[B_COEF]);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, pl->b[B_META]);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, pl->b[B_OFF]);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, pl->b[B_GOFF]);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, pl->b[B_BS]);
    glDispatchCompute((pl->nBlk + 63) / 64, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT); agc_stage_sync();
    double tP = agc_now_s();

    out->words = malloc((size_t)out->nWords * 4 + 4);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, pl->b[B_BS]);
    if (out->nWords)
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (long)out->nWords * 4, out->words);
    double tR = agc_now_s();

    agc_tPass[0] = tUp - t0;  agc_tPass[1] = tA - tUp; agc_tPass[2] = tS - tA;
    agc_tPass[3] = tP - tS;   agc_tPass[4] = tR - tP;  agc_tPass[5] = 0;
    agc_gl_check("encode");
    return 0;
}

// ---------------------------------------------------------------- decode

// Writes W*H padded pixels into `dst`.
static __attribute__((unused)) int agc_decode_plane(Progs *pg, Plane *pl, const Bits *in, int qscale,
                        unsigned char *dst) {
    if (in->nTiles != pl->nTiles) {
        fprintf(stderr, "agc: stream has %d tiles, geometry implies %d\n",
                in->nTiles, pl->nTiles);
        return -1;
    }
    double t0 = agc_now_s();
    unsigned *tb = malloc((size_t)pl->nTiles * 4);
    long sum = 0;
    for (int i = 0; i < pl->nTiles; ++i) { tb[i] = in->tileBits[i]; sum += tb[i]; }
    if ((sum + 31) / 32 > in->nWords) {
        fprintf(stderr, "agc: truncated stream: tiles need %ld words, file has %d\n",
                (sum + 31) / 32, in->nWords);
        free(tb); return -1;
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, pl->b[B_TBITS]);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (long)pl->nTiles * 4, tb);
    free(tb);

    // Clear only past the payload, so a corrupt or truncated stream reads
    // zeros rather than whatever the previous frame left behind.
    agc_clear_head(pl->b[B_BS], (long)in->nWords * 4 + 256, pl->bsBytes);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, pl->b[B_BS]);
    if (in->nWords)
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (long)in->nWords * 4, in->words);
    agc_stage_sync();
    double tUp = agc_now_s();

    agc_clear_buf(pl->b[B_COEF]);
    agc_stage_sync();
    double tCl = agc_now_s();

    agc_prefix_sum(pg, pl->b[B_TBITS], pl->b[B_TOFF], pl->b[B_TGSUM], pl->b[B_TGOFF],
               pl->nTiles, pl->nTGroups);
    agc_stage_sync();
    double tSc = agc_now_s();

    glUseProgram(pg->parse);
    glUniform1i(glGetUniformLocation(pg->parse, "uN"), pl->nBlk);
    glUniform1i(glGetUniformLocation(pg->parse, "uT"), pl->nTiles);
    glUniform1i(glGetUniformLocation(pg->parse, "uK"), TILE_BLOCKS);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, pl->b[B_BS]);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, pl->b[B_TOFF]);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, pl->b[B_TGOFF]);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, pl->b[B_COEF]);
    glDispatchCompute((pl->nTiles + 63) / 64, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT); agc_stage_sync();
    double tPa = agc_now_s();

    glUseProgram(pg->idct);
    glUniform1i(glGetUniformLocation(pg->idct, "uBlocksX"), pl->bx);
    glUniform1i(glGetUniformLocation(pg->idct, "uWidth"), pl->W);
    glUniform1i(glGetUniformLocation(pg->idct, "uQScale"), qscale);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, pl->b[B_COEF]);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, pl->b[B_REC]);
    glDispatchCompute(pl->bx, pl->by, 1);
    glMemoryBarrier(GL_ALL_BARRIER_BITS); agc_stage_sync();
    double tI = agc_now_s();

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, pl->b[B_REC]);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (long)pl->nWordsPix * 4, dst);
    double tD = agc_now_s();

    agc_tPass[0] = tUp - t0; agc_tPass[1] = tCl - tUp; agc_tPass[2] = tSc - tCl;
    agc_tPass[3] = tPa - tSc; agc_tPass[4] = tI - tPa; agc_tPass[5] = tD - tI;
    agc_gl_check("decode");
    return 0;
}

// ---------------------------------------------------------------- padding

// Pads to a multiple of 8 by replicating the edge, which costs far fewer bits
// than zero-filling because it does not manufacture a hard edge.
static __attribute__((unused)) void agc_pad_plane(const unsigned char *src, int rw, int rh,
                      unsigned char *dst, int W, int H) {
    for (int y = 0; y < H; ++y) {
        int sy = y < rh ? y : rh - 1;
        const unsigned char *s = src + (size_t)sy * rw;
        unsigned char *d = dst + (size_t)y * W;
        memcpy(d, s, rw);
        for (int x = rw; x < W; ++x) d[x] = s[rw - 1];
    }
}

static __attribute__((unused)) void agc_crop_plane(const unsigned char *src, int W,
                       unsigned char *dst, int rw, int rh) {
    for (int y = 0; y < rh; ++y)
        memcpy(dst + (size_t)y * rw, src + (size_t)y * W, rw);
}


// ---------------------------------------------------------------- geometry
// ---------------------------------------------------------------- geometry

static __attribute__((unused)) void agc_plane_dims(int format, int idx, int W, int H, int *pw, int *ph) {
    if (format == AGC_YUV420P && idx > 0) { *pw = (W + 1) / 2; *ph = (H + 1) / 2; }
    else                                  { *pw = W;           *ph = H;           }
}

static __attribute__((unused)) size_t agc_frame_raw_bytes(int format, int W, int H) {
    size_t n = (size_t)W * H;
    if (format == AGC_YUV420P) n += 2 * (size_t)((W + 1) / 2) * ((H + 1) / 2);
    return n;
}

static __attribute__((unused)) int agc_chroma_qscale(int luma) { int q = luma * 2; return q > 255 ? 255 : q; }

#ifdef __cplusplus
}
#endif

#endif // AGC_CORE_H
