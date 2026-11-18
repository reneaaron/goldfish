#include "page_reader.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "seedqr.h"
#include "word_reader.h"

// ---------------------------------------------------------------- paper and ink
#define BLK 16
static const uint8_t *img;
static int W, H, gw, gh;
static uint8_t *bg;   // paper brightness per BLK x BLK block

static void paper_levels() {
    uint16_t hist[256];
    for (int by = 0; by < gh; by++)
        for (int bx = 0; bx < gw; bx++) {
            memset(hist, 0, sizeof(hist));
            int n = 0;
            for (int y = by * BLK; y < (by + 1) * BLK && y < H; y++)
                for (int x = bx * BLK; x < (bx + 1) * BLK && x < W; x++) hist[img[y * W + x]]++, n++;
            int k = n * 9 / 10, v = 0;   // 90th percentile: paper, even where there's ink
            for (int seen = 0; v < 255 && (seen += hist[v]) <= k; v++) {}
            bg[by * gw + bx] = uint8_t(v);
        }
}

static inline int paper_at(int x, int y) {
    float fx = (x + 0.5f) / BLK - 0.5f, fy = (y + 0.5f) / BLK - 0.5f;
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    float ax = fx - x0, ay = fy - y0;
    int x1 = x0 + 1, y1 = y0 + 1;
    x0 = x0 < 0 ? 0 : x0 >= gw ? gw - 1 : x0;
    x1 = x1 < 0 ? 0 : x1 >= gw ? gw - 1 : x1;
    y0 = y0 < 0 ? 0 : y0 >= gh ? gh - 1 : y0;
    y1 = y1 < 0 ? 0 : y1 >= gh ? gh - 1 : y1;
    float top = bg[y0 * gw + x0] * (1 - ax) + bg[y0 * gw + x1] * ax;
    float bot = bg[y1 * gw + x0] * (1 - ax) + bg[y1 * gw + x1] * ax;
    return int(top * (1 - ay) + bot * ay);
}

// Ink: clearly darker than the paper around it (pencil included).
static inline bool ink(int x, int y) {
    int p = paper_at(x, y);
    int d = p / 5 > 20 ? p / 5 : 20;
    return p > 60 && img[y * W + x] < p - d;
}

// ---------------------------------------------------------------- tilt
#define MAX_POINTS 8000

static float line_up_score(const int16_t *pts, int n, float a, int32_t *bins, int nb) {
    float c = cosf(a), s = sinf(a);
    memset(bins, 0, sizeof(int32_t) * nb);
    for (int i = 0; i < n; i++) {
        int b = int((-pts[2 * i] * s + pts[2 * i + 1] * c + W) / 2);
        if (b >= 0 && b < nb) bins[b]++;
    }
    float sc = 0;
    for (int i = 0; i < nb; i++) sc += (float)bins[i] * bins[i];
    return sc;
}

// The tilt (radians, within +-15 degrees) at which the ink's rows are sharpest.
static float find_tilt() {
    int total = 0;
    for (int y = 0; y < H; y += 2)
        for (int x = 0; x < W; x += 2) total += ink(x, y);
    if (total < 50) return 0;
    int stride = 2;
    while (total * 4 / (stride * stride) > MAX_POINTS) stride++;
    int16_t *pts = (int16_t *)malloc(sizeof(int16_t) * 2 * MAX_POINTS);
    int nb = (W + H) / 2 + 2;
    int32_t *bins = (int32_t *)malloc(sizeof(int32_t) * nb);
    float best_a = 0;
    if (pts && bins) {
        int n = 0;
        for (int y = 0; y < H; y += stride)
            for (int x = 0; x < W && n < MAX_POINTS; x += stride)
                if (ink(x, y)) { pts[2 * n] = int16_t(x); pts[2 * n + 1] = int16_t(y); n++; }
        float best = -1;
        for (float d = -15; d <= 15.01f; d += 0.5f) {
            float sc = line_up_score(pts, n, d * (float)M_PI / 180, bins, nb);
            if (sc > best) { best = sc; best_a = d; }
        }
        float coarse = best_a;
        for (float d = coarse - 0.5f; d <= coarse + 0.51f; d += 0.1f) {
            float sc = line_up_score(pts, n, d * (float)M_PI / 180, bins, nb);
            if (sc > best) { best = sc; best_a = d; }
        }
        secure_zero(pts, sizeof(int16_t) * 2 * MAX_POINTS);
    }
    free(pts);
    free(bins);
    return best_a * (float)M_PI / 180;
}

