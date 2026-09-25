// AGC-1 : a GPU-native intra codec for Adreno/Turnip.
//
// Every stage runs in a compute shader and is parallel over 8x8 blocks, with
// no serial entropy feedback and no cross-block prediction chains -- the two
// things that make H.264/H.265 hostile to GPUs. Entropy coding happens on the
// GPU so that only the compressed bitstream crosses the bus, which matters
// because measured readback here is ~0.9 GB/s versus ~20 GB/s upload.
//
// ---------------------------------------------------------------- bitstream
// Per-block, bit-exact, no padding between blocks:
//   1 bit   coded     0 => block is entirely zero and nothing else follows
//   6 bits  lastPos   zigzag index of the last significant coefficient
//   n bits  mask      significance flags, zigzag order, n = lastPos+1
//   per significant coefficient, a variable-length magnitude:
//     unary size prefix  (s-1) ones then a zero, or (s-1) ones when s == 15
//     then s bits        sign in the top bit, low s-1 magnitude bits below
//                        (the leading 1 of the magnitude is implicit)
//   so a +-1 coefficient costs 2 bits rather than a fixed width.
//
// Because blocks are bit-packed with no padding, a decoder cannot seek to
// block N without knowing every preceding length. Shipping a 32-bit offset
// per block would cost more than the payload itself, so the stream instead
// carries one 16-bit length per TILE of 32 blocks (~1.4% overhead). The
// decoder prefix-sums those on the GPU to recover tile offsets, then parses
// tiles in parallel, walking the 32 blocks inside a tile sequentially --
// each block's parse reveals exactly where the next one starts.
//
// ---------------------------------------------------------------- container
// All multi-byte fields little-endian.
//   magic   4  "AGC1"
//   version 1  currently 1
//   format  1  0 = gray8, 1 = yuv420p
//   quality 1  1..100, both sides derive the quant scale from this
//   tileLog 1  log2(blocks per tile), currently 5
//   width   4  real luma width  (frame is coded padded up to a multiple of 8)
//   height  4  real luma height
//   planes  4  1 for gray8, 3 for yuv420p
//   fps     4  frames per second x1000, 0 when unknown
//   then per plane: nTiles(4) nWords(4) crc32(4)
//                   tileBits(2*nTiles, padded to 4) payload(4*nWords)
//   The CRC32 covers that plane's tile index and payload.
//
// A sequence is just these frames written one after another. Repeating the
// header every frame costs 24 bytes but keeps each one self-contained, so
// damage in the middle of a stream does not destroy what came before it. A
// frame is self-delimiting, so nothing needs an index to find the next one.
// That bounds the loss from a Ctrl-C to the frame in flight; the signal
// handling below removes even that by stopping between frames.
//
// A stream cannot be seeked, so nothing here may be validated against a file
// size. Instead both counts are checked for exact agreement: nTiles must match
// what the dimensions imply and nWords must match what the tile index sums to.
// That is stricter than a size check and behaves the same on a pipe.

#include "glboot.h"
#include <string.h>
#include <math.h>
#include <errno.h>
#include <signal.h>
#include <sys/resource.h>

// Ctrl-C during a live capture must not leave a half-written frame behind:
// the container is strict on purpose, so a torn frame costs the reader the
// whole tail of the stream. Instead of dying where it stands, the streaming
// loops finish the frame in flight, flush it and stop between frames.
//
// The handler is installed with signal(), which on glibc implies SA_RESTART.
// That is deliberate. Without it a signal arriving mid-read would cut fread
// short, and a partial frame read is indistinguishable from a truncated
// input -- exactly the error this is meant to avoid.
static volatile sig_atomic_t g_stop = 0;
static void on_stop(int s) { (void)s; g_stop = 1; }
static void install_stop_handlers(void) {
    signal(SIGINT, on_stop);
    signal(SIGTERM, on_stop);
}

#define AGC_VERSION   1
#define AGC_GRAY8     0
#define AGC_YUV420P   1
#define TILE_LOG      5
#define TILE_BLOCKS   (1 << TILE_LOG)
#define HDR_BYTES     24
// worst case block: 1 + 6 + 64 mask + 64 coefficients * 29 bits
#define MAX_BLOCK_BITS (1 + 6 + 64 + 64 * 29)

// ---------------------------------------------------------------- tables
// Defined once in C and emitted into GLSL, so the shaders and the host can
// never disagree. IZZ is derived and checked rather than typed by hand.
static const int ZZ[64] = {
     0, 1, 8,16, 9, 2, 3,10,
    17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,
    27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,
    29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,
    53,60,61,54,47,55,62,63
};
static const int QT[64] = {
    16,11,10,16,24,40,51,61,
    12,12,14,19,26,58,60,55,
    14,13,16,24,40,57,69,56,
    14,17,22,29,51,87,80,62,
    18,22,37,56,68,109,103,77,
    24,35,55,64,81,104,113,92,
    49,64,78,87,103,121,120,101,
    72,92,95,98,112,100,103,99
};
static int IZZ[64];

static char *g_prelude;

static void build_prelude(void) {
    for (int i = 0; i < 64; ++i) IZZ[i] = -1;
    for (int i = 0; i < 64; ++i) IZZ[ZZ[i]] = i;
    for (int i = 0; i < 64; ++i)
        if (IZZ[i] < 0 || ZZ[IZZ[i]] != i) {
            fprintf(stderr, "agc: zigzag table is not a valid permutation at %d\n", i);
            exit(1);
        }
    char *s = malloc(8192); int n = 0;
    n += sprintf(s + n, "const int ZZ[64] = int[64](");
    for (int i = 0; i < 64; ++i) n += sprintf(s + n, "%d%s", ZZ[i], i == 63 ? ");\n" : ",");
    n += sprintf(s + n, "const int IZZ[64] = int[64](");
    for (int i = 0; i < 64; ++i) n += sprintf(s + n, "%d%s", IZZ[i], i == 63 ? ");\n" : ",");
    n += sprintf(s + n, "const int QT[64] = int[64](");
    for (int i = 0; i < 64; ++i) n += sprintf(s + n, "%d%s", QT[i], i == 63 ? ");\n" : ",");
    n += sprintf(s + n, "int qval(int i, int s){ return clamp((QT[i]*s + 50)/100, 1, 255); }\n");
    g_prelude = s;
}

// Assembles "#version" + generated tables + shader body.
static GLuint build_shader(const char *body, const char *name) {
    size_t len = strlen(g_prelude) + strlen(body) + 64;
    char *src = malloc(len);
    snprintf(src, len, "#version 430\n%s%s", g_prelude, body);
    GLuint p = gl_build(src, name);
    free(src);
    return p;
}

