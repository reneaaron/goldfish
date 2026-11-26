// "Generate on device": the pool's hashing, each source's credit and health
// checks, and that nothing comes out unless every source did its part. The
// samples come from a fixed xorshift32, so the seed is known: make_vector.py
// models the same hashing in Python, independent of this code.
// Run: pio test -e test -f test_device_entropy
#include <unity.h>
#include <string.h>
#include "../../src/app/device_entropy.h"

static uint32_t x;
static uint32_t next() {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

static uint8_t chunk[DE_CHIP_CHUNK];
static uint8_t lend[DE_CAMERA_LEND];
// A session with the camera's working memory lent, as the UI does.
static void start() {
    device_entropy_start();
    device_entropy_lend(lend);
}
static uint8_t frame[64 * 64];

static void fill_chunk() { for (auto &b : chunk) b = uint8_t(next()); }
static void fill_frame() { for (auto &p : frame) p = uint8_t(100 + next() % 16); }
static void fill_motion(int16_t s[6]) { for (int i = 0; i < 6; i++) s[i] = int16_t(next() & 0xFFFF); }

// Runs every source to its minimum, as the UI would; returns the counts per source.
static void run_all(int *frames, int *motions, int *touches) {
    start();
    while (device_entropy_bits(DS_CHIP) < device_entropy_needed(DS_CHIP)) { fill_chunk(); TEST_ASSERT_TRUE(device_entropy_chip(chunk, sizeof(chunk))); }
    device_entropy_next();
    *frames = 0;
    while (device_entropy_bits(DS_CAMERA) < device_entropy_needed(DS_CAMERA)) { fill_frame(); TEST_ASSERT_TRUE(device_entropy_camera(frame, 64, 64)); ++*frames; }
    device_entropy_next();
    *motions = 0;
    int16_t s[6];
    while (device_entropy_bits(DS_MOTION) < device_entropy_needed(DS_MOTION)) { fill_motion(s); TEST_ASSERT_TRUE(device_entropy_motion(s)); ++*motions; }
    device_entropy_next();
    *touches = 0;
    while (device_entropy_bits(DS_TOUCH) < device_entropy_needed(DS_TOUCH)) {
        int i = *touches;
        TEST_ASSERT_TRUE(device_entropy_touch(i, 2 * i, uint32_t(10 * i)));
        ++*touches;
    }
    TEST_ASSERT_TRUE(device_entropy_next());
    TEST_ASSERT_EQUAL(DS_COUNT, device_entropy_stage());
}

void test_vector() {
    x = 1;
    int frames, motions, touches;
    run_all(&frames, &motions, &touches);
    TEST_ASSERT_EQUAL(14, frames);
    TEST_ASSERT_EQUAL(386, motions);
    TEST_ASSERT_EQUAL(65, touches);
    TEST_ASSERT_NULL(device_entropy_error());
    SeedEntropy e;
    TEST_ASSERT_TRUE(device_entropy_finish(&e));
    TEST_ASSERT_EQUAL(16, e.len);
    const uint8_t want[16] = {0xcd, 0xe5, 0xd7, 0x10, 0xb0, 0xd5, 0xd6, 0xf2,
                              0x5e, 0xfb, 0x69, 0x74, 0x9c, 0x14, 0x6f, 0x5e};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, e.bytes, 16);
    // Finished: the session is gone.
    TEST_ASSERT_FALSE(device_entropy_finish(&e));
    TEST_ASSERT_EQUAL(0, e.len);
}

