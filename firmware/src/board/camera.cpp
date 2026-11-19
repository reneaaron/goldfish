// OV5640 camera: SCCB setup on I2C1, 8-bit DVP capture with PIO + DMA.
// Ported from Waveshare's C/02-CAM demo (cam.c, ov5640.c, picampinos.pio): same
// clock, PLL and window, but the sensor outputs Y8 instead of RGB565, so a
// frame is already the grayscale image quirc wants.
#include <Arduino.h>
#include <Wire.h>
#include <string.h>
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/pio_instructions.h"
#include "hardware/pwm.h"
#include "board_pins.h"
#include "../app/qrscan.h"

#define CAM_ADDR 0x3C
#define CAM_W 240
#define CAM_H 320
// The app gets the middle rows: 240x180, the size of the scan preview.
#define OUT_H 180
#define OUT_Y ((CAM_H - OUT_H) / 2)

// Lens position for autofocus modules, as a 10-bit VCM DAC code (0 = resting,
// focused far away). No autofocus firmware is loaded; the lens is parked at
// one distance. TODO: tune on hardware until a QR at ~12cm is sharp.
// Fixed-focus modules have no VCM and ignore this.
#define CAM_FOCUS 400

#define DELAY 0xFFFF
static const uint16_t init_regs[][2] = {
    // Waveshare's sensor_default_regs
    {0x3008, 0x82}, {DELAY, 10}, {0x3008, 0x42}, {0x3103, 0x13}, {0x3017, 0xff}, {0x3018, 0xff},
    {0x302c, 0xc3}, {0x4740, 0x21}, {0x4713, 0x02}, {0x5001, 0x83}, {0x3000, 0x20}, {DELAY, 10},
    {0x3002, 0x1c}, {0x3004, 0xff}, {0x3006, 0xc3}, {0x5000, 0xa7}, {0x5001, 0xa3}, {0x5003, 0x08},
    {0x370c, 0x02}, {0x3634, 0x40}, {0x3a02, 0x03}, {0x3a03, 0xd8}, {0x3a08, 0x01}, {0x3a09, 0x27},
    {0x3a0a, 0x00}, {0x3a0b, 0xf6}, {0x3a0d, 0x04}, {0x3a0e, 0x03}, {0x3a0f, 0x30}, {0x3a10, 0x28},
    {0x3a11, 0x60}, {0x3a13, 0x43}, {0x3a14, 0x03}, {0x3a15, 0xd8}, {0x3a18, 0x00}, {0x3a19, 0xf8},
    {0x3a1b, 0x30}, {0x3a1e, 0x26}, {0x3a1f, 0x14}, {0x3600, 0x08}, {0x3601, 0x33}, {0x3c01, 0xa4},
    {0x3c04, 0x28}, {0x3c05, 0x98}, {0x3c06, 0x00}, {0x3c07, 0x08}, {0x3c08, 0x00}, {0x3c09, 0x1c},
    {0x3c0a, 0x9c}, {0x3c0b, 0x40}, {0x460c, 0x22}, {0x4001, 0x02}, {0x4004, 0x02}, {0x5180, 0xff},
    {0x5181, 0xf2}, {0x5182, 0x00}, {0x5183, 0x14}, {0x5184, 0x25}, {0x5185, 0x24}, {0x5186, 0x09},
    {0x5187, 0x09}, {0x5188, 0x09}, {0x5189, 0x75}, {0x518a, 0x54}, {0x518b, 0xe0}, {0x518c, 0xb2},
    {0x518d, 0x42}, {0x518e, 0x3d}, {0x518f, 0x56}, {0x5190, 0x46}, {0x5191, 0xf8}, {0x5192, 0x04},
    {0x5193, 0x70}, {0x5194, 0xf0}, {0x5195, 0xf0}, {0x5196, 0x03}, {0x5197, 0x01}, {0x5198, 0x04},
    {0x5199, 0x12}, {0x519a, 0x04}, {0x519b, 0x00}, {0x519c, 0x06}, {0x519d, 0x82}, {0x519e, 0x38},
    {0x5381, 0x1e}, {0x5382, 0x5b}, {0x5383, 0x08}, {0x5384, 0x0a}, {0x5385, 0x7e}, {0x5386, 0x88},
    {0x5387, 0x7c}, {0x5388, 0x6c}, {0x5389, 0x10}, {0x538a, 0x01}, {0x538b, 0x98}, {0x5300, 0x10},
    {0x5301, 0x10}, {0x5302, 0x18}, {0x5303, 0x19}, {0x5304, 0x10}, {0x5305, 0x10}, {0x5306, 0x08},
    {0x5307, 0x16}, {0x5308, 0x40}, {0x5309, 0x10}, {0x530a, 0x10}, {0x530b, 0x04}, {0x530c, 0x06},
    {0x5480, 0x01}, {0x5481, 0x00}, {0x5482, 0x1e}, {0x5483, 0x3b}, {0x5484, 0x58}, {0x5485, 0x66},
    {0x5486, 0x71}, {0x5487, 0x7d}, {0x5488, 0x83}, {0x5489, 0x8f}, {0x548a, 0x98}, {0x548b, 0xa6},
    {0x548c, 0xb8}, {0x548d, 0xca}, {0x548e, 0xd7}, {0x548f, 0xe3}, {0x5490, 0x1d}, {0x5580, 0x04},
    {0x5583, 0x40}, {0x5584, 0x10}, {0x5586, 0x20}, {0x5587, 0x00}, {0x5588, 0x01}, {0x5589, 0x10},
    {0x558a, 0x00}, {0x558b, 0xf8}, {0x501d, 0x40}, {0x3008, 0x02}, {0x3c00, 0x04}, {DELAY, 300},
    // Window: sensor rows/columns (352,26)..(1792,1946), binned and scaled to 240x320
    {0x3800, 0x01}, {0x3801, 0x60}, {0x3802, 0x00}, {0x3803, 0x1a},
    {0x3804, 0x07}, {0x3805, 0x00}, {0x3806, 0x07}, {0x3807, 0x9a},
    {0x3808, CAM_W >> 8}, {0x3809, CAM_W & 0xff}, {0x380a, CAM_H >> 8}, {0x380b, CAM_H & 0xff},
    {0x380c, 0x0a}, {0x380d, 0x20}, {0x380e, 0x07}, {0x380f, 0x98},
    {0x3810, 0x00}, {0x3811, 0x10}, {0x3812, 0x00}, {0x3813, 0x0e}, {DELAY, 50},
    // Binning
    {0x3820, 0x01}, {0x3821, 0x00},  // no sensor flip: 0x07 gave black frames, flip_rows() does it instead
    {0x4514, 0xaa}, {0x4520, 0x0b}, {0x3814, 0x31}, {0x3815, 0x31},
    {DELAY, 50},
    // PLL for the 37.5MHz XCLK: about 20MHz PCLK
    {0x3039, 0x00}, {0x3034, 0x1a}, {0x3035, 0x11}, {0x3036, 0x0b}, {0x3037, 0x01}, {0x3108, 0x16},
    {0x3824, 0x04}, {0x460c, 0x22}, {0x3103, 0x13}, {DELAY, 50},
    // Output format Y8: one byte per pixel (Waveshare uses RGB565: 0x501f=0x01, 0x4300=0x61)
    {0x501f, 0x00}, {0x4300, 0x10}, {DELAY, 50},
};

