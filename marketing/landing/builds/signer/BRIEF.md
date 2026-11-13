# Stateless Signer: landing page brief

Interviewed 2026-11-13. Questions 1, 7, 8 and the peak answered by the owner;
questions 2, 3, 5, 6 were offered with defaults and the owner moved on, so the
defaults below are **proposed, not confirmed**.

## Interview

1. **Vibe:** "Paper and ink zine". Matches the firmware's Paper theme and the
   paper SeedQR card. Cream paper, ink black, one rust accent, Fraunces serif.
2. **Journey (default):** board alone (off-the-shelf) → case closes around it →
   screen wakes, scan seed QR → review tx → slide to sign, signed QR out →
   unplug, screen dark, seed gone → "build one".
3. **Energy (default):** calm open, build-up during assembly, loudest at sign,
   dead quiet at unplug, steady close.
4. **Peak:** "diy signing device".
5. **Never-seen-before:** a first attempt had the page keep the reader's ink
   only while the signer was powered, wiped at the unplug frame. The owner had
   it removed on sight: a page that paints under the cursor reads as broken
   before it reads as a metaphor. The page currently ships with no bespoke
   signature move, which is a knowing gap against the skill's rules.
6. **Range (default):** stay near premium-minimal, tilted to printed zine.
7. **Structure:** "One unbroken world".
8. **Assets:** "Render new Blender sequences" from the real case STL and the
   firmware's real simulator screens. No generated imagery.

## Feeling curve

| Leg | Feeling | What causes it |
|---|---|---|
| 1 Board | Curiosity | A bare screen-on-a-board turning slowly on paper. "That's it?" |
| 2 Case | Ownership (PEAK) | The board drops into a printed shell and clicks home. You could make this. |
| 3 Scan | Trust | The device flips; its own camera reads a paper SeedQR card. Seed loaded. |
| 4 Sign | Control | Close on the screen: amount, fee, slide to sign, signed QR. |
| 5 Unplug | Relief, quiet | The cable pulls out, the screen goes black. Nothing left behind. |
| Close | Resolve | Top-down on paper: device, card, cable. "Build yours." |

## Peak

"You watch a cheap board drop into a printed case and become a Bitcoin signer."
Lives in leg 2, which owns the most scroll.

## Tell-someone sentence

It's the site where you build a Bitcoin signer by scrolling, and when you
unplug it the page forgets everything you scribbled on it.

## Authored silence

Leg 5 (unplug): no copy for the first half while the screen dies. Intentional.

## Copy rules (from marketing/README.md)

- "No Wi-Fi or Bluetooth hardware" ok. "Nothing is saved" ok (RAM only).
- Not "air-gapped" while USB-powered. No audits, secure boot, or tested wallet
  compatibility claims. Early, not for real funds: say so on the page.

## Verified (2026-11-13)

Desktop 1440x900, phone 390x844, and a reduced-motion pass, all against
`http://localhost:4512`.

- No dead scroll. All 5 legs reach full opacity and paint a real frame.
- Contrast at the worst frame under every line: desktop 5.68 and up, phone 8.1
  and up, reduced motion 6.48 and up. No console errors in any pass.
- Each scrim is sized to its copy block's measured rect every frame, not to a
  guessed corner. Guessed geometry failed twice: the blocks are different
  heights, and the case block grew again when the hero's leading was fixed.
- `worldflight-assert.mjs`: 22 of 24. The two it reports are both expected.
  - *"lerp rate is the 0.18 default"*: the page sets 0.12, which
    `references/worldflight.md` §7c recommends for a worldflight.
  - *"outgoing leg only releases once it is fully covered"*: a one-pixel
    boundary artifact. The harness samples the seam band's last point at
    `boundary + half`; probing by hand, the outgoing leg holds opacity 1
    through +72px and drops to 0 at +73px, with the incoming leg already at 1.
    No page ground shows through the seam.
- The harness's FROZEN CLIP warnings are act-mode heuristics: in worldflight a
  leg legitimately idles on its first frame before it is current and holds its
  last frame after, because every leg stays mounted and no `src` is ever
  swapped.
- The ink signature move was removed at the owner's request, along with its
  canvas, its RAM readout and the hero's nudge to use it.

### Not verified

- A real phone. Headless Chrome cannot reproduce iOS autoplay policy, Low Power
  Mode, the video decoder or touch scrolling.
- The page has not been looked at by a human on any device yet.