void test_one_touch_changes_the_seed() {
    SeedEntropy a, b;
    int f, m, t;
    x = 1;
    run_all(&f, &m, &t);
    TEST_ASSERT_TRUE(device_entropy_finish(&a));
    x = 1;
    start();
    while (device_entropy_bits(DS_CHIP) < device_entropy_needed(DS_CHIP)) { fill_chunk(); device_entropy_chip(chunk, sizeof(chunk)); }
    device_entropy_next();
    while (device_entropy_bits(DS_CAMERA) < device_entropy_needed(DS_CAMERA)) { fill_frame(); device_entropy_camera(frame, 64, 64); }
    device_entropy_next();
    int16_t s[6];
    while (device_entropy_bits(DS_MOTION) < device_entropy_needed(DS_MOTION)) { fill_motion(s); device_entropy_motion(s); }
    device_entropy_next();
    for (int i = 0; device_entropy_bits(DS_TOUCH) < device_entropy_needed(DS_TOUCH); i++)
        device_entropy_touch(i, 2 * i + (i == 30), uint32_t(10 * i));
    device_entropy_next();
    TEST_ASSERT_TRUE(device_entropy_finish(&b));
    TEST_ASSERT_FALSE(memcmp(a.bytes, b.bytes, 16) == 0);
}

void test_order_is_fixed() {
    x = 7;
    start();
    fill_frame();
    TEST_ASSERT_FALSE(device_entropy_camera(frame, 64, 64));   // camera before the chip
    TEST_ASSERT_NOT_NULL(device_entropy_error());
    fill_chunk();
    TEST_ASSERT_FALSE(device_entropy_chip(chunk, sizeof(chunk)));   // failed: nothing more is taken
    SeedEntropy e;
    TEST_ASSERT_FALSE(device_entropy_finish(&e));
}

void test_unfinished_gives_nothing() {
    x = 3;
    start();
    while (device_entropy_bits(DS_CHIP) < device_entropy_needed(DS_CHIP)) { fill_chunk(); device_entropy_chip(chunk, sizeof(chunk)); }
    device_entropy_next();
    SeedEntropy e;
    TEST_ASSERT_FALSE(device_entropy_finish(&e));
    TEST_ASSERT_EQUAL(0, e.len);
    TEST_ASSERT_FALSE(device_entropy_finish(&e));   // not started
}

static void expect_chip_fails(const uint8_t *c) {
    start();
    TEST_ASSERT_FALSE(device_entropy_chip(c, DE_CHIP_CHUNK));
    TEST_ASSERT_NOT_NULL(device_entropy_error());
    TEST_ASSERT_EQUAL(0, device_entropy_bits(DS_CHIP));
}

void test_chip_health() {
    uint8_t c[DE_CHIP_CHUNK];
    memset(c, 0, sizeof(c));
    expect_chip_fails(c);                       // dead: all zeros
    memset(c, 0xFF, sizeof(c));
    expect_chip_fails(c);
    x = 5;
    fill_chunk();
    memcpy(c, chunk, sizeof(c));
    memcpy(c + 100, c + 96, 4);
    expect_chip_fails(c);                       // a word repeated
    for (size_t i = 0; i < sizeof(c); i++) c[i] = uint8_t(next() | 0x81);
    expect_chip_fails(c);                       // biased bits
    // A counter (0..255) passes: health checks catch a dead or stuck source,
    // not one that only looks random. That is what the warning screen is for.
    for (size_t i = 0; i < sizeof(c); i++) c[i] = uint8_t(i);
    start();
    TEST_ASSERT_TRUE(device_entropy_chip(c, sizeof(c)));
    // The same good chunk twice is a stuck generator.
    start();
    TEST_ASSERT_TRUE(device_entropy_chip(chunk, sizeof(chunk)));
    TEST_ASSERT_FALSE(device_entropy_chip(chunk, sizeof(chunk)));
    TEST_ASSERT_EQUAL(64, device_entropy_bits(DS_CHIP));
    // Wrong size.
    start();
    TEST_ASSERT_FALSE(device_entropy_chip(chunk, 128));
}

static void chip_done() {
    start();
    while (device_entropy_bits(DS_CHIP) < device_entropy_needed(DS_CHIP)) { fill_chunk(); TEST_ASSERT_TRUE(device_entropy_chip(chunk, sizeof(chunk))); }
    device_entropy_next();
}

