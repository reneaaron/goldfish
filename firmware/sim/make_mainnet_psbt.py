"""Writes sim/samples/mainpsbt_height_demise_base64.png: the same transaction as
SeedSigner's testnet vector (testpsbt_height_demise_base64.png), on mainnet, so
the screenshots in docs/screens show no TESTNET label.

Spends a made-up 25,000-sat coin of the public test seed "height demise ..."
(m/84'/0'/0'/0/0) to two addresses of the public BIP-39 test seed "abandon ...
about": 15,000 to its P2SH-P2WPKH and 9,857 to its P2WPKH address, fee 143,
no change. The coin doesn't exist; nothing here is real funds.

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
OUT = "sim/samples/mainpsbt_height_demise_base64.png"


def root(mnemonic):
    return bip32.HDKey.from_seed(bip39.mnemonic_to_seed(mnemonic))


ours = root("height demise useless trap grow lion found off key clown transfer enroll")
theirs = root("abandon " * 11 + "about")

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
qr = qrcode.QRCode(error_correction=qrcode.constants.ERROR_CORRECT_L, box_size=3, border=4)
qr.add_data(text)
qr.make(fit=True)
qr.make_image().convert("1").save(OUT)
print(OUT, len(text), "chars,", p2sh.address(NET), p2wpkh.address(NET))

# Two made-up coins of the same seed in one transaction, without their previous
# transactions, as Sparrow's QR sends them: what SIM_SHOTS shows for the
# refusal (TWO_COINS_PSBT in src/sim/main.cpp).
path1 = "m/84h/0h/0h/0/1"
key1 = ours.derive(path1).key.get_public_key()
two = PSBT(Transaction(version=2,
                       vin=[TransactionInput(hashlib.sha256(b"sample coin %d" % i).digest(), 0, sequence=0xFFFFFFFD) for i in (0, 1)],
                       vout=[TransactionOutput(39800, p2wpkh)]))
two.inputs[0].witness_utxo = coin
two.inputs[0].bip32_derivations[key] = DerivationPath(ours.my_fingerprint, bip32.parse_path(path))
two.inputs[1].witness_utxo = TransactionOutput(15000, script.p2wpkh(key1))
two.inputs[1].bip32_derivations[key1] = DerivationPath(ours.my_fingerprint, bip32.parse_path(path1))
print("TWO_COINS_PSBT", two.to_string())