// ---------------------------------------------------------------- blobs and words
// Runs of ink along the map's rows, joined into blobs (letters, or letters
// that touch), then blobs into words.
#define MAX_LETTERS 3000
#define MAX_HS 1024
struct Run { int16_t u0, u1, v; int parent; };
struct Blob { int u0, v0, u1, v1, n; };
// All on the heap for one page_find, sized to the image, wiped and freed after.
static Run *runs;
static Blob *blobs, *letters;
static int *lpar;
static int nruns, max_runs;

static int find_run(int a) {
    while (runs[a].parent != a) a = runs[a].parent = runs[runs[a].parent].parent;
    return a;
}
static void join(int a, int b) {
    a = find_run(a); b = find_run(b);
    if (a < b) runs[b].parent = a;
    else if (b < a) runs[a].parent = b;
}
static int lfind(int a) {
    while (lpar[a] != a) a = lpar[a] = lpar[lpar[a]];
    return a;
}
static void lunite(int a, int b) {
    a = lfind(a); b = lfind(b);
    if (a < b) lpar[b] = a;
    else if (b < a) lpar[a] = b;
}

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

bool page_find(const uint8_t *gray, int w, int h, Page *page) {
    memset(page, 0, sizeof(*page));
    img = gray; W = w; H = h;
    page->w = w; page->h = h;
    gw = (w + BLK - 1) / BLK; gh = (h + BLK - 1) / BLK;
    bg = (uint8_t *)malloc(gw * gh);
    if (!bg) return false;
    paper_levels();

    float a = find_tilt(), c = cosf(a), s = sinf(a);
    page->cos_a = c; page->sin_a = s;
    // v (down the page along the tilt) and u (across) ranges of the image
    float vmin = fminf(fminf(0, -w * s), fminf(h * c, -w * s + h * c));
    float vmax = fmaxf(fmaxf(0, -w * s), fmaxf(h * c, -w * s + h * c));
    float umin = fminf(fminf(0, w * c), fminf(h * s, w * c + h * s));
    float umax = fmaxf(fmaxf(0, w * c), fmaxf(h * s, w * c + h * s));
    int nv = int(vmax - vmin) + 2, nu = int(umax - umin) + 2, stride = (nu + 7) / 8;
    uint8_t *map = (uint8_t *)calloc((size_t)stride * nv, 1);   // ink along the tilt, 1 bit per pixel
    bool ok = false, overflow = false;
    nruns = 0;
    max_runs = nu * nv / 12 + 2000;   // far more runs than any page of words has
    if (max_runs > 80000) max_runs = 80000;
    runs = (Run *)malloc(sizeof(Run) * max_runs);
    blobs = (Blob *)malloc(sizeof(Blob) * max_runs);
    letters = (Blob *)malloc(sizeof(Blob) * MAX_LETTERS);
    lpar = (int *)malloc(sizeof(int) * MAX_LETTERS);
    if (!map || !runs || !blobs || !letters || !lpar) goto done;
#define MAP_GET(u, v) ((map[(size_t)(v) * stride + ((u) >> 3)] >> ((u) & 7)) & 1)
#define MAP_CLR(u, v) (map[(size_t)(v) * stride + ((u) >> 3)] &= uint8_t(~(1 << ((u) & 7))))
    for (int v = 0; v < nv; v++)
        for (int u = 0; u < nu; u++) {
            float fu = u + umin, fv = v + vmin;
            int x = int(fu * c - fv * s), y = int(fu * s + fv * c);
            if (x >= 0 && x < w && y >= 0 && y < h && ink(x, y)) map[(size_t)v * stride + (u >> 3)] |= uint8_t(1 << (u & 7));
        }
    {
        // Lines are not words: erase runs of ink far longer than a letter.
        // Across: ruled lines (they'd join every row to the next). Down: a
        // notebook's margin, the paper's edge, a binding (they'd add ink to
        // every row). The strips come from the photo, lines and all, as in
        // training; this only decides where the words are.
        int lh = nu / 8 > 24 ? nu / 8 : 24, lv = nv / 6 > 40 ? nv / 6 : 40;
        for (int v = 0; v < nv; v++)
            for (int u = 0; u < nu;) {
                if (!MAP_GET(u, v)) { u++; continue; }
                int e = u;
                while (e < nu && MAP_GET(e, v)) e++;
                if (e - u >= lh) for (int k = u; k < e; k++) MAP_CLR(k, v);
                u = e;
            }
        for (int u = 0; u < nu; u++)
            for (int v = 0; v < nv;) {
                if (!MAP_GET(u, v)) { v++; continue; }
                int e = v;
                while (e < nv && MAP_GET(u, e)) e++;
                if (e - v >= lv) for (int k = v; k < e; k++) MAP_CLR(u, k);
                v = e;
            }
    }
    {
        // Blobs of ink: runs along each map row, joined where they touch a
        // run on the row above (8-connected), with union-find.
        nruns = 0;
        int prev0 = 0, prev1 = 0;   // runs of the previous row: [prev0, prev1)
        for (int v = 0; v < nv && !overflow; v++) {
            int row0 = nruns;
            for (int u = 0; u < nu && !overflow;) {
                if (!MAP_GET(u, v)) { u++; continue; }
                int e = u;
                while (e < nu && MAP_GET(e, v)) e++;
                if (nruns == max_runs) { overflow = true; break; }
                Run &r = runs[nruns];
                r.u0 = int16_t(u); r.u1 = int16_t(e - 1); r.v = int16_t(v); r.parent = nruns;
                for (int k = prev0; k < prev1; k++)
                    if (runs[k].u0 <= e && runs[k].u1 >= u - 1) join(k, nruns);
                nruns++;
                u = e;
            }
            prev0 = row0; prev1 = nruns;
        }
        if (overflow || !nruns) goto done;
        // blob boxes, on the roots
        for (int i = 0; i < nruns; i++) {
            Run &r = runs[i];
            int root = find_run(i);
            Blob &b = blobs[root];
            if (root == i) b = {r.u0, r.v, r.u1, r.v, 0};
            if (r.u0 < b.u0) b.u0 = r.u0;
            if (r.u1 > b.u1) b.u1 = r.u1;
            if (r.v < b.v0) b.v0 = r.v;
            if (r.v > b.v1) b.v1 = r.v;
            b.n += r.u1 - r.u0 + 1;
        }
        // the letter height: median height of blobs that aren't specks
        int hs[MAX_HS], nh = 0;
        for (int i = 0; i < nruns && nh < MAX_HS; i++)
            if (find_run(i) == i && blobs[i].n >= 8 && blobs[i].v1 - blobs[i].v0 >= 3) hs[nh++] = blobs[i].v1 - blobs[i].v0 + 1;
        if (!nh) goto done;
        qsort(hs, nh, sizeof(int), cmp_int);
        int lh = hs[nh / 2];
        // letters: not specks, not pictures
        int nl = 0;
        for (int i = 0; i < nruns && nl < MAX_LETTERS; i++) {
            if (find_run(i) != i) continue;
            const Blob &b = blobs[i];
            int bh = b.v1 - b.v0 + 1, bw = b.u1 - b.u0 + 1;
            if (bh < lh * 3 / 10 || b.n < 6 || bh > lh * 3 || bw > lh * 12) continue;
            letters[nl++] = b;
        }
        // words: letters that overlap vertically and sit closer than ~a letter apart
        for (int i = 0; i < nl; i++) lpar[i] = i;
        for (int i = 0; i < nl; i++)
            for (int j = 0; j < nl; j++) {
                if (i == j) continue;
                const Blob &p = letters[i], &q = letters[j];
                int gap = q.u0 - p.u1;
                if (gap < -lh || gap > lh * 9 / 10) continue;
                int ov = (p.v1 < q.v1 ? p.v1 : q.v1) - (p.v0 > q.v0 ? p.v0 : q.v0);
                int mh = (p.v1 - p.v0) < (q.v1 - q.v0) ? (p.v1 - p.v0) : (q.v1 - q.v0);
                if (ov * 5 >= mh * 2) lunite(i, j);
            }
        for (int i = 0; i < nl && page->n < PAGE_MAX_WORDS; i++) {
            if (lfind(i) != i) continue;
            Blob wb = letters[i];
            for (int j = 0; j < nl; j++)
                if (j != i && lfind(j) == i) {
                    const Blob &q = letters[j];
                    if (q.u0 < wb.u0) wb.u0 = q.u0;
                    if (q.u1 > wb.u1) wb.u1 = q.u1;
                    if (q.v0 < wb.v0) wb.v0 = q.v0;
                    if (q.v1 > wb.v1) wb.v1 = q.v1;
                }
            int ww = wb.u1 - wb.u0 + 1, wh = wb.v1 - wb.v0 + 1;
            if (ww < lh * 8 / 10 || wh < lh / 2) continue;   // a lone speck or dash
            PageWord &pw = page->words[page->n++];
            pw.u0 = wb.u0 + umin; pw.u1 = wb.u1 + 1 + umin;
            pw.v0 = wb.v0 + vmin; pw.v1 = wb.v1 + 1 + vmin;
        }
        if (!page->n) goto done;

        // A row number written a little apart ("11.   MEDIA") would be a word
        // of its own that can't be read: join a short piece to the word just
        // after it on the same line (the reader expects numbers in front).
        for (int i = 0; i < page->n; i++) {
            PageWord &a = page->words[i];
            if (a.u1 - a.u0 > 2.5f * (a.v1 - a.v0)) continue;   // a word, not a number
            int best = -1;
            float best_gap = 0;
            for (int j = 0; j < page->n; j++) {
                const PageWord &b = page->words[j];
                if (j == i || b.u0 < a.u1) continue;
                float ov = fminf(a.v1, b.v1) - fmaxf(a.v0, b.v0);
                if (ov < 0.4f * fminf(a.v1 - a.v0, b.v1 - b.v0)) continue;
                float gap = b.u0 - a.u1;   // closer than the word is tall
                if (gap < b.v1 - b.v0 && (best < 0 || gap < best_gap)) { best_gap = gap; best = j; }
            }
            if (best < 0) continue;
            PageWord &b = page->words[best];
            b.u0 = a.u0;
            b.v0 = fminf(a.v0, b.v0);
            b.v1 = fmaxf(a.v1, b.v1);
            page->words[i] = page->words[--page->n];
            i = -1;   // indexes moved: start over (a page has few words)
        }
    }

    {
        // a lone row number or a speck is narrower than any word
        int k = 0;
        for (int i = 0; i < page->n; i++)
            if (page->words[i].u1 - page->words[i].u0 >= 1.2f * (page->words[i].v1 - page->words[i].v0))
                page->words[k++] = page->words[i];
        page->n = k;
        if (!page->n) goto done;
    }
    {
        // columns: where words sit across all rows, merged where they overlap
        static float cu0[PAGE_MAX_WORDS], cu1[PAGE_MAX_WORDS];
        int nc = 0;
        int idx[PAGE_MAX_WORDS];
        for (int i = 0; i < page->n; i++) idx[i] = i;
        for (int i = 1; i < page->n; i++)
            for (int j = i; j > 0 && page->words[idx[j]].u0 < page->words[idx[j - 1]].u0; j--) {
                int t2 = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t2;
            }
        for (int k = 0; k < page->n; k++) {
            const PageWord &pw = page->words[idx[k]];
            if (nc && pw.u0 <= cu1[nc - 1]) { if (pw.u1 > cu1[nc - 1]) cu1[nc - 1] = pw.u1; }
            else { cu0[nc] = pw.u0; cu1[nc] = pw.u1; nc++; }
        }
        for (int i = 0; i < page->n; i++) {
            PageWord &pw = page->words[i];
            float mid = (pw.u0 + pw.u1) / 2;
            for (int k = 0; k < nc; k++)
                if (mid >= cu0[k] && mid <= cu1[k]) pw.column = k;
        }
        // reading order: column, then down the page
        for (int i = 1; i < page->n; i++)
            for (int j = i; j > 0; j--) {
                PageWord &p0 = page->words[j - 1], &p1 = page->words[j];
                bool before = p1.column < p0.column || (p1.column == p0.column && p1.v0 < p0.v0);
                if (!before) break;
                PageWord t2 = p0; p0 = p1; p1 = t2;
            }

        // Gaps: a column's words sit at a steady row spacing; a step of about
        // two (or three) rows is a word that wasn't found (too faint, or broken
        // into specks). Put a guessed word there, so it gets a picture and a
        // place in the list instead of every later word moving up one. Only in
        // columns of at least 4 words, and for at most 2 missing in a row.
        static PageWord out[PAGE_MAX_WORDS];
        int no = 0;
        // the longest column, and how far down its words go: a shorter column
        // ending higher up is missing its last words
        int longest = 0;
        float bottom = 0;
        for (int i = 0; i < page->n;) {
            int j = i;
            while (j < page->n && page->words[j].column == page->words[i].column) j++;
            if (j - i > longest) longest = j - i;
            for (int k = i; k < j; k++) bottom = fmaxf(bottom, page->words[k].v1);
            i = j;
        }
        for (int i = 0; i < page->n;) {
            int j = i;
            while (j < page->n && page->words[j].column == page->words[i].column) j++;
            int cn = j - i;
            float steps[PAGE_MAX_WORDS], hts[PAGE_MAX_WORDS], u0s[PAGE_MAX_WORDS];
            for (int k = 0; k < cn; k++) {
                const PageWord &q = page->words[i + k];
                hts[k] = q.v1 - q.v0;
                u0s[k] = q.u0;
                if (k) steps[k - 1] = (q.v0 + q.v1) / 2 - (page->words[i + k - 1].v0 + page->words[i + k - 1].v1) / 2;
            }
            float step = 0, ht = 0, u0 = 0;
            if (cn >= 4) {
                qsort(steps, cn - 1, sizeof(float), cmp_float);
                qsort(hts, cn, sizeof(float), cmp_float);
                qsort(u0s, cn, sizeof(float), cmp_float);
                step = steps[(cn - 1) / 2]; ht = hts[cn / 2]; u0 = u0s[cn / 2];
            }
            for (int k = i; k < j && no < PAGE_MAX_WORDS; k++) {
                if (k > i && step > 0) {
                    const PageWord &a = page->words[k - 1], &b = page->words[k];
                    float gap = (b.v0 + b.v1) / 2 - (a.v0 + a.v1) / 2;
                    int missing = int(gap / step + 0.5f) - 1;
                    if (gap >= 1.6f * step && missing >= 1 && missing <= 2)
                        for (int m = 1; m <= missing && no < PAGE_MAX_WORDS; m++) {
                            PageWord g = {};
                            float vc = (a.v0 + a.v1) / 2 + gap * m / (missing + 1);
                            g.u0 = u0; g.u1 = fmaxf(a.u1, b.u1);
                            g.v0 = vc - ht / 2; g.v1 = vc + ht / 2;
                            g.column = a.column; g.guessed = true;
                            out[no++] = g;
                        }
                }
                if (no < PAGE_MAX_WORDS) out[no++] = page->words[k];
            }
            // the column's end: as long as the others, if there's room above their bottom
            const PageWord last = page->words[j - 1];
            for (int m = 1; step > 0 && m <= longest - cn && m <= 2 && no < PAGE_MAX_WORDS; m++) {
                float vc = (last.v0 + last.v1) / 2 + step * m;
                if (vc > bottom + step / 4) break;
                PageWord g = {};
                g.u0 = u0; g.u1 = last.u1;
                g.v0 = vc - ht / 2; g.v1 = vc + ht / 2;
                g.column = last.column; g.guessed = true;
                out[no++] = g;
            }
            i = j;
        }
        memcpy(page->words, out, sizeof(PageWord) * no);
        page->n = no;
        secure_zero(out, sizeof(out));
    }
    ok = true;

done:
    if (runs) { secure_zero(runs, sizeof(Run) * nruns); free(runs); }
    if (blobs) { secure_zero(blobs, sizeof(Blob) * nruns); free(blobs); }
    if (letters) { secure_zero(letters, sizeof(Blob) * MAX_LETTERS); free(letters); }
    free(lpar);
    runs = nullptr; blobs = letters = nullptr; lpar = nullptr;
    if (map) { secure_zero(map, (size_t)stride * nv); free(map); }
    if (bg) { secure_zero(bg, gw * gh); free(bg); bg = nullptr; }
    return ok;
}

