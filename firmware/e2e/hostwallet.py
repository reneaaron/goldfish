"""The wallet app on the other side of the QR codes: a small stand-in for
Sparrow, built on embit. It never sees the signer's seed: it gets the xpub
from the signer's screen, makes descriptors and PSBTs, shows them as QR codes
the way Sparrow does, and judges what comes back.

Nothing is broadcast and no coins exist: the "funding" transactions are made
up, which a PSBT signer can't tell and doesn't need to.
"""
import hashlib
import itertools
import re
import sys
from pathlib import Path

from embit import bip32, bip39, ec, finalizer, script
from embit.descriptor import Descriptor
from embit.descriptor.checksum import add_checksum
from embit.networks import NETWORKS
from embit.psbt import PSBT, DerivationPath
from embit.transaction import Transaction, TransactionInput, TransactionOutput
from urtypes.crypto import (SCRIPT_EXPRESSION_TAG_MAP, CoinInfo, HDKey, Keypath, MultiKey, Output,
                            PathComponent)
from urtypes import Bytes
from urtypes.crypto import PSBT as UrPsbt

sys.path.insert(0, str(Path(__file__).resolve().parent / "vendor"))
from ur.ur import UR                    # noqa: E402
from ur.ur_decoder import URDecoder     # noqa: E402
from ur.ur_encoder import UREncoder     # noqa: E402

NET = NETWORKS["test"]


# ---------------------------------------------------------------- keys
class Seed:
    """A seed the wallet app (or a test's other cosigner) holds itself."""

    def __init__(self, mnemonic, passphrase=""):
        self.mnemonic = mnemonic
        self.root = bip32.HDKey.from_seed(bip39.mnemonic_to_seed(mnemonic, passphrase))
        self.fingerprint = self.root.my_fingerprint.hex()

    def key(self, path):
        """Descriptor key at `path`, e.g. "48h/1h/0h/2h": "[fingerprint/path]tpub...". """
        return "[%s/%s]%s" % (self.fingerprint, path, self.root.derive("m/" + path).to_public().to_base58(NET["xpub"]))

    def sign(self, psbt):
        return psbt.sign_with(self.root)


def parse_key_export(text):
    """The signer's Connect wallet QR: "[fingerprint/84h/1h/0h]vpub...", or
    "[fingerprint/48h/1h/0h/2h]Vpub..." for a multisig wallet.
    Returns (fingerprint, path, descriptor key with a plain tpub)."""
    m = re.fullmatch(r"\[([0-9a-f]{8})/([0-9h'/]+)\]([a-zA-Z0-9]+)", text)
    assert m, "not a key export: %r" % text
    fingerprint, path, xpub = m.groups()
    tpub = bip32.HDKey.from_string(xpub).to_base58(NET["xpub"])   # vpub and Vpub are SLIP-132 spellings of the same key
    return fingerprint, path, "[%s/%s]%s" % (fingerprint, path, tpub)


def seedqr(mnemonic, compact=False):
    """A mnemonic as SeedQR content: word numbers as digits, or the raw entropy."""
    if compact:
        return bip39.mnemonic_to_bytes(mnemonic)
    return "".join("%04d" % bip39.WORDLIST.index(w) for w in mnemonic.split())


# ---------------------------------------------------------------- UR
def ur_parts(ur_type, cbor, fragment_len=100, skip=0):
    """An animated UR as an endless stream of QR contents, upper case like
    Sparrow shows them. `skip` joins the animation late, so that the scanner
    has to make do with fountain-coded parts."""
    enc = UREncoder(UR(ur_type, cbor), fragment_len)
    for _ in range(skip):
        enc.next_part()
    while True:
        yield enc.next_part().upper()


def ur_single(ur_type, cbor):
    return UREncoder(UR(ur_type, cbor), 10 ** 6).next_part().upper()


def psbt_parts(psbt, **kw):
    return ur_parts("crypto-psbt", UrPsbt(psbt.serialize()).to_cbor(), **kw)


def read_ur(next_frame, max_frames=300):
    """Collect an animated UR from the signer's screen. Returns (type, CBOR)."""
    dec = URDecoder()
    for _ in range(max_frames):
        dec.receive_part(next_frame().lower())
        if dec.is_complete():
            assert dec.is_success(), dec.result
            return dec.result.type, dec.result.cbor
    raise AssertionError("the signer's animated QR never completed")


def read_psbt(next_frame):
    ur_type, cbor = read_ur(next_frame)
    assert ur_type == "crypto-psbt", ur_type
    return PSBT.parse(UrPsbt.from_cbor(cbor).data)


