Test SeedQR images from SeedSigner's docs/seed_qr (MIT license, Copyright (c) 2021 SeedSigner).
They encode the public test vectors from that spec. Never real funds.
mainpsbt_height_demise_base64.png is made by sim/make_mainnet_psbt.py: the testpsbt transaction on
mainnet, spending a made-up coin. Never real funds.
handwritten_page.png is synthetic (handwriting/make_page.py, from EMNIST letters and OFL
handwriting fonts): a made-up 24-word test seed, never real funds.
handwritten_page_demo.png is the same kind of page (make_page.py --seed 5 --pad 80), one the
firmware reads 24 of 24; the browser simulator (/try/) offers it. Also never real funds.
