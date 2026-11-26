// "Generate on device" sources (app/device_entropy.h): the RP2350's TRNG and
// the QMI8658 motion sensor. Used by nothing else; dice, coins and words never
// call these.
#include <Arduino.h>
#include <Wire.h>
#include "hardware/resets.h"
#include "hardware/structs/trng.h"
#include "hardware/regs/trng.h"
#include "board_pins.h"
#include "../app/device_entropy.h"

// ------------------------------------------------------------ TRNG
// The TRNG in its normal mode: von Neumann corrector, repetition (CRNGT) and
// autocorrelation checks all on. Any of their error flags fails the read.
// (The SDK's pico_rand bypasses all three to stream raw bits into its PRNG;
// this reads conditioned bits, with the hardware's own checks.)
#define TRNG_SPACING 2000       // ROSC samples are this many clock cycles apart
#define TRNG_TIMEOUT_MS 500     // per 192 bits

static bool trng_ready;

bool platform_trng(uint8_t *out, size_t n) {
    if (!trng_ready) {
        reset_unreset_block_num_wait_blocking(RESET_TRNG);
        trng_ready = true;
    }
    trng_hw->rnd_source_enable = 0;
    trng_hw->trng_sw_reset = TRNG_TRNG_SW_RESET_BITS;
    while (trng_hw->trng_busy) {}
    trng_hw->rng_imr = TRNG_RNG_IMR_BITS;        // no interrupts: polled
    trng_hw->trng_config = 0;                    // the shortest ROSC chain
    trng_hw->sample_cnt1 = TRNG_SPACING;
    trng_hw->trng_debug_control = 0;             // nothing bypassed
    trng_hw->rng_icr = -1u;
    trng_hw->rnd_source_enable = TRNG_RND_SOURCE_ENABLE_BITS;
    bool ok = true;
    size_t got = 0;
    while (ok && got < n) {
        uint32_t t0 = millis();
        uint32_t isr;
        while (!((isr = trng_hw->rng_isr) & TRNG_RNG_ISR_BITS))
            if (millis() - t0 > TRNG_TIMEOUT_MS) { ok = false; break; }
        if (!ok) break;
        if (isr & (TRNG_RNG_ISR_VN_ERR_BITS | TRNG_RNG_ISR_CRNGT_ERR_BITS | TRNG_RNG_ISR_AUTOCORR_ERR_BITS)) {
            ok = false;
            break;
        }
        for (int i = 0; i < 6 && got < n; i++) {   // 192 bits; reading the last word starts the next round
            uint32_t w = trng_hw->ehr_data[i];
            for (int k = 0; k < 4 && got < n; k++) out[got++] = uint8_t(w >> (8 * k));
        }
        trng_hw->rng_icr = TRNG_RNG_ISR_EHR_VALID_BITS;
    }
    trng_hw->rnd_source_enable = 0;
    trng_hw->rng_icr = -1u;
    if (!ok) memset(out, 0, n);
    return ok;
}

// ------------------------------------------------------------ QMI8658
// On I2C0 with the touch controller. Accelerometer ±8 g and gyro ±512 dps at
// about 1 kHz, read raw; started on first use, left running.
enum : uint8_t {
    QMI_WHO_AM_I = 0x00,   // 0x05
    QMI_CTRL1 = 0x02,      // 0x40: register address auto-increment
    QMI_CTRL2 = 0x03,      // accelerometer range and rate
    QMI_CTRL3 = 0x04,      // gyro range and rate
    QMI_CTRL7 = 0x08,      // 0x03: accelerometer and gyro on
    QMI_AX_L = 0x35,       // ax, ay, az, gx, gy, gz: 12 bytes, little-endian
    QMI_RESET = 0x60,      // 0xB0: soft reset
};

static uint8_t qmi_addr;   // 0: not found yet

static void qmi_wr(uint8_t reg, uint8_t v) {
    Wire.beginTransmission(qmi_addr);
    Wire.write(reg);
    Wire.write(v);
    Wire.endTransmission();
}

static bool qmi_rd(uint8_t addr, uint8_t reg, uint8_t *out, size_t n) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)addr, (int)n) != (int)n) return false;
    for (size_t i = 0; i < n; i++) out[i] = Wire.read();
    return true;
}

static bool qmi_start() {
    static const uint8_t addrs[] = {0x6B, 0x6A};
    for (uint8_t a : addrs) {
        uint8_t id;
        if (qmi_rd(a, QMI_WHO_AM_I, &id, 1) && id == 0x05) { qmi_addr = a; break; }
    }
    if (!qmi_addr) return false;
    qmi_wr(QMI_RESET, 0xB0);
    delay(20);
    qmi_wr(QMI_CTRL1, 0x40);
    qmi_wr(QMI_CTRL2, 0x23);   // ±8 g, 1 kHz
    qmi_wr(QMI_CTRL3, 0x53);   // ±512 dps, 1 kHz
    qmi_wr(QMI_CTRL7, 0x03);
    delay(30);
    return true;
}

bool platform_motion(int16_t out[6]) {
    if (!qmi_addr && !qmi_start()) return false;
    uint8_t r[12];
    if (!qmi_rd(qmi_addr, QMI_AX_L, r, sizeof(r))) return false;
    for (int i = 0; i < 6; i++) out[i] = int16_t(r[2 * i] | (r[2 * i + 1] << 8));
    return true;
}