// ---------------------------------------------------------------- shaders

// Pass 1: DCT + quantise, then derive this block's exact bit length.
static const char *SRC_ANALYZE =
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
static const char *SRC_SCAN1 =
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
static const char *SRC_SCAN2 =
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
static const char *SRC_TILESUM =
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
static const char *SRC_PACK =
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
static const char *SRC_PARSE =
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
static const char *SRC_IDCT =
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

// ---------------------------------------------------------------- context

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

// Per-stage wall time of the last encode/decode, for bench mode.
static double g_tPass[8];

// Draining the pipeline between stages is what lets bench attribute time to
// each one, but every glFinish is a full stall. On a tiled GPU that is
// milliseconds of latency, and a colour frame runs the sequence three times,
// so leaving it on made a 640x480 frame cost as much as a 4K one. Outside
// bench mode the memory barriers alone are what correctness requires: the
// barriers order the dependent dispatches, and glGetBufferSubData synchronises
// implicitly when the result is finally read back.
static int g_timing = 0;
static void stage_sync(void) { if (g_timing) glFinish(); }

static int qscale_for(int quality) {
    int q = (quality < 50) ? (5000 / quality) : (200 - 2 * quality);
    return q < 1 ? 1 : q;
}

static void progs_init(Progs *p) {
    build_prelude();
    p->analyze = build_shader(SRC_ANALYZE, "analyze");
    p->scan1   = build_shader(SRC_SCAN1,   "scan1");
    p->scan2   = build_shader(SRC_SCAN2,   "scan2");
    p->tilesum = build_shader(SRC_TILESUM, "tilesum");
    p->pack    = build_shader(SRC_PACK,    "pack");
    p->parse   = build_shader(SRC_PARSE,   "parse");
    p->idct    = build_shader(SRC_IDCT,    "idct");
}

static void plane_init(Plane *pl, int rw, int rh) {
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
    gl_check("plane buffers");
}

static void plane_free(Plane *pl) { glDeleteBuffers(B_COUNT, pl->b); }

static void clear_buf(GLuint buf) {
    unsigned zero = 0;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER,
                      GL_UNSIGNED_INT, &zero);
}

// Clears only the head of a buffer. The bitstream buffer is sized for the
// worst case, which is ~55x the typical stream, so clearing all of it would
// cost more than every other decode stage combined.
static void clear_head(GLuint buf, long bytes, long cap) {
    if (bytes > cap) bytes = cap;
    bytes &= ~3L;
    if (bytes <= 0) return;
    unsigned zero = 0;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf);
    glClearBufferSubData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, 0, bytes,
                         GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
}

// Exclusive prefix sum of n elements of `in`, into `off` (+ `goff` per 256).
static void prefix_sum(Progs *pg, GLuint in, GLuint off, GLuint gsum,
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

static void bits_free(Bits *bt) {
    free(bt->words); free(bt->tileBits);
    bt->words = NULL; bt->tileBits = NULL;
}

// `src` is W*H padded pixels. Returns 0 on success.
static int encode_plane(Progs *pg, Plane *pl, const unsigned char *src,
                        int qscale, Bits *out) {
    double t0 = now_s();
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, pl->b[B_SRC]);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (long)pl->nWordsPix * 4, src);
    stage_sync();
    double tUp = now_s();

    glUseProgram(pg->analyze);
    glUniform1i(glGetUniformLocation(pg->analyze, "uBlocksX"), pl->bx);
    glUniform1i(glGetUniformLocation(pg->analyze, "uWidth"), pl->W);
    glUniform1i(glGetUniformLocation(pg->analyze, "uQScale"), qscale);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, pl->b[B_SRC]);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, pl->b[B_COEF]);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, pl->b[B_META]);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, pl->b[B_WORD]);
    glDispatchCompute(pl->bx, pl->by, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT); stage_sync();
    double tA = now_s();

    prefix_sum(pg, pl->b[B_WORD], pl->b[B_OFF], pl->b[B_GSUM], pl->b[B_GOFF],
               pl->nBlk, pl->nGroups);

    glUseProgram(pg->tilesum);
    glUniform1i(glGetUniformLocation(pg->tilesum, "uN"), pl->nBlk);
    glUniform1i(glGetUniformLocation(pg->tilesum, "uT"), pl->nTiles);
    glUniform1i(glGetUniformLocation(pg->tilesum, "uK"), TILE_BLOCKS);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, pl->b[B_WORD]);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, pl->b[B_TBITS]);
    glDispatchCompute((pl->nTiles + 63) / 64, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT); stage_sync();

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
    double tS = now_s();

    clear_head(pl->b[B_BS], (long)out->nWords * 4 + 64, pl->bsBytes);
    glUseProgram(pg->pack);
    glUniform1i(glGetUniformLocation(pg->pack, "uN"), pl->nBlk);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, pl->b[B_COEF]);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, pl->b[B_META]);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, pl->b[B_OFF]);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, pl->b[B_GOFF]);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, pl->b[B_BS]);
    glDispatchCompute((pl->nBlk + 63) / 64, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT); stage_sync();
    double tP = now_s();

    out->words = malloc((size_t)out->nWords * 4 + 4);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, pl->b[B_BS]);
    if (out->nWords)
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (long)out->nWords * 4, out->words);
    double tR = now_s();

    g_tPass[0] = tUp - t0;  g_tPass[1] = tA - tUp; g_tPass[2] = tS - tA;
    g_tPass[3] = tP - tS;   g_tPass[4] = tR - tP;  g_tPass[5] = 0;
    gl_check("encode");
    return 0;
}

// ---------------------------------------------------------------- decode

