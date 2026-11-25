#include "device_entropy.h"
#include <stdlib.h>
#include <string.h>
#include <wally_crypto.h>

// What each source must reach. Chip, camera and motion each count bits, and
// each must cover a 128-bit seed on its own with NIST's margin (SP 800-90C:
// 64 bits more than the output, so 192 or more). Touch counts points that
// moved: it is the user's part, hashed in but not counted on for strength.
static const int NEEDED[DS_COUNT] = {256, 256, 192, 64};

// Credit per sample, well below what it carries. Only noise counts: what a
// source does that could be predicted (a scene moving, auto-exposure, a hand's
// swing) is hashed in but earns nothing.
#define CHIP_CREDIT 64          // per 256-byte chunk: 1 bit per 4 bytes
#define CAM_GRID 32             // 32x32 sampled pixels per frame
#define CAM_SETTLE 3            // the first frames get nothing: exposure is still settling
#define CAM_NOISE_MAX 12        // a sample's change, less the frame's mean change, this small is noise
#define CAM_PER_BIT 16          // noisy samples per bit
#define CAM_CREDIT_MAX 24       // per frame
#define MOTION_STUCK 32         // one axis the same this many samples in a row: stuck
#define MOTION_JITTER_AXES 3    // axes off the line through the last two samples, for a sample to count

static uint8_t pool[32];
static bool started;
static DeviceSource stage;
static int bits[DS_COUNT];
static const char *err;
static uint8_t chip_last[32];           // digest of the last chip chunk
static uint8_t cam_last[CAM_GRID * CAM_GRID];
static int cam_frames;
static int16_t motion_last[2][6];        // the last sample, and the one before it
static int motion_n, motion_jitters;
static uint8_t motion_same[6];
static int touch_x = -1, touch_y = -1;

static void absorb(DeviceSource s, const uint8_t *data, size_t n) {
    uint8_t buf[32 + 1 + 4 + 32];
    memcpy(buf, pool, 32);
    buf[32] = uint8_t(s);
    for (int i = 0; i < 4; i++) buf[33 + i] = uint8_t(uint32_t(n) >> (24 - 8 * i));
    wally_sha256(data, n, buf + 37, 32);
    wally_sha256(buf, sizeof(buf), pool, 32);
    secure_zero(buf, sizeof(buf));
}

void device_entropy_wipe() {
    secure_zero(pool, sizeof(pool));
    secure_zero(chip_last, sizeof(chip_last));
    secure_zero(cam_last, sizeof(cam_last));
    secure_zero(motion_last, sizeof(motion_last));
    memset(motion_same, 0, sizeof(motion_same));
    memset(bits, 0, sizeof(bits));
    started = false;
    stage = DS_CHIP;
    err = nullptr;
    cam_frames = motion_n = motion_jitters = 0;
    touch_x = touch_y = -1;
}

void device_entropy_start() {
    device_entropy_wipe();
    static const char tag[] = "goldfish/device-seed/v1";
    wally_sha256((const uint8_t *)tag, sizeof(tag) - 1, pool, 32);
    started = true;
}

void device_entropy_fail(const char *why) {
    if (!err) err = why;
    secure_zero(pool, sizeof(pool));
}

const char *device_entropy_error() { return started ? err : "Not started"; }
DeviceSource device_entropy_stage() { return stage; }
int device_entropy_bits(DeviceSource s) { return s < DS_COUNT ? bits[s] : 0; }
int device_entropy_needed(DeviceSource s) { return s < DS_COUNT ? NEEDED[s] : 0; }

// Whether a sample for `s` may be taken now; a sample out of order fails the session.
static bool accept(DeviceSource s) {
    if (!started || err) return false;
    if (stage != s) { device_entropy_fail("Sources out of order"); return false; }
    return true;
}

static void credit(DeviceSource s, int b) { bits[s] += b; }

bool device_entropy_next() {
    if (!started || err || stage >= DS_COUNT || bits[stage] < NEEDED[stage]) return false;
    stage = DeviceSource(stage + 1);
    return true;
}

bool device_entropy_chip(const uint8_t *b, size_t n) {
    if (!accept(DS_CHIP)) return false;
    if (n != DE_CHIP_CHUNK) { device_entropy_fail("Chip: wrong sample size"); return false; }
    // Repetition: no 32-bit word may equal the one before it (2^-32 by chance).
    for (size_t i = 4; i < n; i += 4)
        if (!memcmp(b + i, b + i - 4, 4)) { device_entropy_fail("Chip: repeating output"); return false; }
    // Proportion: no byte value more than 16 times in 256 (about 1 expected),
    // and about half the bits set (2048 bits: 1024 +- 160, 7 sigma).
    uint16_t count[256] = {0};
    int ones = 0;
    for (size_t i = 0; i < n; i++) {
        if (++count[b[i]] > 16) { device_entropy_fail("Chip: biased output"); return false; }
        for (uint8_t v = b[i]; v; v &= v - 1) ones++;
    }
    if (ones < 1024 - 160 || ones > 1024 + 160) { device_entropy_fail("Chip: biased output"); return false; }
    uint8_t d[32];
    wally_sha256(b, n, d, 32);
    bool same = bits[DS_CHIP] && !memcmp(d, chip_last, 32);
    memcpy(chip_last, d, 32);
    secure_zero(d, sizeof(d));
    if (same) { device_entropy_fail("Chip: repeating output"); return false; }
    absorb(DS_CHIP, b, n);
    credit(DS_CHIP, CHIP_CREDIT);
    return true;
}

