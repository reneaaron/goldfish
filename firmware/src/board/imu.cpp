#include "imu.h"
#include <Arduino.h>
#include <Wire.h>
#include "display.h"

// Registers and the address probe from Waveshare's QMI8658 demo driver.
enum : uint8_t {
    REG_WHO_AM_I = 0x00,   // reads 0x05
    REG_CTRL1 = 0x02,
    REG_CTRL2 = 0x03,
    REG_CTRL7 = 0x08,
    REG_AX_L = 0x35,
};

constexpr int32_t ONE_G = 16384;          // LSB per g at ±2 g
constexpr int32_t TILT = ONE_G / 2;       // ~30° past horizontal before flipping
constexpr uint32_t POLL_MS = 100;
constexpr int STEADY_POLLS = 3;           // same answer this many polls in a row

// The accelerometer axis along the screen's long side, signed so that it reads
// +1 g with the board upright. TODO: confirm on hardware.
constexpr int UP_AXIS = 1;                // 0 = x, 1 = y, 2 = z
constexpr int UP_SIGN = 1;

static uint8_t addr;                      // 0 = no IMU found

static void wr(uint8_t reg, uint8_t v) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.write(v);
    Wire.endTransmission();
}

static bool rd(uint8_t reg, uint8_t *out, size_t n) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)addr, (int)n) != (int)n) return false;
    for (size_t i = 0; i < n; i++) out[i] = Wire.read();
    return true;
}

void imu_init() {
    static const uint8_t ADDRS[] = {0x6A, 0x6B};
    for (uint8_t a : ADDRS) {
        addr = a;
        uint8_t id;
        if (rd(REG_WHO_AM_I, &id, 1) && id == 0x05) break;
        addr = 0;
    }
    if (!addr) return;
    wr(REG_CTRL1, 0x40);   // auto-increment, little-endian
    wr(REG_CTRL2, 0x07);   // accelerometer ±2 g, 62.5 Hz
    wr(REG_CTRL7, 0x01);   // accelerometer on, gyro off
}

void imu_poll() {
    static uint32_t last;
    static int steady;
    if (!addr || millis() - last < POLL_MS) return;
    last = millis();

    uint8_t r[6];
    if (!rd(REG_AX_L, r, sizeof(r))) return;
    int32_t up = UP_SIGN * int16_t(r[2 * UP_AXIS] | (r[2 * UP_AXIS + 1] << 8));
    bool flip = display_flipped();
    bool want = up < -TILT ? true : up > TILT ? false : flip;
    steady = want != flip ? steady + 1 : 0;
    if (steady >= STEADY_POLLS) {
        display_set_flipped(want);
        steady = 0;
    }
}