# ---------------------------------------------------------------- wallet
class Wallet:
    def __init__(self, descriptor):
        self.desc = Descriptor.from_string(descriptor)
        self._funding = itertools.count()

    @classmethod
    def singlesig(cls, key):
        return cls("wpkh(%s/<0;1>/*)" % key)

    @classmethod
    def multisig(cls, m, keys):
        return cls("wsh(sortedmulti(%d,%s))" % (m, ",".join(k + "/<0;1>/*" for k in keys)))

    # What the app shows as "output descriptor", in the spellings wallets use.
    def descriptor(self, chains="<0;1>", checksum=True):
        text = str(self.desc)
        assert "/<0;1>/*" in text
        if chains == "0":            # receive only, as e.g. Specter and old Sparrow write it
            text = text.replace("/<0;1>/*", "/0/*")
        elif chains == "{0,1}":
            text = text.replace("/<0;1>/*", "/{0,1}/*")
        return add_checksum(text) if checksum else text

    def descriptor_ur(self):
        """crypto-output (BCR-2020-010) for a sortedmulti wallet, as Sparrow shows it.
        Returns (UR type, CBOR) for ur_single() or ur_parts()."""
        assert self.desc.is_basic_multisig and self.desc.is_sorted
        keys = []
        for k in self.desc.keys:
            path = [PathComponent(i & 0x7FFFFFFF, i >= 0x80000000) for i in k.origin.derivation]
            keys.append(HDKey({
                "key": k.key.key.sec(),
                "chain_code": k.key.chain_code,
                "use_info": CoinInfo(None, 1),   # testnet
                "origin": Keypath(path, k.origin.fingerprint, len(path)),
                "children": Keypath([PathComponent(0, False), PathComponent(None, False)], None, None),
                "parent_fingerprint": k.key.fingerprint,
            }))
        threshold = self.desc.miniscript.args[0].num
        out = Output([SCRIPT_EXPRESSION_TAG_MAP[401], SCRIPT_EXPRESSION_TAG_MAP[407]], MultiKey(threshold, [], keys))
        return "crypto-output", out.to_cbor()

    def descriptor_bytes_ur(self):
        """The descriptor text in a bytes UR."""
        return "bytes", Bytes(self.descriptor().encode()).to_cbor()

    def fingerprints(self):
        return [k.origin.fingerprint.hex() for k in self.desc.keys]

    def address(self, index, change=False):
        return self.desc.derive(index, branch_index=int(change)).address(NET)

    def fund(self, index, amount, change=False):
        """Pretend somebody paid `amount` to one of our addresses. Returns the coin."""
        n = next(self._funding)
        src = TransactionInput(hashlib.sha256(b"e2e funding %d" % n).digest(), 0)
        prev = Transaction(version=2, vin=[src],
                           vout=[TransactionOutput(amount, self.desc.derive(index, branch_index=int(change)).script_pubkey())])
        return {"tx": prev, "vout": 0, "index": index, "change": change}

    def _describe(self, scope, index, change):
        """Fill in what a signer needs to recognise an input or output as this wallet's."""
        d = self.desc.derive(index, branch_index=int(change))
        for k in d.keys:
            scope.bip32_derivations[k.get_public_key()] = DerivationPath(k.origin.fingerprint, k.origin.derivation)
        if d.witness_script() is not None:
            scope.witness_script = d.witness_script()

    def create_psbt(self, coins, payments, fee, change_index=0):
        """Spend `coins` to `payments` [(address, amount)], the rest minus `fee` back to change."""
        total = sum(c["tx"].vout[c["vout"]].value for c in coins)
        change = total - fee - sum(a for _, a in payments)
        assert change >= 0
        vin = [TransactionInput(c["tx"].txid(), c["vout"], sequence=0xFFFFFFFD) for c in coins]
        vout = [TransactionOutput(amount, script.address_to_scriptpubkey(addr)) for addr, amount in payments]
        if change:
            vout.append(TransactionOutput(change, self.desc.derive(change_index, branch_index=1).script_pubkey()))
        psbt = PSBT(Transaction(version=2, vin=vin, vout=vout))
        for scope, c in zip(psbt.inputs, coins):
            scope.non_witness_utxo = c["tx"]
            scope.witness_utxo = c["tx"].vout[c["vout"]]
            self._describe(scope, c["index"], c["change"])
        if change:
            self._describe(psbt.outputs[-1], change_index, True)
        return psbt

    # -------------------------------------------------------- judging what comes back
    def combine(self, psbt, signed):
        """Take the signatures from what the signer showed into the app's own
        PSBT. The signer may hand back a trimmed PSBT (signatures only), but
        it must be for the very transaction it was shown."""
        assert signed.tx.serialize() == psbt.tx.serialize(), "the signer signed a different transaction"
        for mine, theirs in zip(psbt.inputs, signed.inputs):
            mine.partial_sigs.update(theirs.partial_sigs)
        return psbt

    def signers(self, psbt):
        """Fingerprints whose signature is on every input, each one checked
        against the transaction: a signature that doesn't verify fails the test."""
        per_input = []
        for i, scope in enumerate(psbt.inputs):
            sighash = psbt.sighash(i)
            who = set()
            for pub, sig in scope.partial_sigs.items():
                assert sig[-1] == 1, "not SIGHASH_ALL"
                assert pub.verify(ec.Signature.parse(sig[:-1]), sighash), "invalid signature on input %d" % i
                who.add(scope.bip32_derivations[pub].fingerprint.hex())
            per_input.append(who)
        return set.intersection(*per_input)

    def finalize(self, psbt):
        """The transaction, ready to broadcast. Fails if signatures are missing."""
        tx = finalizer.finalize_psbt(psbt)
        assert tx is not None, "not enough signatures to finalize"
        for vin in tx.vin:
            assert vin.witness and len(vin.witness.items) >= 2
        return tx