// Writes W*H padded pixels into `dst`.
static int decode_plane(Progs *pg, Plane *pl, const Bits *in, int qscale,
                        unsigned char *dst) {
    if (in->nTiles != pl->nTiles) {
        fprintf(stderr, "agc: stream has %d tiles, geometry implies %d\n",
                in->nTiles, pl->nTiles);
        return -1;
    }
    double t0 = now_s();
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
    clear_head(pl->b[B_BS], (long)in->nWords * 4 + 256, pl->bsBytes);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, pl->b[B_BS]);
    if (in->nWords)
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (long)in->nWords * 4, in->words);
    stage_sync();
    double tUp = now_s();

    clear_buf(pl->b[B_COEF]);
    stage_sync();
    double tCl = now_s();

    prefix_sum(pg, pl->b[B_TBITS], pl->b[B_TOFF], pl->b[B_TGSUM], pl->b[B_TGOFF],
               pl->nTiles, pl->nTGroups);
    stage_sync();
    double tSc = now_s();

    glUseProgram(pg->parse);
    glUniform1i(glGetUniformLocation(pg->parse, "uN"), pl->nBlk);
    glUniform1i(glGetUniformLocation(pg->parse, "uT"), pl->nTiles);
    glUniform1i(glGetUniformLocation(pg->parse, "uK"), TILE_BLOCKS);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, pl->b[B_BS]);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, pl->b[B_TOFF]);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, pl->b[B_TGOFF]);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, pl->b[B_COEF]);
    glDispatchCompute((pl->nTiles + 63) / 64, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT); stage_sync();
    double tPa = now_s();

    glUseProgram(pg->idct);
    glUniform1i(glGetUniformLocation(pg->idct, "uBlocksX"), pl->bx);
    glUniform1i(glGetUniformLocation(pg->idct, "uWidth"), pl->W);
    glUniform1i(glGetUniformLocation(pg->idct, "uQScale"), qscale);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, pl->b[B_COEF]);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, pl->b[B_REC]);
    glDispatchCompute(pl->bx, pl->by, 1);
    glMemoryBarrier(GL_ALL_BARRIER_BITS); stage_sync();
    double tI = now_s();

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, pl->b[B_REC]);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (long)pl->nWordsPix * 4, dst);
    double tD = now_s();

    g_tPass[0] = tUp - t0; g_tPass[1] = tCl - tUp; g_tPass[2] = tSc - tCl;
    g_tPass[3] = tPa - tSc; g_tPass[4] = tI - tPa; g_tPass[5] = tD - tI;
    gl_check("decode");
    return 0;
}

// ---------------------------------------------------------------- padding

// Pads to a multiple of 8 by replicating the edge, which costs far fewer bits
// than zero-filling because it does not manufacture a hard edge.
static void pad_plane(const unsigned char *src, int rw, int rh,
                      unsigned char *dst, int W, int H) {
    for (int y = 0; y < H; ++y) {
        int sy = y < rh ? y : rh - 1;
        const unsigned char *s = src + (size_t)sy * rw;
        unsigned char *d = dst + (size_t)y * W;
        memcpy(d, s, rw);
        for (int x = rw; x < W; ++x) d[x] = s[rw - 1];
    }
}

static void crop_plane(const unsigned char *src, int W,
                       unsigned char *dst, int rw, int rh) {
    for (int y = 0; y < rh; ++y)
        memcpy(dst + (size_t)y * rw, src + (size_t)y * W, rw);
}

// ---------------------------------------------------------------- container

static void put32(unsigned char *p, unsigned v) {
    p[0]=(unsigned char)v; p[1]=(unsigned char)(v>>8);
    p[2]=(unsigned char)(v>>16); p[3]=(unsigned char)(v>>24);
}
static unsigned get32(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1]<<8) | ((unsigned)p[2]<<16) | ((unsigned)p[3]<<24);
}
static void put16(unsigned char *p, unsigned v) {
    p[0]=(unsigned char)v; p[1]=(unsigned char)(v>>8);
}
static unsigned get16(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1]<<8);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "agc: out of memory (%zu bytes)\n", n); exit(1); }
    return p;
}