// ---------------------------------------------------------------- strips
void page_strip(const uint8_t *gray, const Page *page, int i, uint8_t *strip) {
    const PageWord &pw = page->words[i];
    float c = page->cos_a, s = page->sin_a;
    float hgt = pw.v1 - pw.v0;
    float sh = hgt / 0.65f;                       // strip height in image pixels
    float v0 = (pw.v0 + pw.v1) / 2 - sh / 2;
    float u0 = pw.u0 - 0.3f * hgt, uw = (pw.u1 - pw.u0) + 0.6f * hgt;
    float scale = sh / WR_H;
    int sw = int(uw / scale + 0.5f);
    if (sw > WR_W) sw = WR_W;                     // squeeze a long row into the strip
    if (sw < 8) sw = 8;
    float sx = uw / sw;
    int k = int(ceilf(scale > sx ? scale : sx));  // samples per output pixel and axis
    if (k < 1) k = 1;
    if (k > 6) k = 6;
    static uint16_t hist[256];
    memset(hist, 0, sizeof(hist));
    for (int oy = 0; oy < WR_H; oy++)
        for (int ox = 0; ox < sw; ox++) {
            int sum = 0;
            for (int j = 0; j < k; j++)
                for (int q = 0; q < k; q++) {
                    float u = u0 + (ox + (q + 0.5f) / k) * sx, v = v0 + (oy + (j + 0.5f) / k) * scale;
                    int x = int(u * c - v * s), y = int(u * s + v * c);
                    x = x < 0 ? 0 : x >= page->w ? page->w - 1 : x;
                    y = y < 0 ? 0 : y >= page->h ? page->h - 1 : y;
                    sum += gray[y * page->w + x];
                }
            uint8_t g = uint8_t(sum / (k * k));
            strip[oy * WR_W + ox] = g;
            hist[g]++;
        }
    // the rest of the strip: plain paper, the strip's median
    int n = WR_H * sw, seen = 0, med = 0;
    for (; med < 255 && (seen += hist[med]) <= n / 2; med++) {}
    for (int oy = 0; oy < WR_H; oy++)
        for (int ox = sw; ox < WR_W; ox++) strip[oy * WR_W + ox] = uint8_t(med);
    secure_zero(hist, sizeof(hist));
}

void page_corners(const Page *page, int i, float xy[8]) {
    const PageWord &pw = page->words[i];
    float c = page->cos_a, s = page->sin_a;
    float uv[4][2] = {{pw.u0, pw.v0}, {pw.u1, pw.v0}, {pw.u1, pw.v1}, {pw.u0, pw.v1}};
    for (int k = 0; k < 4; k++) {
        xy[2 * k] = uv[k][0] * c - uv[k][1] * s;
        xy[2 * k + 1] = uv[k][0] * s + uv[k][1] * c;
    }
}

