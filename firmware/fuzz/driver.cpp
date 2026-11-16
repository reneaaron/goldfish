// Minimal coverage-guided fuzzer for GCC, which has no libFuzzer.
// Harnesses use the libFuzzer interface (LLVMFuzzerTestOneInput), so with
// clang you can build them against -fsanitize=fuzzer instead and drop this file.
//
// Coverage: GCC's -fsanitize-coverage=trace-pc calls __sanitizer_cov_trace_pc
// on every basic block; we hash (prev, cur) into an AFL-style edge map.
//
//   ./fuzz_x corpus_dir [-t seconds] [-seed N]   fuzz, saving new inputs to corpus_dir
//   ./fuzz_x file...                             run each file once (reproduce)
#include <dirent.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <sanitizer/common_interface_defs.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
extern "C" __attribute__((weak)) int LLVMFuzzerInitialize(int *argc, char ***argv);

#define MAP_SIZE (1u << 16)
static uint8_t edges[MAP_SIZE];
static uint8_t virgin[MAP_SIZE];
static __thread uintptr_t prev_loc;
static bool tracing;

extern "C" __attribute__((no_sanitize_address, no_sanitize("undefined"))) void __sanitizer_cov_trace_pc() {
    if (!tracing) return;
    uintptr_t cur = (uintptr_t)__builtin_return_address(0);
    cur = (cur >> 4) ^ (cur << 8);
    edges[(cur ^ prev_loc) & (MAP_SIZE - 1)]++;
    prev_loc = cur >> 1;
}

static uint8_t bucket(uint8_t c) {
    if (c <= 3) return c;
    if (c <= 7) return 4;
    if (c <= 15) return 8;
    if (c <= 31) return 16;
    if (c <= 127) return 32;
    return 128;
}

// true if this run hit an edge or hit-count bucket never seen before
static bool new_coverage() {
    bool fresh = false;
    for (size_t i = 0; i < MAP_SIZE; i++) {
        if (!edges[i]) continue;
        uint8_t b = bucket(edges[i]);
        if ((virgin[i] | b) != virgin[i]) { virgin[i] |= b; fresh = true; }
    }
    return fresh;
}

static size_t count_edges() {
    size_t n = 0;
    for (size_t i = 0; i < MAP_SIZE; i++) n += virgin[i] != 0;
    return n;
}

static const uint8_t *cur_data;
static size_t cur_size;
static std::string out_dir = ".";

static uint64_t fnv(const uint8_t *d, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ d[i]) * 1099511628211ull;
    return h;
}

static void write_file(const std::string &path, const uint8_t *d, size_t n) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return;
    fwrite(d, 1, n, f);
    fclose(f);
}

static void save_crash() {
    if (!cur_data) return;
    char name[64];
    snprintf(name, sizeof(name), "/crash-%016llx", (unsigned long long)fnv(cur_data, cur_size));
    std::string p = out_dir + name;
    write_file(p, cur_data, cur_size);
    fprintf(stderr, "\n==fuzz== crashing input saved to %s (%zu bytes)\n", p.c_str(), cur_size);
    cur_data = nullptr;
}

static void on_signal(int sig) {
    if (sig == SIGALRM) fprintf(stderr, "\n==fuzz== timeout\n");
    save_crash();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void run_one(const uint8_t *d, size_t n) {
    memset(edges, 0, sizeof(edges));
    prev_loc = 0;
    // copy so ASan sees reads past the end of the real input
    uint8_t *copy = (uint8_t *)malloc(n ? n : 1);
    if (n) memcpy(copy, d, n);
    cur_data = copy; cur_size = n;
    alarm(10);
    tracing = true;
    LLVMFuzzerTestOneInput(copy, n);
    tracing = false;
    alarm(0);
    cur_data = nullptr;
    free(copy);
}

static std::vector<uint8_t> read_file(const char *path) {
    std::vector<uint8_t> v;
    FILE *f = fopen(path, "rb");
    if (!f) return v;
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) v.insert(v.end(), buf, buf + n);
    fclose(f);
    return v;
}

// ------------------------------------------------------------ mutation
static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint64_t rnd() {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
    return rng_state;
}
static size_t rn(size_t n) { return n ? size_t(rnd() % n) : 0; }

static const int64_t INTERESTING[] = {0, 1, -1, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff, 0x10000,
                                      0x7fffffff, (int64_t)0x80000000, (int64_t)0xffffffff, 2047, 2048,
                                      253, 254, 24, 25, 26, 27, 127, 128, 256, 8192};
static std::vector<std::vector<uint8_t>> corpus;