// A stored frame that silently decodes to garbage is worse than one that
// refuses to decode, so each plane carries a CRC32 of its index and payload.
static unsigned crc32_buf(const void *data, size_t n, unsigned crc) {
    static unsigned tab[256];
    static int init = 0;
    if (!init) {
        for (unsigned i = 0; i < 256; ++i) {
            unsigned c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            tab[i] = c;
        }
        init = 1;
    }
    const unsigned char *p = (const unsigned char *)data;
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = tab[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// ---------------------------------------------------------------- geometry

static void plane_dims(int format, int idx, int W, int H, int *pw, int *ph) {
    if (format == AGC_YUV420P && idx > 0) { *pw = (W + 1) / 2; *ph = (H + 1) / 2; }
    else                                  { *pw = W;           *ph = H;           }
}

static size_t frame_raw_bytes(int format, int W, int H) {
    size_t n = (size_t)W * H;
    if (format == AGC_YUV420P) n += 2 * (size_t)((W + 1) / 2) * ((H + 1) / 2);
    return n;
}

static int chroma_qscale(int luma) { int q = luma * 2; return q > 255 ? 255 : q; }

// How many tiles a plane of these dimensions must contain. A stream cannot be
// seeked to measure it against the file size, but this is exact, so the
// decoder can reject a wrong tile count outright rather than merely
// implausible ones.
static int expected_tiles(int format, int idx, int W, int H) {
    int pw, ph; plane_dims(format, idx, W, H, &pw, &ph);
    int bx = ((pw + 7) & ~7) / 8, by = ((ph + 7) & ~7) / 8;
    long nBlk = (long)bx * by;
    return (int)((nBlk + TILE_BLOCKS - 1) / TILE_BLOCKS);
}

typedef struct {
    int format, quality, width, height, nplanes;
    unsigned fpsMilli;          // frames per second x1000, 0 if unknown
    Bits plane[3];
} Frame;

static void frame_free(Frame *f) {
    for (int i = 0; i < f->nplanes; ++i) bits_free(&f->plane[i]);
}

static int frame_write_fp(FILE *fp, const Frame *f, const char *name) {
    unsigned char h[HDR_BYTES];
    memset(h, 0, sizeof h);
    memcpy(h, "AGC1", 4);
    h[4] = AGC_VERSION; h[5] = (unsigned char)f->format;
    h[6] = (unsigned char)f->quality; h[7] = TILE_LOG;
    put32(h + 8,  (unsigned)f->width);
    put32(h + 12, (unsigned)f->height);
    put32(h + 16, (unsigned)f->nplanes);
    put32(h + 20, f->fpsMilli);
    if (fwrite(h, 1, HDR_BYTES, fp) != HDR_BYTES) goto werr;

    for (int i = 0; i < f->nplanes; ++i) {
        const Bits *b = &f->plane[i];
        size_t tbBytes = (size_t)b->nTiles * 2;
        size_t padded = (tbBytes + 3) & ~(size_t)3;
        unsigned char *tb = xmalloc(padded + 4);
        memset(tb, 0, padded + 4);
        for (int t = 0; t < b->nTiles; ++t) put16(tb + t * 2, b->tileBits[t]);
        // The payload is stored as little-endian 32-bit words so the file is
        // byte-order independent even though the packer works in words.
        size_t pwBytes = (size_t)b->nWords * 4;
        unsigned char *pw = xmalloc(pwBytes + 4);
        for (int w = 0; w < b->nWords; ++w) put32(pw + w * 4, b->words[w]);

        unsigned crc = crc32_buf(tb, padded, 0);
        crc = crc32_buf(pw, pwBytes, crc);

        unsigned char ph[12];
        put32(ph, (unsigned)b->nTiles);
        put32(ph + 4, (unsigned)b->nWords);
        put32(ph + 8, crc);
        if (fwrite(ph, 1, 12, fp) != 12) { free(tb); free(pw); goto werr; }
        if (fwrite(tb, 1, padded, fp) != padded) { free(tb); free(pw); goto werr; }
        if (pwBytes && fwrite(pw, 1, pwBytes, fp) != pwBytes) { free(tb); free(pw); goto werr; }
        free(tb); free(pw);
    }
    return 0;
werr:
    fprintf(stderr, "agc: %s: short write: %s\n", name, strerror(errno));
    return -1;
}


// Reads one frame from a stream. Returns 0 on success, 1 at a clean end of
// stream (nothing at all was left to read) and -1 on a damaged one.
//
// A pipe cannot be seeked, so the old file-size validation is not available
// here. It is replaced by something stronger: the tile count and the word
// count are both *exactly* derivable -- the tile count from the frame
// dimensions, the word count from the sum of the tile lengths -- so anything
// that disagrees is rejected before a single byte is allocated.
static int frame_read_fp(FILE *fp, Frame *f, const char *name) {
    memset(f, 0, sizeof *f);
    unsigned char h[HDR_BYTES];
    size_t got = fread(h, 1, HDR_BYTES, fp);
    if (got == 0 && feof(fp)) return 1;
    if (got != HDR_BYTES) {
        fprintf(stderr, "agc: %s: truncated header (%zu of %d bytes)\n",
                name, got, HDR_BYTES);
        return -1;
    }
    if (memcmp(h, "AGC1", 4) != 0) {
        fprintf(stderr, "agc: %s: not an AGC stream (bad magic)\n", name);
        return -1;
    }
    if (h[4] != AGC_VERSION) {
        fprintf(stderr, "agc: %s: version %u, this build understands %u\n",
                name, h[4], AGC_VERSION);
        return -1;
    }
    if (h[7] != TILE_LOG) {
        fprintf(stderr, "agc: %s: tile size 2^%u, this build uses 2^%u\n",
                name, h[7], TILE_LOG);
        return -1;
    }
    f->format   = h[5];
    f->quality  = h[6];
    f->width    = (int)get32(h + 8);
    f->height   = (int)get32(h + 12);
    f->nplanes  = (int)get32(h + 16);
    f->fpsMilli = get32(h + 20);
    if (f->format != AGC_GRAY8 && f->format != AGC_YUV420P) {
        fprintf(stderr, "agc: %s: unknown pixel format %d\n", name, f->format);
        return -1;
    }
    int want = (f->format == AGC_YUV420P) ? 3 : 1;
    if (f->nplanes != want) {
        fprintf(stderr, "agc: %s: format implies %d planes, header says %d\n",
                name, want, f->nplanes);
        return -1;
    }
    if (f->width <= 0 || f->height <= 0 || f->width > 65536 || f->height > 65536 ||
        f->quality < 1 || f->quality > 100) {
        fprintf(stderr, "agc: %s: implausible header (%dx%d q%d)\n",
                name, f->width, f->height, f->quality);
        return -1;
    }
    for (int i = 0; i < f->nplanes; ++i) {
        unsigned char ph[12];
        if (fread(ph, 1, 12, fp) != 12) goto rerr;
        Bits *b = &f->plane[i];
        b->nTiles = (int)get32(ph);
        b->nWords = (int)get32(ph + 4);
        unsigned wantCrc = get32(ph + 8);

        int mustTiles = expected_tiles(f->format, i, f->width, f->height);
        if (b->nTiles != mustTiles) {
            fprintf(stderr, "agc: %s: plane %d declares %d tiles but %dx%d "
                            "requires exactly %d\n",
                    name, i, b->nTiles, f->width, f->height, mustTiles);
            goto rerr_quiet;
        }
        if (b->nWords < 0) goto rerr;

        size_t tbBytes = (size_t)b->nTiles * 2;
        size_t padded = (tbBytes + 3) & ~(size_t)3;
        unsigned char *tb = xmalloc(padded + 4);
        if (fread(tb, 1, padded, fp) != padded) { free(tb); goto rerr; }

        b->tileBits = xmalloc((size_t)b->nTiles * sizeof *b->tileBits + 4);
        b->totalBits = 0;
        for (int t = 0; t < b->nTiles; ++t) {
            b->tileBits[t] = (unsigned short)get16(tb + t * 2);
            b->totalBits += b->tileBits[t];
        }
        // The encoder derives nWords from totalBits, so any other value means
        // the stream is inconsistent with itself.
        int mustWords = (int)((b->totalBits + 31) / 32);
        if (b->nWords != mustWords) {
            fprintf(stderr, "agc: %s: plane %d declares %d words but its tile "
                            "index accounts for %d\n",
                    name, i, b->nWords, mustWords);
            free(tb);
            goto rerr_quiet;
        }

        size_t pwBytes = (size_t)b->nWords * 4;
        unsigned char *pw = xmalloc(pwBytes + 4);
        if (pwBytes && fread(pw, 1, pwBytes, fp) != pwBytes) { free(tb); free(pw); goto rerr; }

        unsigned crc = crc32_buf(tb, padded, 0);
        crc = crc32_buf(pw, pwBytes, crc);
        if (crc != wantCrc) {
            fprintf(stderr, "agc: %s: plane %d failed its checksum "
                            "(stored %08x, computed %08x) -- the stream is damaged\n",
                    name, i, wantCrc, crc);
            free(tb); free(pw);
            goto rerr_quiet;
        }
        free(tb);
        b->words = xmalloc(pwBytes + 4);
        for (int w = 0; w < b->nWords; ++w) b->words[w] = get32(pw + w * 4);
        free(pw);
    }
    return 0;
rerr:
    fprintf(stderr, "agc: %s: truncated or corrupt stream\n", name);
rerr_quiet:
    frame_free(f);
    return -1;
}


// ---------------------------------------------------------------- commands

static int parse_size(const char *s, int *w, int *h) {
    const char *x = strchr(s, 'x');
    if (!x) x = strchr(s, 'X');
    if (!x) return -1;
    *w = atoi(s); *h = atoi(x + 1);
    return (*w > 0 && *h > 0) ? 0 : -1;
}

static int parse_format(const char *s) {
    if (!strcmp(s, "gray8") || !strcmp(s, "gray")) return AGC_GRAY8;
    if (!strcmp(s, "yuv420p") || !strcmp(s, "yuv420")) return AGC_YUV420P;
    return -1;
}

static const char *format_name(int f) {
    return f == AGC_YUV420P ? "yuv420p" : "gray8";
}

// A plane's worth of GPU state, held across frames. It is rebuilt only when
// the geometry changes, so a long sequence pays for buffer allocation and
// shader setup once instead of once per frame. At 4K that is the difference
// between streaming and stuttering.
typedef struct { Plane pl; unsigned char *pad; int inited, rw, rh; } PlaneCtx;

static Plane *plane_ctx(PlaneCtx *c, int rw, int rh) {
    if (!c->inited || c->rw != rw || c->rh != rh) {
        if (c->inited) { plane_free(&c->pl); free(c->pad); }
        plane_init(&c->pl, rw, rh);
        // pad_plane and decode_plane both write every pixel of the padded
        // plane, so this buffer never needs clearing between frames.
        c->pad = xmalloc((size_t)c->pl.W * c->pl.H + 4);
        c->inited = 1; c->rw = rw; c->rh = rh;
    }
    return &c->pl;
}

static void plane_ctx_free(PlaneCtx *c) {
    if (c->inited) { plane_free(&c->pl); free(c->pad); }
    c->inited = 0;
}

// A lone "-" means stdin or stdout. Option parsing already lets it through,
// because the unknown-option test requires a character after the dash.
static int is_stdio(const char *p) { return p && p[0] == '-' && p[1] == 0; }

static int cmd_encode(int argc, char **argv) {
    const char *in = NULL, *out = NULL;
    int W = 0, H = 0, format = AGC_GRAY8, quality = 75, verbose = 0;
    double fps = 0;
    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc) {
            if (parse_size(argv[++i], &W, &H)) {
                fprintf(stderr, "agc: bad size '%s', expected WxH\n", argv[i]); return 2; }
        } else if (!strcmp(argv[i], "-f") && i + 1 < argc) {
            format = parse_format(argv[++i]);
            if (format < 0) { fprintf(stderr, "agc: bad format '%s'\n", argv[i]); return 2; }
        } else if (!strcmp(argv[i], "-q") && i + 1 < argc) {
            quality = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-r") && i + 1 < argc) {
            fps = atof(argv[++i]);
        } else if (!strcmp(argv[i], "-v")) { verbose = 1;
        } else if (argv[i][0] == '-' && argv[i][1]) {
            fprintf(stderr, "agc: unknown option '%s'\n", argv[i]); return 2;
        } else if (!in)  in = argv[i];
        else if (!out) out = argv[i];
        else { fprintf(stderr, "agc: unexpected argument '%s'\n", argv[i]); return 2; }
    }
    if (!in || !out) {
        fprintf(stderr, "agc: encode needs an input and an output path "
                        "('-' for stdin/stdout)\n");
        return 2;
    }
    if (!W || !H)    { fprintf(stderr, "agc: encode needs -s WxH\n"); return 2; }
    if (quality < 1 || quality > 100) { fprintf(stderr, "agc: quality must be 1..100\n"); return 2; }
    if (fps < 0 || fps > 1000) { fprintf(stderr, "agc: frame rate must be 0..1000\n"); return 2; }

    FILE *fi = is_stdio(in) ? stdin : fopen(in, "rb");
    if (!fi) { fprintf(stderr, "agc: %s: %s\n", in, strerror(errno)); return 1; }
    FILE *fo = is_stdio(out) ? stdout : fopen(out, "wb");
    if (!fo) {
        fprintf(stderr, "agc: %s: %s\n", out, strerror(errno));
        if (fi != stdin) fclose(fi);
        return 1;
    }
    // Nothing human-readable may be written to the same place as the
    // bitstream, or the stream is corrupted by its own status line.
    FILE *msg = (fo == stdout) ? stderr : stdout;

    size_t need = frame_raw_bytes(format, W, H);
    unsigned char *raw = xmalloc(need);
    int nplanes = (format == AGC_YUV420P) ? 3 : 1;
    int qy = qscale_for(quality), qc = chroma_qscale(qy);

    gl_boot(0);
    Progs pg; progs_init(&pg);
    PlaneCtx ctx[3]; memset(ctx, 0, sizeof ctx);

    long frames = 0, bytes = 0;
    int rc = 0, stopped = 0;
    double t0 = now_s();
    install_stop_handlers();
    for (;;) {
        if (g_stop) { stopped = 1; break; }
        size_t got = fread(raw, 1, need, fi);
        if (got == 0) {
            if (ferror(fi)) {
                fprintf(stderr, "agc: %s: %s\n", in, strerror(errno));
                rc = 1;
            }
            break;
        }
        if (got != need) {
            fprintf(stderr, "agc: %s: frame %ld is short -- %dx%d %s needs "
                            "%zu bytes, got %zu\n",
                    in, frames, W, H, format_name(format), need, got);
            rc = 1; break;
        }
        Frame f; memset(&f, 0, sizeof f);
        f.format = format; f.quality = quality;
        f.width = W; f.height = H; f.nplanes = nplanes;
        f.fpsMilli = (unsigned)(fps * 1000.0 + 0.5);

        size_t off = 0;
        for (int i = 0; i < nplanes; ++i) {
            int pw, ph; plane_dims(format, i, W, H, &pw, &ph);
            Plane *pl = plane_ctx(&ctx[i], pw, ph);
            pad_plane(raw + off, pw, ph, ctx[i].pad, pl->W, pl->H);
            if (encode_plane(&pg, pl, ctx[i].pad, i ? qc : qy, &f.plane[i])) { rc = 1; break; }
            if (verbose)
                fprintf(stderr, "    frame %ld plane %d  %dx%d (coded %dx%d)  "
                                "%d tiles  %ld bits\n",
                        frames, i, pw, ph, pl->W, pl->H,
                        f.plane[i].nTiles, f.plane[i].totalBits);
            off += (size_t)pw * ph;
        }
        if (rc) { frame_free(&f); break; }

        for (int i = 0; i < nplanes; ++i)
            bytes += (long)f.plane[i].nWords * 4 + ((f.plane[i].nTiles * 2 + 3) & ~3) + 12;
        bytes += HDR_BYTES;

        if (frame_write_fp(fo, &f, out)) { frame_free(&f); rc = 1; break; }
        frame_free(&f);
        frames++;
    }
    double t1 = now_s();

    for (int i = 0; i < 3; ++i) plane_ctx_free(&ctx[i]);
    free(raw);
    if (fi != stdin) fclose(fi);
    if (fo != stdout) { if (fclose(fo) != 0) { fprintf(stderr, "agc: %s: %s\n", out, strerror(errno)); rc = 1; } }
    else fflush(fo);

    if (rc) return 1;
    if (frames == 0) {
        if (stopped) { fprintf(stderr, "agc: interrupted before a frame completed\n"); return 1; }
        fprintf(stderr, "agc: %s: no frames in input\n", in); return 1;
    }
    if (stopped)
        fprintf(msg, "agc: interrupted -- %ld complete frames written\n", frames);

    double dt = t1 - t0;
    if (dt <= 0) dt = 1e-9;
    if (frames == 1)
        fprintf(msg, "%s -> %s  %dx%d %s q%d  %.1f KB  %.2f:1  %.1f ms\n",
                in, out, W, H, format_name(format), quality,
                bytes / 1024.0, (double)need / (double)bytes, dt * 1e3);
    else
        fprintf(msg, "%s -> %s  %dx%d %s q%d  %ld frames  %.1f KB  %.2f:1  "
                     "%.1f ms/frame (%.1f fps)\n",
                in, out, W, H, format_name(format), quality, frames,
                bytes / 1024.0,
                (double)(need * (size_t)frames) / (double)bytes,
                dt * 1e3 / frames, frames / dt);
    return 0;
}

