#pragma once
// Waveshare RP2350-Touch-LCD-2 pin map, taken from Waveshare's demo code
// (RP2350-Touch-LCD-2.zip: Arduino/DEV_Config.h and C/02-CAM/lib/CAM/cam.h).

// LCD, SPI0
#define PIN_LCD_SCK   18
#define PIN_LCD_MOSI  19
#define PIN_LCD_CS    17
#define PIN_LCD_DC    16
#define PIN_LCD_RST   20   // shared with the touch controller reset
#define PIN_LCD_BL    15

// I2C0: CST816D touch (0x15) and QMI8658 IMU
#define PIN_I2C0_SDA  12
#define PIN_I2C0_SCL  13
#define PIN_TOUCH_INT 29
#define PIN_IMU_INT1  14
#define TOUCH_ADDR    0x15

// Battery voltage divider
#define PIN_BAT_ADC   28

// Camera (OV5640 over DVP, read by PIO). Pin order from Waveshare's
// picampinos.pio: GPIO0..7 D2..D9, then VSYNC, HREF, PCLK.
#define PIN_CAM_D0    0
#define PIN_CAM_VSYNC 8
#define PIN_CAM_HREF  9
#define PIN_CAM_PCLK  10
#define PIN_CAM_XCLK  11   // PWM clock to the sensor
#define PIN_CAM_SDA   22   // SCCB on I2C1
#define PIN_CAM_SCL   23

#define LCD_W 240
#define LCD_H 320
