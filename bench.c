// Headless GPU compute benchmark for a GPU-native video codec on Adreno/Turnip.
// Measures the two kernels that dominate codec cost: 8x8 DCT+quant (intra)
// and 16x16 SAD motion search (inter), plus host<->device bandwidth.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

static double now_s(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

// The headers declare real prototypes; we resolve everything at runtime
// instead, so keep our pointers in a separate namespace and redirect calls.
#define GLFN(t, n) static t p_##n;
GLFN(PFNGLCREATESHADERPROC, glCreateShader)
GLFN(PFNGLSHADERSOURCEPROC, glShaderSource)
GLFN(PFNGLCOMPILESHADERPROC, glCompileShader)
GLFN(PFNGLGETSHADERIVPROC, glGetShaderiv)
GLFN(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)
GLFN(PFNGLCREATEPROGRAMPROC, glCreateProgram)
GLFN(PFNGLATTACHSHADERPROC, glAttachShader)
GLFN(PFNGLLINKPROGRAMPROC, glLinkProgram)
GLFN(PFNGLGETPROGRAMIVPROC, glGetProgramiv)
GLFN(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)
GLFN(PFNGLUSEPROGRAMPROC, glUseProgram)
GLFN(PFNGLGENBUFFERSPROC, glGenBuffers)
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
GLFN(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)
GLFN(PFNGLGETINTEGERI_VPROC, glGetIntegeri_v)

#define glCreateShader       p_glCreateShader
#define glShaderSource       p_glShaderSource
#define glCompileShader      p_glCompileShader
#define glGetShaderiv        p_glGetShaderiv
#define glGetShaderInfoLog   p_glGetShaderInfoLog
#define glCreateProgram      p_glCreateProgram
#define glAttachShader       p_glAttachShader
#define glLinkProgram        p_glLinkProgram
#define glGetProgramiv       p_glGetProgramiv
#define glGetProgramInfoLog  p_glGetProgramInfoLog
#define glUseProgram         p_glUseProgram
#define glGenBuffers         p_glGenBuffers
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
#define glGetUniformLocation p_glGetUniformLocation
#define glGetIntegeri_v      p_glGetIntegeri_v

static void load_gl(void) {
#define L(n) p_##n = (void *)eglGetProcAddress(#n); if (!p_##n) { fprintf(stderr, "missing %s\n", #n); exit(1); }
    L(glCreateShader) L(glShaderSource) L(glCompileShader) L(glGetShaderiv)
    L(glGetShaderInfoLog) L(glCreateProgram) L(glAttachShader) L(glLinkProgram)
    L(glGetProgramiv) L(glGetProgramInfoLog) L(glUseProgram) L(glGenBuffers)
    L(glBindBuffer) L(glBufferData) L(glBufferSubData) L(glBindBufferBase)
    L(glGetBufferSubData) L(glDispatchCompute) L(glMemoryBarrier) L(glFinish)
    L(glGetString) L(glGetError) L(glUniform1i) L(glGetUniformLocation)
    L(glGetIntegeri_v)
#undef L
}

static void ck(const char *where) {
    GLenum e = glGetError();
    if (e != GL_NO_ERROR) fprintf(stderr, "    !! GL error 0x%04x at %s\n", e, where);
}

// ---- 8x8 forward DCT + quantisation. One workgroup (64 threads) per block,
// which maps exactly onto the Adreno wave64.
static const char *SRC_DCT =
"#version 430\n"
"layout(local_size_x=8, local_size_y=8) in;\n"
"layout(std430, binding=0) readonly  buffer Src { uint px[]; };\n"
"layout(std430, binding=1) writeonly buffer Dst { int  co[]; };\n"
"uniform int uBlocksX; uniform int uWidth;\n"
"shared float s[64];\n"
"const float C0 = 0.35355339059; // 1/sqrt(8)\n"
"const float C  = 0.5;\n"
"float cf(int u){ return u==0 ? C0 : C; }\n"
"void main(){\n"
"  int bx = int(gl_WorkGroupID.x);\n"
"  int by = int(gl_WorkGroupID.y);\n"
"  int lx = int(gl_LocalInvocationID.x), ly = int(gl_LocalInvocationID.y);\n"
"  int gx = bx*8 + lx, gy = by*8 + ly;\n"
"  uint word = px[(gy*uWidth + gx) >> 2];\n"
"  uint byteSel = uint(gx & 3) * 8u;\n"
"  float v = float((word >> byteSel) & 0xFFu) - 128.0;\n"
"  s[ly*8 + lx] = v;\n"
"  barrier();\n"
"  // rows\n"
"  float acc = 0.0;\n"
"  for (int k = 0; k < 8; ++k)\n"
"    acc += s[ly*8 + k] * cos(3.14159265*(2.0*float(k)+1.0)*float(lx)/16.0);\n"
"  acc *= cf(lx);\n"
"  barrier();\n"
"  s[ly*8 + lx] = acc;\n"
"  barrier();\n"
"  // cols\n"
"  float a2 = 0.0;\n"
"  for (int k = 0; k < 8; ++k)\n"
"    a2 += s[k*8 + lx] * cos(3.14159265*(2.0*float(k)+1.0)*float(ly)/16.0);\n"
"  a2 *= cf(ly);\n"
"  // quantise (flat q=16, stand-in for a real matrix)\n"
"  int q = int(round(a2 / 16.0));\n"
"  uint blk = gl_WorkGroupID.y * uint(uBlocksX) + gl_WorkGroupID.x;\n"
"  co[blk*64u + uint(ly*8 + lx)] = q;\n"
"}\n";

// ---- 16x16 SAD motion search over a +/-8 window: the encoder's hot loop.
static const char *SRC_SAD =
"#version 430\n"
"layout(local_size_x=64) in;\n"
"layout(std430, binding=0) readonly  buffer Cur { uint cur[]; };\n"
"layout(std430, binding=1) readonly  buffer Ref { uint ref[]; };\n"
"layout(std430, binding=2) writeonly buffer Out { uint mv[]; };\n"
"uniform int uBlocksX; uniform int uWidth; uniform int uHeight;\n"
"shared uint bestCost[64]; shared uint bestIdx[64];\n"
"uint pix(uint buf, int x, int y);\n"
"uint getc(int x,int y){ uint w = cur[(y*uWidth + x) >> 2]; return (w >> (uint(x&3)*8u)) & 0xFFu; }\n"
"uint getr(int x,int y){ uint w = ref[(y*uWidth + x) >> 2]; return (w >> (uint(x&3)*8u)) & 0xFFu; }\n"
"void main(){\n"
"  int blk = int(gl_WorkGroupID.x);\n"
"  int bx = (blk % uBlocksX) * 16, by = (blk / uBlocksX) * 16;\n"
"  int t = int(gl_LocalInvocationID.x);\n"
"  uint bc = 0xFFFFFFFFu; uint bi = 0u;\n"
"  // 289 candidates (-8..8)^2 spread across 64 threads\n"
"  for (int c = t; c < 289; c += 64) {\n"
"    int dx = (c % 17) - 8, dy = (c / 17) - 8;\n"
"    int rx = bx + dx, ry = by + dy;\n"
"    if (rx < 0 || ry < 0 || rx+16 > uWidth || ry+16 > uHeight) continue;\n"
"    uint sad = 0u;\n"
"    for (int y = 0; y < 16; ++y)\n"
"      for (int x = 0; x < 16; ++x) {\n"
"        int a = int(getc(bx+x, by+y)); int b = int(getr(rx+x, ry+y));\n"
"        sad += uint(abs(a-b));\n"
"      }\n"
"    if (sad < bc) { bc = sad; bi = uint(c); }\n"
"  }\n"
"  bestCost[t] = bc; bestIdx[t] = bi;\n"
"  barrier();\n"
"  for (int s = 32; s > 0; s >>= 1) {\n"
"    if (t < s && bestCost[t+s] < bestCost[t]) { bestCost[t]=bestCost[t+s]; bestIdx[t]=bestIdx[t+s]; }\n"
"    barrier();\n"
"  }\n"
"  if (t == 0) mv[blk] = bestIdx[0];\n"
"}\n";

static GLuint build(const char *src) {
    GLuint sh = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[4096]; glGetShaderInfoLog(sh, 4096, NULL, log);
        fprintf(stderr, "compile failed:\n%s\n", log); exit(1); }
    GLuint p = glCreateProgram(); glAttachShader(p, sh); glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[4096]; glGetProgramInfoLog(p, 4096, NULL, log);
        fprintf(stderr, "link failed:\n%s\n", log); exit(1); }
    return p;
}

