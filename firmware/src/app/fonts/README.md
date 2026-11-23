Generated with lv_font_conv 1.5.2 (4 bpp, uncompressed):

- inter_13 (Medium), inter_15 (Regular), inter_17_semi: Inter 4.1, SIL OFL 1.1
- rethink_24, rethink_32: Rethink Sans ExtraBold (wght 800 instance of the Google Fonts variable font, `fonttools varLib.instancer`), SIL OFL 1.1, see LICENSE-RethinkSans.txt
- mono_16, mono_22, mono_26: JetBrains Mono 2.304 Medium, SIL OFL 1.1
- icons in inter_13/15/17 (inter_13 also has wifi-off and bluetooth-off), icons_17 (maximize only, the full-screen QR button), icons_34 (plus shield-check for the home badge) and icons_40 (dice faces, coin sides): Lucide 0.460.0 (`lucide-static` npm package, font/lucide.ttf), ISC, see LICENSE-Lucide.txt. Codepoints are listed in ../theme.h.

After generating inter_13/15/17_semi, lower the Lucide glyphs by 2px (`.ofs_y -= 2` on the last 20 `glyph_dsc` entries, 22 in inter_13): Lucide sits on the baseline, so unshifted icons centre 2px above the cap height of the text beside them.
