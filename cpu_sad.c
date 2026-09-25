// CPU baseline for the identical exhaustive 16x16 SAD motion search the GPU
// kernel performs, so the comparison is apples-to-apples. Auto-vectorised and
// OpenMP-parallel, i.e. a fair rather than a strawman CPU implementation.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <omp.h>

static double now_s(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

int main(void) {
    const int W = 3840, H = 2160;
    const int bxS = W / 16, byS = H / 16, nBlk = bxS * byS;
    unsigned char *cur = malloc((size_t)W * H), *ref = malloc((size_t)W * H);
    for (size_t i = 0; i < (size_t)W * H; ++i) {
        cur[i] = (unsigned char)(i * 37u); ref[i] = (unsigned char)(i * 91u);
    }
    unsigned *mv = malloc((size_t)nBlk * 4);

    printf("    threads available: %d   cpuset=", omp_get_max_threads());
    fflush(stdout); system("cat /proc/self/cpuset");

    double best = 1e30;
    for (int rep = 0; rep < 3; ++rep) {
        double t0 = now_s();
#pragma omp parallel for schedule(static)
        for (int blk = 0; blk < nBlk; ++blk) {
            int bx = (blk % bxS) * 16, by = (blk / bxS) * 16;
            unsigned bc = 0xFFFFFFFFu, bi = 0;
            for (int c = 0; c < 289; ++c) {
                int dx = (c % 17) - 8, dy = (c / 17) - 8;
                int rx = bx + dx, ry = by + dy;
                if (rx < 0 || ry < 0 || rx + 16 > W || ry + 16 > H) continue;
                unsigned sad = 0;
                for (int y = 0; y < 16; ++y) {
                    const unsigned char *a = cur + (size_t)(by + y) * W + bx;
                    const unsigned char *b = ref + (size_t)(ry + y) * W + rx;
                    for (int x = 0; x < 16; ++x) {
                        int d = (int)a[x] - (int)b[x];
                        sad += (unsigned)(d < 0 ? -d : d);
                    }
                }
                if (sad < bc) { bc = sad; bi = (unsigned)c; }
            }
            mv[blk] = bi;
        }
        double dt = now_s() - t0;
        if (dt < best) best = dt;
    }
    double ops = (double)nBlk * 289.0 * 256.0;
    printf("    CPU SAD ME 16x16 +/-8 4K : %7.2f ms  -> %6.2f fps   (%.2f G SAD/s)\n",
           best * 1e3, 1.0 / best, ops / best / 1e9);
    return 0;
}
