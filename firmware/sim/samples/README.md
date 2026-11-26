Test SeedQR images from SeedSigner's docs/seed_qr (MIT license, Copyright (c) 2021 SeedSigner).
They encode the public test vectors from that spec. Never real funds.
mainpsbt_height_demise_base64.png is made by sim/make_mainnet_psbt.py: the testpsbt transaction on
mainnet, spending a made-up coin. Never real funds.
handwritten_page.png is synthetic (handwriting/make_page.py, from EMNIST letters and OFL
handwriting fonts): a made-up 24-word test seed, never real funds.
handwritten_page_demo.png is the same kind of page with the 12 words of the height demise test seed
(make_page.py --words 12 --seed 20 --pad 80 --mnemonic ...), a layout the firmware reads 12 of 12;
the landing page offers it next to that seed's SeedQR and mainnet transaction. Never real funds.