static void wr(uint16_t reg, uint8_t v) {
    Wire1.beginTransmission(CAM_ADDR);
    Wire1.write(reg >> 8);
    Wire1.write(reg & 0xFF);
    Wire1.write(v);
    Wire1.endTransmission();
}

static bool rd(uint16_t reg, uint8_t *out) {
    Wire1.beginTransmission(CAM_ADDR);
    Wire1.write(reg >> 8);
    Wire1.write(reg & 0xFF);
    if (Wire1.endTransmission() != 0) return false;
    if (Wire1.requestFrom(CAM_ADDR, 1) != 1) return false;
    *out = Wire1.read();
    return true;
}

static enum { CAM_OFF, CAM_FAILED, CAM_IDLE, CAM_BUSY, CAM_READY } state = CAM_OFF;
static uint8_t frame[CAM_W * CAM_H];
static PIO pio;
static uint sm, prog_offset;
static int dma_ch;
static bool still_held;        // the app has `frame` as its still: stopping leaves it be
static bool zoomed;            // the sensor outputs CAM_W * CAM_ZOOM x CAM_H * CAM_ZOOM
static uint32_t armed_ms;
static bool asleep;
static char cam_err[40];   // shown on the scan screen, see platform_camera_error()

// Wait for the start of a frame, drop X + 1 bytes (none if X is 0: a band
// starts that far into the frame), then push one byte per PCLK while HREF is
// high. Dropping a byte takes as many cycles as pushing one.
static uint16_t prog_instr[11];
static const pio_program_t prog = {prog_instr, 11, -1};