bool device_entropy_camera(const uint8_t *f, int w, int h) {
    if (!accept(DS_CAMERA)) return false;
    if (!f || w < CAM_GRID * 2 || h < CAM_GRID * 2) { device_entropy_fail("Camera: no picture"); return false; }
    absorb(DS_CAMERA, f, size_t(w) * h);
    // Noise moves each pixel a little, up or down at random, from one frame to
    // the next. Change that isn't noise is taken out first: the whole frame
    // shifting (auto-exposure, flicker) as the mean change, and big jumps
    // (something moving) by the size limit. What's left must go up about as
    // often as down, or it isn't noise either.
    // Two passes over the sampled pixels (no room for a copy on the board's
    // stack): the mean change first, then each change less the mean.
    auto at = [&](int i) { int gx = i % CAM_GRID, gy = i / CAM_GRID;
                           return f[(gy * h / CAM_GRID + h / (2 * CAM_GRID)) * w + gx * w / CAM_GRID + w / (2 * CAM_GRID)]; };
    auto unclipped = [](int v) { return v >= 8 && v <= 247; };   // clipped pixels show no noise
    int lit = 0, sum = 0;
    for (int i = 0; i < CAM_GRID * CAM_GRID && cam_frames; i++) {
        int v = at(i);
        if (unclipped(v) && unclipped(cam_last[i])) { lit++; sum += v - cam_last[i]; }
    }
    int mean = lit ? sum / lit : 0;
    int noisy = 0, up = 0, down = 0;
    for (int i = 0; i < CAM_GRID * CAM_GRID; i++) {
        int v = at(i);
        int r = v - cam_last[i] - mean;
        bool use = cam_frames && unclipped(v) && unclipped(cam_last[i]) && r != 0 && abs(r) <= CAM_NOISE_MAX;
        cam_last[i] = uint8_t(v);
        if (!use) continue;
        noisy++;
        if (r > 0) up++; else down++;
    }
    int c = noisy / CAM_PER_BIT;
    if (++cam_frames <= CAM_SETTLE) c = 0;
    if (lit < CAM_GRID * CAM_GRID / 4) c = 0;    // too dark or too bright to trust
    if (abs(up - down) * 4 > noisy) c = 0;       // lopsided: not noise
    if (c > CAM_CREDIT_MAX) c = CAM_CREDIT_MAX;
    credit(DS_CAMERA, c);
    return true;
}

bool device_entropy_motion(const int16_t s[6]) {
    if (!accept(DS_MOTION)) return false;
    absorb(DS_MOTION, (const uint8_t *)s, 12);
    // Stuck: any one axis the same, sample after sample.
    for (int i = 0; i < 6; i++) {
        bool same = motion_n && s[i] == motion_last[0][i];
        motion_same[i] = same ? uint8_t(motion_same[i] + 1) : 0;
        if (motion_same[i] >= MOTION_STUCK) { device_entropy_fail("Motion sensor is stuck"); return false; }
    }
    // A hand's swing is smooth: each sample is nearly where the last two point.
    // What's off that line is the sensor's jitter; a sample with jitter on
    // enough axes counts, half a bit each.
    int off = 0;
    for (int i = 0; i < 6 && motion_n >= 2; i++)
        off += int(s[i]) - 2 * motion_last[0][i] + motion_last[1][i] != 0;
    memcpy(motion_last[1], motion_last[0], 12);
    memcpy(motion_last[0], s, 12);
    motion_n++;
    bool counts = off >= MOTION_JITTER_AXES && ++motion_jitters % 2 == 0;
    credit(DS_MOTION, counts ? 1 : 0);
    return true;
}

bool device_entropy_touch(int x, int y, uint32_t ms) {
    if (!accept(DS_TOUCH)) return false;
    uint8_t b[12];
    uint32_t v[3] = {uint32_t(x), uint32_t(y), ms};
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 4; k++) b[i * 4 + k] = uint8_t(v[i] >> (24 - 8 * k));
    absorb(DS_TOUCH, b, sizeof(b));
    bool moved = touch_x >= 0 && (x != touch_x || y != touch_y);
    touch_x = x;
    touch_y = y;
    credit(DS_TOUCH, moved ? 1 : 0);
    return true;
}

bool device_entropy_finish(SeedEntropy *out) {
    memset(out, 0, sizeof(*out));
    bool ok = started && !err && stage == DS_COUNT;
    for (int s = 0; s < DS_COUNT; s++) ok = ok && bits[s] >= NEEDED[s];
    if (ok) {
        uint8_t buf[32 + 4], h[32];
        memcpy(buf, pool, 32);
        memcpy(buf + 32, "seed", 4);
        wally_sha256(buf, sizeof(buf), h, 32);
        memcpy(out->bytes, h, 16);
        out->len = 16;
        secure_zero(buf, sizeof(buf));
        secure_zero(h, sizeof(h));
    }
    device_entropy_wipe();
    return ok;
}