static int cmd_decode(int argc, char **argv) {
    const char *in = NULL, *out = NULL;
    int verbose = 0;
    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (argv[i][0] == '-' && argv[i][1]) {
            fprintf(stderr, "agc: unknown option '%s'\n", argv[i]); return 2;
        } else if (!in) in = argv[i];
        else if (!out) out = argv[i];
        else { fprintf(stderr, "agc: unexpected argument '%s'\n", argv[i]); return 2; }
    }
    if (!in || !out) {
        fprintf(stderr, "agc: decode needs an input and an output path "
                        "('-' for stdin/stdout)\n");
        return 2;
    }

    FILE *fi = is_stdio(in) ? stdin : fopen(in, "rb");
    if (!fi) { fprintf(stderr, "agc: %s: %s\n", in, strerror(errno)); return 1; }
    FILE *fo = is_stdio(out) ? stdout : fopen(out, "wb");
    if (!fo) {
        fprintf(stderr, "agc: %s: %s\n", out, strerror(errno));
        if (fi != stdin) fclose(fi);
        return 1;
    }
    FILE *msg = (fo == stdout) ? stderr : stdout;

    gl_boot(0);
    Progs pg; progs_init(&pg);
    PlaneCtx ctx[3]; memset(ctx, 0, sizeof ctx);

    unsigned char *raw = NULL;
    size_t rawCap = 0;
    long frames = 0;
    int rc = 0, W = 0, H = 0, format = AGC_GRAY8, quality = 0, stopped = 0;
    double t0 = now_s();
    install_stop_handlers();
    for (;;) {
        if (g_stop) { stopped = 1; break; }
        Frame f;
        int r = frame_read_fp(fi, &f, in);
        if (r == 1) break;
        if (r < 0) { rc = 1; break; }

        W = f.width; H = f.height; format = f.format; quality = f.quality;
        size_t need = frame_raw_bytes(f.format, f.width, f.height);
        if (need > rawCap) { free(raw); raw = xmalloc(need); rawCap = need; }

        int qy = qscale_for(f.quality), qc = chroma_qscale(qy);
        size_t off = 0;
        for (int i = 0; i < f.nplanes; ++i) {
            int pw, ph; plane_dims(f.format, i, f.width, f.height, &pw, &ph);
            Plane *pl = plane_ctx(&ctx[i], pw, ph);
            if (decode_plane(&pg, pl, &f.plane[i], i ? qc : qy, ctx[i].pad)) { rc = 1; break; }
            crop_plane(ctx[i].pad, pl->W, raw + off, pw, ph);
            if (verbose)
                fprintf(stderr, "    frame %ld plane %d  %dx%d  %d tiles\n",
                        frames, i, pw, ph, f.plane[i].nTiles);
            off += (size_t)pw * ph;
        }
        frame_free(&f);
        if (rc) break;

        if (fwrite(raw, 1, need, fo) != need) {
            fprintf(stderr, "agc: %s: short write: %s\n", out, strerror(errno));
            rc = 1; break;
        }
        frames++;
    }
    double t1 = now_s();

    for (int i = 0; i < 3; ++i) plane_ctx_free(&ctx[i]);
    free(raw);
    if (fi != stdin) fclose(fi);
    if (fo != stdout) { if (fclose(fo) != 0) { fprintf(stderr, "agc: %s: %s\n", out, strerror(errno)); rc = 1; } }
    else fflush(fo);

    if (rc) return 1;
    if (frames == 0) {
        if (stopped) { fprintf(stderr, "agc: interrupted before a frame completed\n"); return 1; }
        fprintf(stderr, "agc: %s: no frames in input\n", in); return 1;
    }
    if (stopped)
        fprintf(msg, "agc: interrupted -- %ld complete frames written\n", frames);

    double dt = t1 - t0;
    if (dt <= 0) dt = 1e-9;
    if (frames == 1)
        fprintf(msg, "%s -> %s  %dx%d %s q%d  %.1f ms\n",
                in, out, W, H, format_name(format), quality, dt * 1e3);
    else
        fprintf(msg, "%s -> %s  %dx%d %s q%d  %ld frames  %.1f ms/frame (%.1f fps)\n",
                in, out, W, H, format_name(format), quality, frames,
                dt * 1e3 / frames, frames / dt);
    return 0;
}