static bool camera_init() {
    // 37.5MHz XCLK at the 150MHz system clock, as in the Waveshare demo
    uint32_t top = clock_get_hz(clk_sys) / 37000000 - 1;
    gpio_set_function(PIN_CAM_XCLK, GPIO_FUNC_PWM);
    pwm_config pc = pwm_get_default_config();
    pwm_config_set_wrap(&pc, top);
    pwm_init(pwm_gpio_to_slice_num(PIN_CAM_XCLK), &pc, true);
    pwm_set_gpio_level(PIN_CAM_XCLK, (top + 1) / 2);
    delay(50);

    Wire1.setSDA(PIN_CAM_SDA);
    Wire1.setSCL(PIN_CAM_SCL);
    Wire1.setClock(100000);
    Wire1.begin();

    uint8_t hi = 0, lo = 0;
    bool ok = rd(0x300A, &hi) && rd(0x300B, &lo);
    if (!ok || hi != 0x56 || lo != 0x40) {
        snprintf(cam_err, sizeof(cam_err), "Chip id 0x%02X%02X, not 0x5640", hi, lo);
        return false;
    }

    for (size_t i = 0; i < sizeof(init_regs) / sizeof(init_regs[0]); i++) {
        if (init_regs[i][0] == DELAY) delay(init_regs[i][1]);
        else wr(init_regs[i][0], init_regs[i][1]);
    }

    // (jmp targets are relative to the program: adding it relocates them)
    prog_instr[0] = pio_encode_wait_gpio(false, PIN_CAM_VSYNC);
    prog_instr[1] = pio_encode_wait_gpio(true, PIN_CAM_VSYNC);
    prog_instr[2] = pio_encode_jmp_not_x(7);
    prog_instr[3] = pio_encode_wait_gpio(true, PIN_CAM_HREF);      // drop
    prog_instr[4] = pio_encode_wait_gpio(true, PIN_CAM_PCLK);
    prog_instr[5] = pio_encode_wait_gpio(false, PIN_CAM_PCLK) | pio_encode_delay(1);
    prog_instr[6] = pio_encode_jmp_x_dec(3);
    prog_instr[7] = pio_encode_wait_gpio(true, PIN_CAM_HREF);      // push: wrap target
    prog_instr[8] = pio_encode_wait_gpio(true, PIN_CAM_PCLK);
    prog_instr[9] = pio_encode_in(pio_pins, 8);
    // one extra cycle so HREF has settled when the loop checks it again
    prog_instr[10] = pio_encode_wait_gpio(false, PIN_CAM_PCLK) | pio_encode_delay(1);
    if (!pio_claim_free_sm_and_add_program(&prog, &pio, &sm, &prog_offset)) return false;

    for (int i = 0; i < 8; i++) pio_gpio_init(pio, PIN_CAM_D0 + i);
    pio_gpio_init(pio, PIN_CAM_VSYNC);
    pio_gpio_init(pio, PIN_CAM_HREF);
    pio_gpio_init(pio, PIN_CAM_PCLK);

    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, prog_offset + 7, prog_offset + 10);
    sm_config_set_in_pins(&c, PIN_CAM_D0);
    sm_config_set_in_shift(&c, false, true, 8);   // autopush every byte
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    pio_sm_init(pio, sm, prog_offset, &c);

    dma_ch = dma_claim_unused_channel(true);
    return true;
}

// Output size, scaled by the ISP from the same window: the same view, more or fewer pixels.
static void set_size(int w, int h) {
    wr(0x3808, w >> 8); wr(0x3809, w & 0xff);
    wr(0x380a, h >> 8); wr(0x380b, h & 0xff);
}

// Unflipped, the image comes out mirrored top to bottom.
static void flip_rows() {
    static uint8_t tmp[CAM_W];
    for (int y = 0; y < CAM_H / 2; y++) {
        uint8_t *a = frame + y * CAM_W, *b = frame + (CAM_H - 1 - y) * CAM_W;
        memcpy(tmp, a, CAM_W);
        memcpy(a, b, CAM_W);
        memcpy(b, tmp, CAM_W);
    }
}