void test_camera_needs_noise() {
    x = 9;
    chip_done();
    fill_frame();
    // The same frame over and over: hashed, never credited.
    for (int i = 0; i < 20; i++) TEST_ASSERT_TRUE(device_entropy_camera(frame, 64, 64));
    TEST_ASSERT_EQUAL(0, device_entropy_bits(DS_CAMERA));
    // Black frames that change only below the clip level: no credit either.
    for (int i = 0; i < 20; i++) {
        for (auto &p : frame) p = uint8_t(next() % 8);
        TEST_ASSERT_TRUE(device_entropy_camera(frame, 64, 64));
    }
    TEST_ASSERT_EQUAL(0, device_entropy_bits(DS_CAMERA));
    // Noisy frames, after the dark ones: the first only sets what the next
    // is compared with; then at most 24 bits each.
    fill_frame();
    device_entropy_camera(frame, 64, 64);
    TEST_ASSERT_EQUAL(0, device_entropy_bits(DS_CAMERA));
    fill_frame();
    device_entropy_camera(frame, 64, 64);
    TEST_ASSERT_EQUAL(24, device_entropy_bits(DS_CAMERA));
    fill_frame();
    device_entropy_camera(frame, 64, 64);
    TEST_ASSERT_EQUAL(48, device_entropy_bits(DS_CAMERA));
    TEST_ASSERT_FALSE(device_entropy_camera(nullptr, 64, 64));   // no picture fails
}

// Change that isn't noise earns nothing: the whole picture brightening a
// step a frame (auto-exposure), a scene moving past, and changes that only go
// one way.
void test_camera_counts_noise_not_change() {
    x = 19;
    chip_done();
    fill_frame();
    for (int i = 0; i < 10; i++) {   // exposure: every pixel one brighter than before
        for (auto &p : frame) p++;
        TEST_ASSERT_TRUE(device_entropy_camera(frame, 64, 64));
    }
    TEST_ASSERT_EQUAL(0, device_entropy_bits(DS_CAMERA));
    // A scene panning past, 5 pixels a frame (its first frame is a jump from
    // the last scene, compared with which anything goes; after that, nothing).
    int before = 0;
    for (int i = 0; i < 10; i++) {
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++) frame[y * 64 + x] = uint8_t(20 + ((x + 5 * i) % 64) * 3 + y);
        TEST_ASSERT_TRUE(device_entropy_camera(frame, 64, 64));
        if (i == 0) before = device_entropy_bits(DS_CAMERA);
    }
    TEST_ASSERT_EQUAL(before, device_entropy_bits(DS_CAMERA));
    for (int i = 0; i < 10; i++) {   // half the pixels creep up, the rest stay: lopsided
        for (size_t k = 0; k < sizeof(frame); k += 2) frame[k] = uint8_t(frame[k] < 200 ? frame[k] + 1 : 100);
        TEST_ASSERT_TRUE(device_entropy_camera(frame, 64, 64));
    }
    TEST_ASSERT_EQUAL(before, device_entropy_bits(DS_CAMERA));
}

void test_motion_needs_movement() {
    x = 11;
    chip_done();
    while (device_entropy_bits(DS_CAMERA) < device_entropy_needed(DS_CAMERA)) { fill_frame(); device_entropy_camera(frame, 64, 64); }
    device_entropy_next();
    // Smooth movement with jitter on only two axes: no credit.
    int16_t s[6];
    for (int i = 0; i < 50; i++) {
        s[0] = int16_t(next()); s[1] = int16_t(next());
        for (int k = 2; k < 6; k++) s[k] = int16_t(i * 10 * k);   // on a straight line
        TEST_ASSERT_TRUE(device_entropy_motion(s));
    }
    TEST_ASSERT_EQUAL(0, device_entropy_bits(DS_MOTION));
    // Jitter on every axis: half a bit a sample.
    for (int i = 0; i < 20; i++) { fill_motion(s); TEST_ASSERT_TRUE(device_entropy_motion(s)); }
    TEST_ASSERT_EQUAL(10, device_entropy_bits(DS_MOTION));
    // One axis stuck while the others move: the session fails on its 32nd repeat.
    int i = 0;
    for (;; i++) {
        fill_motion(s);
        s[2] = 777;
        if (!device_entropy_motion(s)) break;
    }
    TEST_ASSERT_EQUAL(32, i);
    TEST_ASSERT_NOT_NULL(device_entropy_error());
}

