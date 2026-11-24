# Sourcing research

Snapshot from November 2026, taken from web listings; prices rechecked
24 November 2026. Waveshare's own pages, Amazon and Newegg block automated reads, so
their figures come from search results. Prices and stock
change; nothing here was ordered or checked live. The question: can people
around the world get the hardware easily, compared with SeedSigner's
Pi Zero v1.3 build, and which second board could we port to if supply of
the current one fails.

## Current board vs SeedSigner

### Price

| | This project | SeedSigner |
|---|---|---|
| Main board | RP2350-Touch-LCD-2-C, $19.99 with OV5640 ($14.99 without) | Pi Zero v1.3: ~$10–16 at official resellers (Adafruit $10, limit 2), ~$20–26 elsewhere |
| Display | built in | Waveshare 1.3" 240×240 HAT, $13.99 at Waveshare, ~$10–16 at resellers |
| Camera | included in -C | OV5647 Zero camera, ~$9–20 |
| microSD | not needed | ~$10–20 (cheapest 32 GB ~$9–11, SanDisk/PNY ~$15–20) |
| Total (no case) | ~$20 | ~$40–80 |
| Soldering | none | header pins on the Zero v1.3, unless bought pre-soldered |

### Availability

| | RP2350-Touch-LCD-2 | Pi Zero v1.3 |
|---|---|---|
| Who makes it | Waveshare only (Spotpear sells the same board) | Raspberry Pi |
| Production guarantee | none published; launched ~2025 | until at least January 2030 |
| Stock | in stock at OpenELAB and The Pi Hut (base board) | sold out at Adafruit, The Pi Hut, Pimoroni and BerryBase on 24 November 2026; Adafruit caps orders at 2 |
| If a part is missing | hard to swap: firmware is tied to this board | easy to swap (other Pi models, any OV5647 camera), but 3–4 parts from several shops |

Summary: our board is under half the price, one part, no soldering. Its
weak spot is supply: one vendor, one SKU, no lifetime guarantee. The Pi
Zero is guaranteed and sold by many shops but often out of stock.

## Candidate second boards

All boards with Wi-Fi or Bluetooth would break the README claim "no Wi-Fi
or Bluetooth hardware". Every ESP32 except the P4 has a radio in the chip.
The P4 has none, but most P4 display boards add an ESP32-C6 radio chip.

| Board | Price | Radio | Fit | Porting effort |
|---|---|---|---|---|
| Waveshare ESP32-S3-Touch-LCD-2-C | $21.99 with OV5640 ($16.99 without) | yes, in the chip | same 2" 240×320 screen, same ST7789 display and CST816D touch chips, same OV5640 bundle, 8MB PSRAM; case fit not checked | low: `src/app` carries over, new display/touch/camera/main |
| M5Stack CoreS3 | $54.51 | yes, in the chip | 2" touch screen, 0.3MP (VGA) camera | medium |
| Waveshare ESP32-P4-WiFi6-Touch-LCD-3.5 | $39.99 with OV5647 (search snippet; page not loaded) | yes, ESP32-C6 on the board | 320×480, one board | medium |
| Waveshare ESP32-P4-Pico + 5-DSI-TOUCH-A + OV5647 | ~$11 + ~$35–37 + ~$10–15 ≈ $55–65 | none | 5" 720×1280, 3 parts plus cable and 5V wiring | medium to high: new screen sizes, MIPI drivers on ESP-IDF |

