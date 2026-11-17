# Hardware

No custom PCB: everything is off the shelf.

## Parts

| Part | Notes |
|---|---|
| Waveshare RP2350-Touch-LCD-2-C (SKU 30718) | RP2350A, 16MB flash, 2" 240×320 IPS with capacitive touch (CST816D), microSD, LiPo charger. The -C bundle includes an OV5640 camera. [Product page](https://www.waveshare.com/rp2350-touch-lcd-2.htm?sku=30718) |
| or: RP2350-Touch-LCD-2 (SKU 30711) + an OV5640 module | Base board without camera. The camera must use the 24-pin 0.5mm DVP connector (ESP32-CAM style). Check its pinout against the board's J1 before plugging in. A 60–75° lens suits QR scanning better than wide-angle ones |
| Printed case | See [`../case`](../case). Clip-in, no screws |
| USB-C data cable | Power and flashing |

No battery for now: the device runs from USB-C. The board has a PH1.25 2-pin
battery connector and an ETA6098 charger if that changes later.

## Board notes

- No Wi-Fi or Bluetooth hardware on the board (the RP2350 has no radio).
- Pin map: [`../firmware/include/board_pins.h`](../firmware/include/board_pins.h),
  taken from Waveshare's demo code.
- The firmware has no USB stack: the running device doesn't show up on a
  computer. USB-C is power, plus flashing through the ROM bootloader.
- Buttons on the board: RST and BOOT only, reachable through pinholes in the
  case. BOOT plus plugging in USB-C enters the UF2 bootloader for flashing.
- Waveshare wiki: https://www.waveshare.com/wiki/RP2350-Touch-LCD-2