void test_touch_needs_movement() {
    x = 13;
    chip_done();
    while (device_entropy_bits(DS_CAMERA) < device_entropy_needed(DS_CAMERA)) { fill_frame(); device_entropy_camera(frame, 64, 64); }
    device_entropy_next();
    int16_t s[6];
    while (device_entropy_bits(DS_MOTION) < device_entropy_needed(DS_MOTION)) { fill_motion(s); device_entropy_motion(s); }
    device_entropy_next();
    for (int i = 0; i < 100; i++) TEST_ASSERT_TRUE(device_entropy_touch(50, 60, uint32_t(i * 30)));
    TEST_ASSERT_EQUAL(0, device_entropy_bits(DS_TOUCH));
    TEST_ASSERT_FALSE(device_entropy_next());   // not before its minimum
    TEST_ASSERT_EQUAL(DS_TOUCH, device_entropy_stage());
}

// A step may take more than its minimum (the UI keeps each up a few
// seconds); only next() moves on, and only once the minimum is there.
void test_next_waits_for_the_minimum() {
    x = 17;
    start();
    TEST_ASSERT_FALSE(device_entropy_next());
    for (int i = 0; i < 8; i++) { fill_chunk(); TEST_ASSERT_TRUE(device_entropy_chip(chunk, sizeof(chunk))); }
    TEST_ASSERT_EQUAL(512, device_entropy_bits(DS_CHIP));   // twice the minimum, still the chip
    TEST_ASSERT_EQUAL(DS_CHIP, device_entropy_stage());
    TEST_ASSERT_TRUE(device_entropy_next());
    TEST_ASSERT_EQUAL(DS_CAMERA, device_entropy_stage());
    fill_chunk();
    TEST_ASSERT_FALSE(device_entropy_chip(chunk, sizeof(chunk)));   // the chip's turn is over
}

void test_camera_needs_its_loan() {
    x = 23;
    device_entropy_start();   // nothing lent
    while (device_entropy_bits(DS_CHIP) < device_entropy_needed(DS_CHIP)) { fill_chunk(); device_entropy_chip(chunk, sizeof(chunk)); }
    device_entropy_next();
    fill_frame();
    TEST_ASSERT_FALSE(device_entropy_camera(frame, 64, 64));
    TEST_ASSERT_NOT_NULL(device_entropy_error());
    // A finished session wipes what it borrowed.
    start();
    memset(lend, 0xAA, sizeof(lend));
    device_entropy_wipe();
    for (uint8_t b : lend) TEST_ASSERT_EQUAL(0, b);
}

void test_fail_wipes() {
    x = 1;
    int f, m, t;
    run_all(&f, &m, &t);
    device_entropy_fail("Camera stopped");
    TEST_ASSERT_EQUAL_STRING("Camera stopped", device_entropy_error());
    SeedEntropy e;
    TEST_ASSERT_FALSE(device_entropy_finish(&e));
    TEST_ASSERT_EQUAL(0, e.len);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_vector);
    RUN_TEST(test_one_touch_changes_the_seed);
    RUN_TEST(test_order_is_fixed);
    RUN_TEST(test_unfinished_gives_nothing);
    RUN_TEST(test_chip_health);
    RUN_TEST(test_camera_needs_noise);
    RUN_TEST(test_camera_counts_noise_not_change);
    RUN_TEST(test_motion_needs_movement);
    RUN_TEST(test_touch_needs_movement);
    RUN_TEST(test_next_waits_for_the_minimum);
    RUN_TEST(test_camera_needs_its_loan);
    RUN_TEST(test_fail_wipes);
    return UNITY_END();
}