Prior art: [Kern](https://github.com/cyb650/Kern) is a C signer in the style
of Krux that already runs on these P4 boards, including the radio-less
P4-Pico. Krux's own K210 boards are not an option: the Maix Amigo is
discontinued and the rest are niche.

## Worldwide availability ratings

Scores are for how easy it is to buy in many countries, not for price or
fit.

| Rank | Option | Score | Where to buy | Weak spots |
|---|---|---|---|---|
| 1 | M5Stack CoreS3 | 4/5 | Mouser in about 30 countries, the M5Stack store, Amazon | ~$55, radio, VGA camera; Mouser stock of a few hundred |
| 2 | RP2350-Touch-LCD-2-C (current) | 3/5 | Waveshare direct (worldwide), Amazon US, Newegg, OpenELAB (EU), Little Bird (AU), Switch Science (JP), Fruugo | Pi Hut UK has the base board only (£14.50, in stock); not on Mouser or DigiKey; no reseller found in India or Brazil |
| 2 | ESP32-S3-Touch-LCD-2-C | 3/5 | same Waveshare network; Pi Hut UK in stock at £17.10 | not on Mouser or DigiKey; same single vendor |
| — | SeedSigner on Pi Zero v1.3 (reference) | 3/5 | official Raspberry Pi resellers in most countries | stock-outs, order limits, several parts |
| 4 | ESP32-P4-WiFi6-Touch-LCD-3.5 | 2/5 | Waveshare direct | no resellers confirmed; newer product |
| 5 | ESP32-P4-Pico kit | 1–2/5 | eBay (Waveshare's store), one Amazon listing | out of stock on Waveshare's site; 3 parts that all need to be in stock |

What this means:

- Only the CoreS3 comes from another vendor with a global distributor.
  Every other option depends on Waveshare's reseller network, so the
  ESP32-S3 twin protects against one SKU going away, not against relying on
  Waveshare.
- Every Waveshare board can be ordered direct from China, at the cost of
  shipping time, customs and tariffs.
- The radio-less P4 route is the hardest to buy today.
- India, Brazil and Africa: no confirmed local reseller for any of these
  boards. Waveshare lists Indian distributors (robu.in, Evelta) but stock
  of these exact boards wasn't confirmed.

## Open questions

- Does the printed case fit the ESP32-S3-Touch-LCD-2 (same screen, PCB
  outline not compared)?
- Would a radio-present board ship under different copy ("radio present,
  never enabled by firmware"), as Blockstream Jade does?
- Watch for a small radio-less ESP32-P4 board with a built-in screen; the
  Crux project plans to move to one.

## Sources

- [Waveshare RP2350-Touch-LCD-2](https://www.waveshare.com/rp2350-touch-lcd-2.htm)
- [Waveshare ESP32-S3-Touch-LCD-2](https://www.waveshare.com/product/esp32-s3-touch-lcd-2.htm)
- [Waveshare ESP32-P4-Pico](https://www.waveshare.com/esp32-p4-pico.htm)
- [Waveshare 5-DSI-TOUCH-A](https://www.waveshare.com/wiki/5-DSI-TOUCH-A)
- [Waveshare ESP32-P4-WiFi6-Touch-LCD-3.5](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-3.5.htm)
- [Waveshare distributors](https://www.waveshare.com/distributors)
- [Mouser: CoreS3 announcement](https://www.mouser.in/newsroom/publicrelations-m5stack-cores3-iot-kit-2023final)
- [CNX: M5Stack CoreS3](https://www.cnx-software.com/2023/05/05/60-m5stack-cores3-esp32-s3-iot-controller-comes-with-2-inch-display-vga-camera-multiple-sensors/)
- [The Pi Hut: RP2350 2"](https://thepihut.com/products/rp2350-2-capacitive-touch-display-dev-board-with-6-axis-imu)
- [The Pi Hut: Pi Zero](https://thepihut.com/products/raspberry-pi-zero)
- [PiShop: Pi Zero](https://www.pishop.us/product/raspberry-pi-zero/)
- [Raspberry Pi lifecycle](https://raspberry.tips/en/raspberrypi-infos/raspberry-pi-prices-lifecycle)
- [SeedSigner hardware](https://seedsigner.com/hardware)
- microSD prices: [BasketPrice](https://www.basketprice.com/sale/microsd-card), [Storage Price Tracker](https://storagepricetracker.com/ItemDetails/MicroSD/B0H6K393TQ), [Walmart](https://www.walmart.com/c/kp/micro-sd-32-cards), [Staples](https://www.staples.com/32GB-microSD-Memory-Cards/cat_CL142162/8mtjb-8mx01)
- [Waveshare 1.3" LCD HAT](https://www.waveshare.com/product/raspberry-pi/displays/1.3inch-lcd-hat.htm)
- [Kern](https://github.com/cyb650/Kern), [Crux](https://github.com/pythcoiner/Crux)
- [Krux parts list](https://selfcustody.github.io/krux/parts/)