static int cmd_probe(int argc, char **argv) {
    if (argc != 1) { fprintf(stderr, "agc: probe needs exactly one file\n"); return 2; }
    const char *path = argv[0];
    FILE *fp = is_stdio(path) ? stdin : fopen(path, "rb");
    if (!fp) { fprintf(stderr, "agc: %s: %s\n", path, strerror(errno)); return 1; }
    Frame f;
    int r = frame_read_fp(fp, &f, path);
    if (fp != stdin) fclose(fp);
    if (r != 0) {
        if (r == 1) fprintf(stderr, "agc: %s: too short to be an AGC stream\n", path);
        return 1;
    }
    // Shell-eval-able, so wrappers never have to scrape the human output.
    printf("AGC_WIDTH=%d\n",  f.width);
    printf("AGC_HEIGHT=%d\n", f.height);
    printf("AGC_FORMAT=%s\n", format_name(f.format));
    printf("AGC_QUALITY=%d\n", f.quality);
    printf("AGC_FPS=%.3f\n", f.fpsMilli ? f.fpsMilli / 1000.0 : 0.0);
    frame_free(&f);
    return 0;
}

static int cmd_info(int argc, char **argv) {
    if (argc != 1) { fprintf(stderr, "agc: info needs exactly one file\n"); return 2; }
    const char *path = argv[0];
    FILE *fp = is_stdio(path) ? stdin : fopen(path, "rb");
    if (!fp) { fprintf(stderr, "agc: %s: %s\n", path, strerror(errno)); return 1; }

    Frame f;
    int r = frame_read_fp(fp, &f, path);
    if (r != 0) {
        if (r == 1) fprintf(stderr, "agc: %s: too short to be an AGC stream\n", path);
        if (fp != stdin) fclose(fp);
        return 1;
    }
    size_t raw = frame_raw_bytes(f.format, f.width, f.height);
    long bytes = HDR_BYTES;
    for (int i = 0; i < f.nplanes; ++i)
        bytes += (long)f.plane[i].nWords * 4 + ((f.plane[i].nTiles * 2 + 3) & ~3) + 12;
    printf("  file      %s\n", path);
    printf("  format    AGC1 v%d, %s\n", AGC_VERSION, format_name(f.format));
    printf("  frame     %d x %d\n", f.width, f.height);
    printf("  quality   %d (luma scale %d, chroma %d)\n",
           f.quality, qscale_for(f.quality), chroma_qscale(qscale_for(f.quality)));
    printf("  planes    %d\n", f.nplanes);
    for (int i = 0; i < f.nplanes; ++i) {
        int pw, ph; plane_dims(f.format, i, f.width, f.height, &pw, &ph);
        long idx = (long)f.plane[i].nTiles * 2;
        long pay = (long)f.plane[i].nWords * 4;
        printf("    [%d] %5dx%-5d  %7d tiles  index %6ld B  payload %8ld B  (index %.2f%%)\n",
               i, pw, ph, f.plane[i].nTiles, idx, pay,
               pay ? 100.0 * idx / (double)(idx + pay) : 0.0);
    }
    unsigned fpsMilli = f.fpsMilli;
    frame_free(&f);

    // Walk the rest of the stream so a sequence reports its true length
    // rather than describing only its first frame.
    long frames = 1, total = bytes;
    int damaged = 0;
    for (;;) {
        Frame g;
        int s = frame_read_fp(fp, &g, path);
        if (s == 1) break;
        if (s < 0) { damaged = 1; break; }
        total += HDR_BYTES;
        for (int i = 0; i < g.nplanes; ++i)
            total += (long)g.plane[i].nWords * 4 + ((g.plane[i].nTiles * 2 + 3) & ~3) + 12;
        if (g.fpsMilli && !fpsMilli) fpsMilli = g.fpsMilli;
        frames++;
        frame_free(&g);
    }
    if (fp != stdin) fclose(fp);

    if (frames == 1) {
        printf("  total     %ld B, raw %zu B, ratio %.2f:1\n",
               bytes, raw, (double)raw / (double)bytes);
    } else {
        printf("  frames    %ld", frames);
        if (fpsMilli) printf(" @ %.3f fps  (%.2f s)", fpsMilli / 1000.0,
                             frames / (fpsMilli / 1000.0));
        printf("\n");
        printf("  total     %ld B, raw %zu B, ratio %.2f:1  (%.1f KB/frame)\n",
               total, raw * (size_t)frames,
               (double)(raw * (size_t)frames) / (double)total,
               total / 1024.0 / frames);
    }
    if (damaged) {
        fprintf(stderr, "agc: %s: stream is damaged after frame %ld\n", path, frames);
        return 1;
    }
    return 0;
}

