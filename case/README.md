# Case

One-piece printed case for the Waveshare RP2350-Touch-LCD-2, modelled in
[gcad](https://github.com/v0l/gcad). Outside: 40.9 × 62.6 × 17.7 mm.

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

## Estimated, check on the first print

Board outline, glass and mounting holes come from Waveshare's drawing. These
were measured off product photos and may need adjusting: camera module size
(`camw`, `lens`) and position (`camx`, `camy`), and USB-C and button positions.