static void set_focus(bool on) {
    // 0x3603: bit 7 power down, bits 5:0 D[9:4]. 0x3602: bits 7:4 D[3:0], low bits 0 = jump directly
    wr(0x3602, on ? (CAM_FOCUS & 0x0F) << 4 : 0x00);
    wr(0x3603, on ? (CAM_FOCUS >> 4) & 0x3F : 0x80);
}

// Restart at the next frame, dropping its first `skip` bytes. The count goes
// in through the TX FIFO, given back for a moment (the RX one has joined it).
static void restart_sm(uint32_t skip) {
    pio_sm_set_enabled(pio, sm, false);
    pio_sm_restart(pio, sm);
    hw_clear_bits(&pio->sm[sm].shiftctrl, PIO_SM0_SHIFTCTRL_FJOIN_RX_BITS);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_put(pio, sm, skip ? skip - 1 : 0);
    pio_sm_exec(pio, sm, pio_encode_pull(false, true));
    pio_sm_exec(pio, sm, pio_encode_mov(pio_x, pio_osr));
    hw_set_bits(&pio->sm[sm].shiftctrl, PIO_SM0_SHIFTCTRL_FJOIN_RX_BITS);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_exec(pio, sm, pio_encode_jmp(prog_offset));
}

static void arm() {
    restart_sm(0);
    dma_channel_config dc = dma_channel_get_default_config(dma_ch);
    channel_config_set_read_increment(&dc, false);
    channel_config_set_write_increment(&dc, true);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_8);
    channel_config_set_dreq(&dc, pio_get_dreq(pio, sm, false));
    dma_channel_configure(dma_ch, &dc, frame, &pio->rxf[sm], sizeof(frame), true);

    pio_sm_set_enabled(pio, sm, true);
    armed_ms = millis();
    state = CAM_BUSY;
}

const uint8_t *platform_camera_frame(int *width, int *height) {
    if (state == CAM_OFF) state = camera_init() ? CAM_IDLE : CAM_FAILED;
    if (state == CAM_FAILED) return nullptr;

    if (asleep) {
        wr(0x3008, 0x02);
        asleep = false;
    }
    if (zoomed) {   // back from reading a page; the first frame may be torn, the preview shrugs it off
        set_size(CAM_W, CAM_H);
        zoomed = false;
        cam_err[0] = 0;   // a band's error was the reading screen's to show, not the preview's
    }
    if (state == CAM_BUSY) {
        if (dma_channel_is_busy(dma_ch)) {
            if (millis() - armed_ms < 1000) return nullptr;
            // no VSYNC/PCLK, or fewer bytes than expected
            snprintf(cam_err, sizeof(cam_err), "No image within 1s");
            dma_channel_abort(dma_ch);
            state = CAM_IDLE;
        } else {
            state = CAM_READY;
            flip_rows();
            cam_err[0] = 0;
            *width = CAM_W;
            *height = OUT_H;
            return frame + OUT_Y * CAM_W;
        }
    }
    // CAM_IDLE, or CAM_READY once the app is done with the previous frame
    if (state == CAM_IDLE) set_focus(true);
    arm();
    return nullptr;
}

const char *platform_camera_error() { return cam_err[0] ? cam_err : nullptr; }

// A still: the rows of the last frame the preview showed, moved to the start
// of the frame buffer, which is lent to the app: there's no RAM for a copy.
// (The rows above and below were never on screen: other paper on the desk.)
uint8_t *platform_camera_still(int *width, int *height, size_t *room) {
    if (state == CAM_BUSY) {   // the preview re-arms every other tick: wait for that frame
        uint32_t t0 = millis();
        while (dma_channel_is_busy(dma_ch) && millis() - t0 < 1000) {}
        if (dma_channel_is_busy(dma_ch)) return nullptr;
        state = CAM_READY;
        flip_rows();
    }
    if (state != CAM_READY) return nullptr;
    still_held = true;
    memmove(frame, frame + OUT_Y * CAM_W, OUT_H * CAM_W);
    memset(frame + OUT_H * CAM_W, 0, sizeof(frame) - OUT_H * CAM_W);
    *width = CAM_W;
    *height = OUT_H;
    *room = sizeof(frame);
    return frame;
}