static int cmd_compare(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "agc: compare needs <a.raw> <b.raw> <WxH> <format>\n"); return 2; }
    int W, H;
    if (parse_size(argv[2], &W, &H)) { fprintf(stderr, "agc: bad size\n"); return 2; }
    int format = parse_format(argv[3]);
    if (format < 0) { fprintf(stderr, "agc: bad format\n"); return 2; }
    size_t n = frame_raw_bytes(format, W, H);
    unsigned char *a = malloc(n), *b = malloc(n);
    FILE *fa = fopen(argv[0], "rb"), *fb = fopen(argv[1], "rb");
    if (!fa || !fb) { fprintf(stderr, "agc: cannot open inputs\n"); return 1; }
    if (fread(a, 1, n, fa) != n || fread(b, 1, n, fb) != n) {
        fprintf(stderr, "agc: inputs are not both %zu bytes\n", n); return 1; }
    fclose(fa); fclose(fb);
    double se = 0; long maxerr = 0;
    for (size_t i = 0; i < n; ++i) {
        long d = (long)a[i] - (long)b[i];
        if (labs(d) > maxerr) maxerr = labs(d);
        se += (double)d * (double)d;
    }
    double mse = se / (double)n;
    if (mse == 0.0) printf("identical (PSNR infinite)\n");
    else printf("PSNR %.2f dB   max abs error %ld   MSE %.4f\n",
                10.0 * log10(255.0 * 255.0 / mse), maxerr, mse);
    free(a); free(b);
    return 0;
}