int main(void) {
    // ---- headless EGL, no window, no swapchain
    PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatDisp =
        (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay dpy = getPlatDisp ?
        getPlatDisp(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL)
        : eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY) { fprintf(stderr, "no EGL display\n"); return 1; }
    EGLint maj, min;
    if (!eglInitialize(dpy, &maj, &min)) { fprintf(stderr, "eglInitialize failed\n"); return 1; }
    printf("    EGL %d.%d  vendor=%s\n", maj, min, eglQueryString(dpy, EGL_VENDOR));
    eglBindAPI(EGL_OPENGL_API);
    EGLint cfgAttr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                         EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE };
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(dpy, cfgAttr, &cfg, 1, &n) || n < 1) {
        fprintf(stderr, "no EGL config\n"); return 1; }
    EGLint ctxAttr[] = { EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 3,
                         EGL_CONTEXT_OPENGL_PROFILE_MASK,
                         EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttr);
    if (ctx == EGL_NO_CONTEXT) { fprintf(stderr, "no GL 4.3 context\n"); return 1; }
    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        fprintf(stderr, "makeCurrent failed\n"); return 1; }
    load_gl();
    printf("    GL renderer: %s\n", glGetString(GL_RENDERER));
    printf("    GL version : %s\n", glGetString(GL_VERSION));
    GLint wgc[3];
    for (int i = 0; i < 3; ++i) glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, i, &wgc[i]);
    printf("    max workgroup count: %d x %d x %d\n\n", wgc[0], wgc[1], wgc[2]);

    const int W = 3840, H = 2160;
    const int nPix = W * H, nWords = nPix / 4;
    const int bxD = W / 8,  byD = H / 8,  nBlkD = bxD * byD;
    const int bxS = W / 16, byS = H / 16, nBlkS = bxS * byS;

    unsigned *frame = malloc(nWords * 4), *refr = malloc(nWords * 4);
    for (int i = 0; i < nWords; ++i) { frame[i] = (unsigned)(i * 2654435761u);
                                       refr[i]  = (unsigned)(i * 2246822519u); }

    GLuint buf[4]; glGenBuffers(4, buf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf[0]);
    glBufferData(GL_SHADER_STORAGE_BUFFER, (long)nWords*4, frame, GL_STATIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf[1]);
    glBufferData(GL_SHADER_STORAGE_BUFFER, (long)nBlkD*64*4, NULL, GL_DYNAMIC_COPY);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf[2]);
    glBufferData(GL_SHADER_STORAGE_BUFFER, (long)nWords*4, refr, GL_STATIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf[3]);
    glBufferData(GL_SHADER_STORAGE_BUFFER, (long)nBlkS*4, NULL, GL_DYNAMIC_COPY);
    glFinish();

    // ---- upload bandwidth
    double t0 = now_s();
    for (int i = 0; i < 20; ++i) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf[0]);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (long)nWords*4, frame);
    }
    glFinish();
    double up = (now_s() - t0) / 20.0;
    printf("    upload 4K luma (8.3MB)   : %7.2f ms  -> %6.1f GB/s  (%.0f fps ceiling)\n",
           up*1e3, (nWords*4.0)/up/1e9, 1.0/up);

    // ---- DCT
    GLuint pD = build(SRC_DCT);
    glUseProgram(pD);
    glUniform1i(glGetUniformLocation(pD, "uBlocksX"), bxD);
    glUniform1i(glGetUniformLocation(pD, "uWidth"), W);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buf[0]);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, buf[1]);
    glDispatchCompute(bxD, byD, 1); glMemoryBarrier(GL_ALL_BARRIER_BITS); glFinish();
    ck("dct warmup");
    int itD = 30; t0 = now_s();
    for (int i = 0; i < itD; ++i) { glDispatchCompute(bxD, byD, 1);
                                    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT); }
    glFinish();
    double dt = (now_s() - t0) / itD;
    printf("    DCT8x8+quant 4K frame    : %7.2f ms  -> %6.1f fps   (%d blocks)\n",
           dt*1e3, 1.0/dt, nBlkD);

    // ---- SAD motion search
    GLuint pS = build(SRC_SAD);
    glUseProgram(pS);
    glUniform1i(glGetUniformLocation(pS, "uBlocksX"), bxS);
    glUniform1i(glGetUniformLocation(pS, "uWidth"), W);
    glUniform1i(glGetUniformLocation(pS, "uHeight"), H);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buf[0]);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, buf[2]);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, buf[3]);
    glDispatchCompute(nBlkS, 1, 1); glMemoryBarrier(GL_ALL_BARRIER_BITS); glFinish();
    int itS = 10; t0 = now_s();
    for (int i = 0; i < itS; ++i) { glDispatchCompute(nBlkS, 1, 1);
                                    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT); }
    glFinish();
    double st = (now_s() - t0) / itS;
    double sadOps = (double)nBlkS * 289.0 * 256.0;
    printf("    SAD ME 16x16 +/-8 4K     : %7.2f ms  -> %6.1f fps   (%.2f G SAD/s)\n",
           st*1e3, 1.0/st, sadOps/st/1e9);

    // ---- readback
    t0 = now_s();
    int *co = malloc((long)nBlkD*64*4);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf[1]);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (long)nBlkD*64*4, co);
    double rb = now_s() - t0;
    printf("    readback coeffs (%.1fMB)  : %7.2f ms  -> %6.1f GB/s\n",
           nBlkD*64*4/1e6, rb*1e3, (nBlkD*64.0*4)/rb/1e9);

    long nz = 0; for (int i = 0; i < nBlkD*64; ++i) if (co[i]) nz++;
    printf("\n    non-zero coefficients: %ld / %d\n", nz, nBlkD*64);

    // ---- correctness: CPU reference DCT of block (0,0) vs the GPU result
    double maxerr = 0.0; int worst = 0;
    for (int v = 0; v < 8; ++v) for (int u = 0; u < 8; ++u) {
        double acc = 0.0;
        for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) {
            unsigned w = frame[(y*W + x) >> 2];
            double f = (double)((w >> ((x & 3) * 8)) & 0xFFu) - 128.0;
            acc += f * cos(3.14159265358979 * (2.0*x + 1.0) * u / 16.0)
                     * cos(3.14159265358979 * (2.0*y + 1.0) * v / 16.0);
        }
        double cu = (u == 0) ? 0.35355339059 : 0.5;
        double cv = (v == 0) ? 0.35355339059 : 0.5;
        int ref = (int)lround(acc * cu * cv / 16.0);
        int got = co[v*8 + u];
        double e = fabs((double)(ref - got));
        if (e > maxerr) { maxerr = e; worst = v*8 + u; }
    }
    printf("    CPU-reference check      : max |gpu-cpu| = %.0f quantised step"
           " (coef #%d)  -> %s\n", maxerr, worst,
           maxerr <= 1.0 ? "MATCH" : "MISMATCH");
    return 0;
}