void platform_camera_still_free() {
    if (still_held) memset(frame, 0, sizeof(frame));
    still_held = false;
}

// Waits for n frames to start (VSYNC rising), with a timeout.
static bool wait_frames(int n) {
    uint32_t t0 = millis();
    for (int i = 0; i < n; i++) {
        while (gpio_get(PIN_CAM_VSYNC)) if (millis() - t0 > 1500) return false;
        while (!gpio_get(PIN_CAM_VSYNC)) if (millis() - t0 > 1500) return false;
    }
    return true;
}

bool platform_camera_band(int y0, int rows, uint8_t *buf) {
    const int zw = CAM_W * CAM_ZOOM, zh = CAM_H * CAM_ZOOM;
    if (state == CAM_OFF || state == CAM_FAILED || y0 < 0 || rows <= 0 || y0 + rows > OUT_H * CAM_ZOOM) return false;
    y0 += OUT_Y * CAM_ZOOM;   // the still's rows are the preview's: the middle of the frame
    if (state == CAM_BUSY) dma_channel_abort(dma_ch);
    pio_sm_set_enabled(pio, sm, false);
    state = CAM_IDLE;
    bool settle = asleep || !zoomed;
    if (asleep) {
        wr(0x3008, 0x02);
        asleep = false;
        set_focus(true);
    }
    if (!zoomed) {
        set_size(zw, zh);
        zoomed = true;
    }
    // the frame under way when the size or power changed can be torn: skip it
    if (settle && !wait_frames(2)) {
        snprintf(cam_err, sizeof(cam_err), "No image within 1s");
        return false;
    }
    // Rows come bottom up (see flip_rows): the band is the sensor's rows
    // [zh - y0 - rows, zh - y0); the PIO program drops the rows before it.
    // Now and then a whole frame comes out one flat shade (a full 480x640 of
    // it, measured on the board): that band is taken again, from the next frame.
    uint32_t n = uint32_t(rows) * zw;
    for (int tries = 1;; tries++) {
        restart_sm(uint32_t(zh - y0 - rows) * zw);
        dma_channel_config dc = dma_channel_get_default_config(dma_ch);
        channel_config_set_read_increment(&dc, false);
        channel_config_set_write_increment(&dc, true);
        channel_config_set_transfer_data_size(&dc, DMA_SIZE_8);
        channel_config_set_dreq(&dc, pio_get_dreq(pio, sm, false));
        dma_channel_configure(dma_ch, &dc, buf, &pio->rxf[sm], n, true);
        pio_sm_set_enabled(pio, sm, true);
        uint32_t t0 = millis();
        while (dma_channel_is_busy(dma_ch) && millis() - t0 < 1000) {}
        bool ok = !dma_channel_is_busy(dma_ch);
        dma_channel_abort(dma_ch);
        pio_sm_set_enabled(pio, sm, false);
        if (!ok) {
            snprintf(cam_err, sizeof(cam_err), "No image within 1s");
            memset(buf, 0, n);
            return false;
        }
        uint8_t lo = 255, hi = 0;
        for (uint32_t i = 0; i < n; i += 7) { lo = buf[i] < lo ? buf[i] : lo; hi = buf[i] > hi ? buf[i] : hi; }
        if (hi - lo >= 8) break;
        if (tries == 3) {
            snprintf(cam_err, sizeof(cam_err), "Blank picture, 3 tries");
            return false;
        }
    }
    static uint8_t tmp[CAM_W * CAM_ZOOM];
    for (int y = 0; y < rows / 2; y++) {
        uint8_t *a = buf + y * zw, *b = buf + (rows - 1 - y) * zw;
        memcpy(tmp, a, zw);
        memcpy(a, b, zw);
        memcpy(b, tmp, zw);
    }
    memset(tmp, 0, sizeof(tmp));
    return true;
}

void platform_camera_stop() {
    if (state == CAM_OFF || state == CAM_FAILED || asleep) return;
    dma_channel_abort(dma_ch);
    pio_sm_set_enabled(pio, sm, false);
    // the frame can be a picture of a seed QR (or the app's still: wiped when it's done)
    if (!still_held) memset(frame, 0, sizeof(frame));
    set_focus(false);
    wr(0x3008, 0x42);   // software power down
    asleep = true;
    state = CAM_IDLE;
}
