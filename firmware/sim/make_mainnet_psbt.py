"""Writes sim/samples/mainpsbt_height_demise_base64.png: the same transaction as
SeedSigner's testnet vector (testpsbt_height_demise_base64.png), on mainnet, so
the screenshots in docs/screens show no TESTNET label.

Spends a made-up 25,000-sat coin of the public test seed "height demise ..."
(m/84'/0'/0'/0/0) to two addresses of the public BIP-39 test seed "abandon ...
about": 15,000 to its P2SH-P2WPKH and 9,857 to its P2WPKH address, fee 143,
no change. The coin doesn't exist; nothing here is real funds.

Also writes the same transaction for the made-up test seed on
handwritten_page_demo.png ("blossom network ..."), with a SeedQR of it, so the
landing page's hero has a paper backup, a SeedQR and a transaction of one seed:
demopsbt_blossom_base64.png and demoseed_blossom_12word.png.

Run from firmware/:
    uv run --with-requirements e2e/requirements.txt python sim/make_mainnet_psbt.py
"""
import hashlib

import qrcode
from embit import bip32, bip39, script
from embit.networks import NETWORKS
from embit.psbt import PSBT, DerivationPath
from embit.transaction import Transaction, TransactionInput, TransactionOutput

NET = NETWORKS["main"]


def root(mnemonic):
    return bip32.HDKey.from_seed(bip39.mnemonic_to_seed(mnemonic))


theirs = root("abandon " * 11 + "about")


def qr_png(text, out):
    qr = qrcode.QRCode(error_correction=qrcode.constants.ERROR_CORRECT_L, box_size=3, border=4)
    qr.add_data(text)
    qr.make(fit=True)
    qr.make_image().convert("1").save(out)


def write_psbt(mnemonic, out):
    ours = root(mnemonic)
    path = "m/84h/0h/0h/0/0"
    key = ours.derive(path).key.get_public_key()
    coin = TransactionOutput(25000, script.p2wpkh(key))

    p2sh = script.p2sh(script.p2wpkh(theirs.derive("m/49h/0h/0h/0/0").key.get_public_key()))
    p2wpkh = script.p2wpkh(theirs.derive("m/84h/0h/0h/0/0").key.get_public_key())

    tx = Transaction(version=2,
                     vin=[TransactionInput(hashlib.sha256(b"sample coin").digest(), 0, sequence=0xFFFFFFFD)],
                     vout=[TransactionOutput(15000, p2sh), TransactionOutput(9857, p2wpkh)])
    psbt = PSBT(tx)
    psbt.inputs[0].witness_utxo = coin
    psbt.inputs[0].bip32_derivations[key] = DerivationPath(ours.my_fingerprint, bip32.parse_path(path))

    text = psbt.to_string()
    qr_png(text, out)
    print(out, len(text), "chars,", p2sh.address(NET), p2wpkh.address(NET))


write_psbt("height demise useless trap grow lion found off key clown transfer enroll",
           "sim/samples/mainpsbt_height_demise_base64.png")

demo = "blossom network smile solid detect anxiety beyond column fence task comic guitar"
write_psbt(demo, "sim/samples/demopsbt_blossom_base64.png")
qr_png("".join("%04d" % bip39.WORDLIST.index(w) for w in demo.split()), "sim/samples/demoseed_blossom_12word.png")
