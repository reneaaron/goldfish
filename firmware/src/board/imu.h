#pragma once

// QMI8658 accelerometer on I2C0, used only to tell which way up the board is
// held. Call after touch_init(), which starts the I2C0 bus.
void imu_init();

// Call from loop(): flips the screen 180° when the board is held upside down.
// Lying flat keeps the current way up.
void imu_poll();
