Test SeedQR images from SeedSigner's docs/seed_qr (MIT license, Copyright (c) 2021 SeedSigner).
They encode the public test vectors from that spec. Never real funds.

`card_photo_zero_seed_*.png`: phone photos of a hand-filled paper SeedQR
card holding the all-zero Compact SeedQR ("abandon" x11 "about",
fingerprint 73c5da0a). The signer's scanner (quirc) reads neither yet:
`_shadow` loses the bottom-left finder square to uneven light, `_lines` the
top-right one to grid lines printed through its ring (a since-fixed problem
of the card template). zxing reads both. See issue #35.

A larger set of real QR photos for scanner work is zxing's blackbox corpus
(Apache-2.0), `core/src/test/resources/blackbox/qrcode-1` to `-6` at commit
cbc6f347928ffc3daf1990c8670e09b7d3ab8a63: 179 photos, 14 MB, not copied here.
Fitted to the board's 240x180 frame, quirc reads 70 of them and zxing 141.
