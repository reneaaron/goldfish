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
            printf("], \"column\": %d, \"guessed\": %s, \"word\": \"%s\", \"margin\": %.2f, \"ok\": %s}",
                   page.words[i].column, page.words[i].guessed ? "true" : "false", r.word >= 0 ? seed_word(r.word) : "",
                   r.margin, ok ? "true" : "false");
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