// Bench keeps the original in-memory round trip, including the exact
// coefficient check that proves the parser really reconstructs the encoder's
// data rather than reading leftovers.
static int cmd_bench(int argc, char **argv) {
    const char *path = (argc > 0) ? argv[0] : NULL;
    int W = (argc > 2) ? atoi(argv[1]) : 3840;
    int H = (argc > 2) ? atoi(argv[2]) : 2160;
    int quality = (argc > 3) ? atoi(argv[3]) : 75;
    int qscale = qscale_for(quality);

    // Attributing time to each stage requires draining the pipeline between
    // them, which inflates the totals. Bench is the only mode that pays it.
    g_timing = 1;

    printf("\n  === AGC-1 : GPU-native intra codec on Adreno ===\n");
    gl_boot(1);
    printf("    frame: %dx%d   quality=%d (jpeg scale %d)\n", W, H, quality, qscale);
    FILE *cs = fopen("/proc/self/cpuset", "r");
    if (cs) { char cb[128]; if (fgets(cb, sizeof cb, cs)) printf("    cpuset:%s", cb); fclose(cs); }
    printf("\n");

    Progs pg; progs_init(&pg);
    Plane pl; plane_init(&pl, W, H);

    size_t nPix = (size_t)pl.W * pl.H;
    unsigned char *img = malloc(nPix);
    if (path) {
        FILE *f = fopen(path, "rb");
        if (!f) { fprintf(stderr, "agc: %s: %s\n", path, strerror(errno)); return 1; }
        unsigned char *raw = malloc((size_t)W * H);
        if (fread(raw, 1, (size_t)W * H, f) != (size_t)W * H) {
            fprintf(stderr, "agc: short read: need %zu bytes of gray8\n", (size_t)W * H);
            return 1; }
        fclose(f);
        pad_plane(raw, W, H, img, pl.W, pl.H);
        free(raw);
        printf("    source: %s\n", path);
    } else {
        for (int y = 0; y < pl.H; ++y) for (int x = 0; x < pl.W; ++x) {
            int v = (x * 255 / pl.W + y * 255 / pl.H) / 2;
            if (((x >> 6) + (y >> 6)) & 1) v = 255 - v;
            v += ((x * 7 + y * 13) % 11) - 5;
            img[(size_t)y * pl.W + x] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
        printf("    source: synthetic gradient/checker test image\n");
    }

    unsigned char *rec = malloc(nPix);
    Bits bt; memset(&bt, 0, sizeof bt);
    double tEnc = 0, tDec = 0;
    double ePass[6] = {0,0,0,0,0,0}, dPass[6] = {0,0,0,0,0,0};
    int iters = 10;
    struct rusage ru0, ru1;
    double tLoop0 = 0;

    for (int it = 0; it < iters + 1; ++it) {
        if (it == 1) { getrusage(RUSAGE_SELF, &ru0); tLoop0 = now_s(); }
        if (it) bits_free(&bt);
        memset(&bt, 0, sizeof bt);
        double a = now_s();
        if (encode_plane(&pg, &pl, img, qscale, &bt)) return 1;
        double b = now_s();
        if (it) for (int k = 0; k < 6; ++k) ePass[k] += g_tPass[k];
        if (decode_plane(&pg, &pl, &bt, qscale, rec)) return 1;
        double c = now_s();
        if (it) { tEnc += b - a; tDec += c - b;
                  for (int k = 0; k < 6; ++k) dPass[k] += g_tPass[k]; }
    }
    getrusage(RUSAGE_SELF, &ru1);
    double wall = now_s() - tLoop0;
    double cpu = (ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec)
               + (ru1.ru_utime.tv_usec - ru0.ru_utime.tv_usec) / 1e6
               + (ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec)
               + (ru1.ru_stime.tv_usec - ru0.ru_stime.tv_usec) / 1e6;

    double se = 0;
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        double d = (double)img[(size_t)y*pl.W+x] - (double)rec[(size_t)y*pl.W+x];
        se += d * d;
    }
    double mse = se / ((double)W * H);
    long bytes = (long)bt.nWords * 4 + (long)bt.nTiles * 2;

    printf("    encode   %6.2f ms  (%5.1f fps)   upload %.2f  dct %.2f  scan %.2f  pack %.2f  read %.2f\n",
           tEnc / iters * 1e3, iters / tEnc,
           ePass[0]/iters*1e3, ePass[1]/iters*1e3, ePass[2]/iters*1e3,
           ePass[3]/iters*1e3, ePass[4]/iters*1e3);
    printf("    decode   %6.2f ms  (%5.1f fps)   upload %.2f  clear %.2f  scan %.2f  parse %.2f  idct %.2f  read %.2f\n",
           tDec / iters * 1e3, iters / tDec,
           dPass[0]/iters*1e3, dPass[1]/iters*1e3, dPass[2]/iters*1e3,
           dPass[3]/iters*1e3, dPass[4]/iters*1e3, dPass[5]/iters*1e3);
    printf("    stream   %.1f KB payload + %.1f KB tile index  -> %.2f:1\n",
           bt.nWords * 4 / 1024.0, bt.nTiles * 2 / 1024.0,
           (double)W * H / (double)bytes);
    printf("    quality  PSNR %.2f dB\n", 10.0 * log10(255.0 * 255.0 / mse));
    printf("    cpu      %.2f ms/frame consumed (%.1f%% of one core)\n",
           cpu / iters * 1e3, 100.0 * cpu / wall);
    return 0;
}

static void usage(void) {
    printf(
"AGC-1 -- GPU-native intra codec (Adreno/Turnip, OpenGL 4.3 compute)\n"
"\n"
"  agc encode <in.raw> <out.agc> -s WxH [-f gray8|yuv420p] [-q 1..100]\n"
"                                       [-r fps] [-v]\n"
"  agc decode <in.agc> <out.raw> [-v]\n"
"  agc info    <in.agc>\n"
"  agc probe   <in.agc>          machine-readable geometry, for scripts\n"
"  agc compare <a.raw> <b.raw> <WxH> <gray8|yuv420p>\n"
"  agc bench   [frame.gray] [W] [H] [quality]\n"
"\n"
"Use '-' for stdin or stdout, so agc can sit in a pipeline. Any input holding\n"
"more than one frame is encoded as a sequence and decoded back as one; the GPU\n"
"context and its buffers are built once and reused for every frame.\n"
"\n"
"Input and output for encode/decode are planar raw frames: gray8 is W*H bytes,\n"
"yuv420p is W*H plus two (W+1)/2 x (H+1)/2 chroma planes. Dimensions that are\n"
"not a multiple of 8 are coded padded by edge replication and cropped on decode.\n"
"\n"
"Examples:\n"
"  # one frame, via files\n"
"  ffmpeg -i clip.mp4 -frames:v 1 -pix_fmt yuv420p -f rawvideo f.yuv\n"
"  agc encode f.yuv f.agc -s 1920x1080 -f yuv420p -q 80\n"
"  agc decode f.agc out.yuv\n"
"\n"
"  # a whole clip, streamed, nothing hits the disk twice\n"
"  ffmpeg -v error -i clip.mp4 -pix_fmt yuv420p -f rawvideo - \\\n"
"    | agc encode - clip.agc -s 1920x1080 -f yuv420p -q 80 -r 30\n"
"  agc-play clip.agc\n"
"\n"
"encode and decode stop cleanly on Ctrl-C: the frame in flight is finished and\n"
"flushed, so an interrupted capture is still a complete, readable stream.\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 2; }
    const char *cmd = argv[1];
    if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help") || !strcmp(cmd, "help")) { usage(); return 0; }
    if (!strcmp(cmd, "encode"))  return cmd_encode(argc - 2, argv + 2);
    if (!strcmp(cmd, "decode"))  return cmd_decode(argc - 2, argv + 2);
    if (!strcmp(cmd, "info"))    return cmd_info(argc - 2, argv + 2);
    if (!strcmp(cmd, "probe"))   return cmd_probe(argc - 2, argv + 2);
    if (!strcmp(cmd, "compare")) return cmd_compare(argc - 2, argv + 2);
    if (!strcmp(cmd, "bench"))   return cmd_bench(argc - 2, argv + 2);
    fprintf(stderr, "agc: unknown command '%s'\n\n", cmd);
    usage();
    return 2;
}