static void mutate(std::vector<uint8_t> &v, size_t max_len) {
    int rounds = 1 + int(rn(8));
    for (int r = 0; r < rounds; r++) {
        switch (rn(11)) {
            case 0: if (!v.empty()) v[rn(v.size())] ^= uint8_t(1u << rn(8)); break;
            case 1: if (!v.empty()) v[rn(v.size())] = uint8_t(rnd()); break;
            case 2: if (!v.empty()) v[rn(v.size())] += uint8_t(rn(35)) - 17; break;
            case 3: { // interesting value, 1/2/4/8 bytes, either endianness
                if (v.empty()) break;
                int w = 1 << rn(4);
                if (v.size() < size_t(w)) break;
                size_t at = rn(v.size() - w + 1);
                uint64_t x = uint64_t(INTERESTING[rn(sizeof(INTERESTING) / sizeof(INTERESTING[0]))]);
                bool be = rn(2);
                for (int k = 0; k < w; k++) v[at + k] = uint8_t(x >> (8 * (be ? w - 1 - k : k)));
                break;
            }
            case 4: if (v.size() > 1) { size_t a = rn(v.size()), l = 1 + rn(std::min<size_t>(16, v.size() - a)); v.erase(v.begin() + a, v.begin() + a + l); } break;
            case 5: if (v.size() < max_len) { size_t a = rn(v.size() + 1), l = 1 + rn(16); v.insert(v.begin() + a, l, uint8_t(rnd())); } break;
            case 6: if (v.size() < max_len && !v.empty()) { // duplicate a chunk
                size_t a = rn(v.size()), l = 1 + rn(std::min<size_t>(32, v.size() - a));
                std::vector<uint8_t> chunk(v.begin() + a, v.begin() + a + l);
                v.insert(v.begin() + rn(v.size() + 1), chunk.begin(), chunk.end());
            } break;
            case 7: if (v.size() > 2) { // copy within
                size_t a = rn(v.size()), b = rn(v.size()), l = 1 + rn(std::min(v.size() - a, v.size() - b));
                memmove(&v[b], &v[a], l);
            } break;
            case 8: if (!corpus.empty()) { // splice with another corpus entry
                const auto &o = corpus[rn(corpus.size())];
                if (o.empty()) break;
                size_t cut = rn(v.size() + 1), from = rn(o.size());
                v.resize(cut);
                v.insert(v.end(), o.begin() + from, o.end());
            } break;
            case 9: if (!corpus.empty()) { // overwrite a chunk with bytes from another entry
                const auto &o = corpus[rn(corpus.size())];
                if (o.empty() || v.empty()) break;
                size_t from = rn(o.size()), to = rn(v.size());
                size_t l = std::min(o.size() - from, v.size() - to);
                l = std::min<size_t>(l, 1 + rn(64));
                memcpy(&v[to], &o[from], l);
            } break;
            case 10: if (!v.empty()) v.resize(rn(v.size()) + 1); break;
        }
    }
    if (v.size() > max_len) v.resize(max_len);
}

int main(int argc, char **argv) {
    if (LLVMFuzzerInitialize) LLVMFuzzerInitialize(&argc, &argv);
    // ASan reports SEGV/BUS/FPE itself and then calls save_crash; we only
    // catch abort() (failed checks) and our own timeout.
    signal(SIGABRT, on_signal);
    signal(SIGALRM, on_signal);
    __sanitizer_set_death_callback(save_crash);

    std::string dir;
    long seconds = 60;
    size_t max_len = 8192;
    std::vector<std::string> files;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t") && i + 1 < argc) seconds = atol(argv[++i]);
        else if (!strcmp(argv[i], "-seed") && i + 1 < argc) rng_state ^= strtoull(argv[++i], 0, 0) * 0x9E3779B97F4A7C15ull;
        else if (!strcmp(argv[i], "-max_len") && i + 1 < argc) max_len = size_t(atol(argv[++i]));
        else {
            struct stat st;
            if (stat(argv[i], &st) == 0 && S_ISDIR(st.st_mode)) dir = argv[i];
            else files.push_back(argv[i]);
        }
    }

    if (!files.empty()) {   // reproduce mode
        for (auto &f : files) {
            auto v = read_file(f.c_str());
            fprintf(stderr, "running %s (%zu bytes)\n", f.c_str(), v.size());
            run_one(v.data(), v.size());
        }
        fprintf(stderr, "ok\n");
        return 0;
    }
    if (dir.empty()) { fprintf(stderr, "usage: %s corpus_dir [-t sec] [-seed N] | file...\n", argv[0]); return 2; }
    out_dir = dir;

    // load corpus
    if (DIR *d = opendir(dir.c_str())) {
        while (struct dirent *e = readdir(d)) {
            if (e->d_name[0] == '.' || !strncmp(e->d_name, "crash-", 6)) continue;
            auto v = read_file((dir + "/" + e->d_name).c_str());
            corpus.push_back(v);
        }
        closedir(d);
    }
    if (corpus.empty()) corpus.push_back({});
    for (auto &v : corpus) { run_one(v.data(), v.size()); new_coverage(); }
    fprintf(stderr, "loaded %zu inputs, %zu edges\n", corpus.size(), count_edges());

    time_t start = time(nullptr), last = start;
    uint64_t execs = 0;
    std::vector<uint8_t> v;
    while (time(nullptr) - start < seconds) {
        v = corpus[rn(corpus.size())];
        mutate(v, max_len);
        run_one(v.data(), v.size());
        execs++;
        if (new_coverage()) {
            corpus.push_back(v);
            char name[64];
            snprintf(name, sizeof(name), "/%016llx", (unsigned long long)fnv(v.data(), v.size()));
            write_file(dir + name, v.data(), v.size());
        }
        if (time(nullptr) - last >= 10) {
            last = time(nullptr);
            fprintf(stderr, "#%llu  edges %zu  corpus %zu  exec/s %llu\n", (unsigned long long)execs,
                    count_edges(), corpus.size(), (unsigned long long)(execs / std::max<time_t>(1, last - start)));
        }
    }
    fprintf(stderr, "done: %llu execs, %zu edges, corpus %zu\n", (unsigned long long)execs, count_edges(), corpus.size());
    return 0;
}
