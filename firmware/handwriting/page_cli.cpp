// Run the firmware's page reader + word reader on photos, natively.
//   page_cli <max side> <image>...
// Each image is scaled down (box filter) so its longer side is at most
// <max side> pixels, like a camera of that resolution. One JSON line per
// image: the words found in reading order, their corners in the original
// image's pixels, the word read, its margin and whether it would be offered.
// PAGE_DUMP=<dir>: also write every strip as a PGM there.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "page_reader.h"
#include "seed_create.h"
#include "word_reader.h"
#define STB_IMAGE_IMPLEMENTATION
#include "../src/sim/stb_image.h"

int main(int argc, char **argv) {
    if (argc < 3 || !word_reader_init()) return 1;
    int max_side = atoi(argv[1]);
    const char *dump = getenv("PAGE_DUMP");
    bool topk = getenv("PAGE_TOPK") != nullptr;   // also each strip's WR_TOPK likeliest words
    static uint8_t strip[WR_H * WR_W];
    static Page page;
    for (int a = 2; a < argc; a++) {
        int w, h, n;
        uint8_t *img = stbi_load(argv[a], &w, &h, &n, 1);
        if (!img) { printf("{\"file\": \"%s\", \"error\": true}\n", argv[a]); continue; }
        int f = 1;
        while ((w + f - 1) / f > max_side || (h + f - 1) / f > max_side) f++;
        int sw = w / f, sh = h / f;
        uint8_t *g = (uint8_t *)malloc((size_t)sw * sh);
        for (int y = 0; y < sh; y++)
            for (int x = 0; x < sw; x++) {
                int sum = 0;
                for (int j = 0; j < f; j++)
                    for (int i = 0; i < f; i++) sum += img[(y * f + j) * w + x * f + i];
                g[y * sw + x] = uint8_t(sum / (f * f));
            }
        stbi_image_free(img);
        bool found = page_find(g, sw, sh, &page);
        printf("{\"file\": \"%s\", \"scale\": %d, \"size\": [%d, %d], \"words\": [", argv[a], f, sw, sh);
        for (int i = 0; found && i < page.n; i++) {
            page_strip(g, &page, i, strip);
            WordResult r;
            bool ok = word_reader_read(strip, nullptr, &r);
            float xy[8];
            page_corners(&page, i, xy);
            printf("%s{\"corners\": [", i ? ", " : "");
            for (int k = 0; k < 8; k++) printf("%s%.1f", k ? ", " : "", xy[k] * f);
            printf("], \"column\": %d, \"row\": %d, \"guessed\": %s, \"word\": \"%s\", \"margin\": %.2f, \"ok\": %s",
                   page.words[i].column, page.words[i].row, page.words[i].guessed ? "true" : "false", r.word >= 0 ? seed_word(r.word) : "",
                   r.margin, ok ? "true" : "false");
            if (topk) {
                int tw[WR_TOPK];
                float ts[WR_TOPK];
                int nt = word_reader_top(strip, nullptr, WR_TOPK, tw, ts);
                printf(", \"top\": [");
                for (int k = 0; k < nt; k++) printf("%s[\"%s\", %.3f]", k ? ", " : "", seed_word(tw[k]), ts[k]);
                printf("]");
                // greedy reading of the whole strip (shows the row number in front)
                static float lp[WR_T * WR_C];
                word_reader_logp(strip, lp, nullptr);
                const char *abc = "-ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.)";
                char gs[WR_T + 1];
                int ng = 0, prev = 0;
                for (int t = 0; t < WR_T; t++) {
                    int b = 0;
                    for (int c = 1; c < WR_C; c++) if (lp[t * WR_C + c] > lp[t * WR_C + b]) b = c;
                    if (b && b != prev) gs[ng++] = abc[b];
                    prev = b;
                }
                gs[ng] = 0;
                printf(", \"greedy\": \"%s\"", gs);
                // row numbers 1..24: best CTC path of the number's digits from the start, the rest free
                float rest[WR_T + 1];
                rest[WR_T] = 0;
                for (int t = WR_T - 1; t >= 0; t--) {
                    float m = lp[t * WR_C];
                    for (int c = 1; c < WR_C; c++) m = lp[t * WR_C + c] > m ? lp[t * WR_C + c] : m;
                    rest[t] = rest[t + 1] + m;
                }
                printf(", \"nums\": [");
                for (int nn = 1; nn <= 24; nn++) {
                    int lab[2], nl = 0;
                    if (nn >= 10) lab[nl++] = 27 + nn / 10;
                    lab[nl++] = 27 + nn % 10;
                    // states: 2*nl+1 (blank, d1, blank, d2, blank)
                    int S = 2 * nl + 1;
                    float a[5], b[5];
                    for (int k = 0; k < S; k++) a[k] = -1e30f;
                    a[0] = lp[0];
                    a[1] = lp[lab[0]];
                    float best = -1e30f;
                    for (int t = 0; t < WR_T; t++) {
                        if (t) {
                            for (int k = 0; k < S; k++) {
                                float v = a[k];
                                if (k > 0 && a[k - 1] > v) v = a[k - 1];
                                if (k > 1 && (k % 2) && lab[k / 2] != lab[k / 2 - 1] && a[k - 2] > v) v = a[k - 2];
                                int cls = (k % 2) ? lab[k / 2] : 0;
                                b[k] = v + lp[t * WR_C + cls];
                            }
                            for (int k = 0; k < S; k++) a[k] = b[k];
                        }
                        float done = a[S - 1] > a[S - 2] ? a[S - 1] : a[S - 2];   // number finished at t
                        if (done + rest[t + 1] > best) best = done + rest[t + 1];
                    }
                    printf("%s%.2f", nn > 1 ? ", " : "", best - rest[0]);
                }
                printf("]");
            }
            printf("}");
            if (dump) {
                char path[512];
                const char *base = strrchr(argv[a], '/');
                snprintf(path, sizeof(path), "%s/%s_%02d.pgm", dump, base ? base + 1 : argv[a], i);
                FILE *fp = fopen(path, "wb");
                if (fp) { fprintf(fp, "P5\n%d %d\n255\n", WR_W, WR_H); fwrite(strip, 1, sizeof(strip), fp); fclose(fp); }
            }
        }
        printf("]}\n");
        fflush(stdout);
        free(g);
    }
    word_reader_free();
    return 0;
}
