Test SeedQR images from SeedSigner's docs/seed_qr (MIT license, Copyright (c) 2021 SeedSigner).
They encode the public test vectors from that spec. Never real funds.
mainpsbt_height_demise_base64.png is made by sim/make_mainnet_psbt.py: the testpsbt transaction on
mainnet, spending a made-up coin. Never real funds.
handwritten_page.png is synthetic (handwriting/make_page.py, from EMNIST letters and OFL
handwriting fonts): a made-up 24-word test seed, never real funds.
handwritten_page_demo.png is a synthetic photo of the project's word card (cards/), filled in with the
12 words of the height demise test seed and its fingerprint (handwriting/make_card.py, the command in
its docstring), which the firmware reads 12 of 12; the landing page offers it with that seed's
mainnet transaction, and the screenshots' scan clip shows it. Never real funds.
