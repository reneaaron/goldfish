# Case

One-piece printed case for the Waveshare RP2350-Touch-LCD-2, modelled in
[gcad](https://github.com/v0l/gcad). Outside: 40.9 × 62.6 × 18.9 mm.

![Case](renders/case.png)

No screws or other parts: two flexing tabs on each long wall catch the glass
edge, and the board rests on four posts behind it.

## Files

- `parts/case.gcad`: the case
- `parts/board.gcad`: stand-in model of the board and camera, for fit checks
- `stateless-signer.gasm`: case + board, with an interference check
- `exports/`: STL for printing, STEP of the whole assembly
- `renders/`: engineering views from gcad

```bash
gcad check stateless-signer.gasm                 # fit check
gcad export parts/case.gcad exports/case.stl
```

## Printing

Flat on its back, no supports. PLA works; the clip tabs flex about 1%.

## Where the dimensions come from

From Waveshare's [outline drawing](https://www.waveshare.com/img/devkit/RP2350-Touch-LCD-2/RP2350-Touch-LCD-2-details-size.jpg):
glass 37.1 × 58.8 R2.6, PCB 35 × 48.2, M2 standoffs on 29 × 42.2, 7.2 from
glass front to PCB back, 9.7 to the standoff ends, buttons 18.9 apart.

Measured off the photos in that drawing, good to about 0.3 mm:

- Header pins end 8.64 mm behind the PCB (standard 2.54 mm header). The case
  leaves 9.2 (`hdr`).
- The PCB sits 0.3 mm toward the USB end of the glass (`oy`).
- The USB-C socket ends about 5.6 mm inside the outer wall, so the plug's
  overmould has to pass through the wall. The cutout is 14 × 8 mm for that.
- BOOT and RST are side buttons about 7 mm behind their 2.6 mm holes. Press
  them with a paperclip.

## Not published, measure before printing

Waveshare gives no drawing for the bundled OV5640 module (flex marked
`DC-5M21-5640`). The camera seat is built on these guesses:

| Variable | Guess | What to measure |
|---|---|---|
| `camw` | 8.7 | square base of the module, plus 0.2 |
| `lens` | 7.8 | round lens barrel diameter, plus 0.6 |
| `ped` | 2.4 | barrel height above the square base, minus 0.6 |
| `camy` | 0.5 | flex length from the connector to the base; 7.4 assumed |

The module pushes into the frame on the case floor, lens through the back,
and is held by friction only.
