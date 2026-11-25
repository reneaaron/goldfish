#pragma once
#include <stddef.h>
#include <stdint.h>
#include "seedqr.h"

// "Generate on device": a 12-word seed from the device's own sources, a
// separate method next to dice, coins and drawn words. It never touches those:
// seed_create.h's functions take only what the user typed, and nothing here is
// mixed into them.
//
// Unlike dice, the user can't re-check the result: they trust this firmware
// and the chip. The UI says so before it starts. What this module can do is
// not rely on any one source:
//
//   1. chip    the RP2350's TRNG, read first, before the other sources exist
//   2. camera  sensor noise: how sampled pixels jitter from frame to frame
//   3. motion  the QMI8658's jitter, while the user shakes the device
//   4. touch   the user's scribble: points and their times (hashed in, not
//              counted on for strength)
//
// Every source must reach its own minimum before the next one starts (chip,
// camera and motion: 192 bits or more each, enough for a 128-bit seed on its
// own with NIST SP 800-90C's 64-bit margin) and pass its health checks; any
// failure ends the session and no seed comes out. Sources are taken in that
// order only.
//
// Each sample is hashed into a 32-byte pool as it arrives (nothing raw is
// kept): pool = SHA256(pool || source || length || SHA256(sample)), starting
// from SHA256("goldfish/device-seed/v1"). The seed's entropy is the first 16
// bytes of SHA256(pool || "seed").
//
// Credit is counted conservatively, far below what the samples carry, and only
// for noise: change that could be predicted (a scene moving, auto-exposure, a
// hand's smooth swing) is hashed in but earns nothing. The health checks catch
// a source that is dead, stuck or constant. They can't
// catch one built to look random; that is what the warning is for.

enum DeviceSource { DS_CHIP, DS_CAMERA, DS_MOTION, DS_TOUCH, DS_COUNT };

// Platform hooks, one per target (board: RP2350 TRNG and QMI8658; sim and
// web: the host's random source, which is not this device's hardware).
// false: the source can't be read (no sensor, a hardware error flag).
bool platform_trng(uint8_t *out, size_t n);
bool platform_motion(int16_t out[6]);    // accel x, y, z, then gyro x, y, z

void device_entropy_start();             // wipes and starts a new session
void device_entropy_wipe();

// Chip: exactly DE_CHIP_CHUNK bytes per call.
#define DE_CHIP_CHUNK 256
bool device_entropy_chip(const uint8_t *b, size_t n);
// The camera step's working memory, DE_CAMERA_LEND bytes of the caller's idle
// RAM (the board has no static room for it), lent after device_entropy_start
// and wiped and given back with the session.
#define DE_CAMERA_LEND 1024
void device_entropy_lend(uint8_t *buf);
// Camera: one grayscale frame. Credit only for noise: each sampled pixel's
// change less the frame's mean change, small, about as often up as down; none
// for the first frames while exposure settles.
bool device_entropy_camera(const uint8_t *frame, int w, int h);
// Motion: one sample; half a bit when it's off the line through the last two
// on at least three axes. One axis stuck for 32 samples fails the session.
bool device_entropy_motion(const int16_t s[6]);
// Touch: one point of the scribble, with the time in ms; counts points that moved.
bool device_entropy_touch(int x, int y, uint32_t ms);

// Moves on to the next source once this one has its minimum (it may take more
// first: the UI keeps each step up for a few seconds). false if it hasn't.
bool device_entropy_next();
// The source being collected now (DS_COUNT: all done), its credited bits and the minimum.
DeviceSource device_entropy_stage();
int device_entropy_bits(DeviceSource s);
int device_entropy_needed(DeviceSource s);
// Why the session failed, nullptr if it hasn't. Every add returns false from then on.
const char *device_entropy_error();
// Marks the session failed (e.g. a source the platform can't read, or one that never gives credit).
void device_entropy_fail(const char *why);

// 12 words' entropy once every source is done and none failed; wipes the session either way.
bool device_entropy_finish(SeedEntropy *out);
