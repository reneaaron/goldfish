"""Multisig, end to end: the signer is one cosigner of a wsh(sortedmulti)
wallet that the wallet app sets up, the other cosigners live in the app. The
signer learns the wallet from its descriptor QR, and from then on checks
transactions and addresses against it.

All seeds here are public test seeds. Never real funds.
"""
import pytest
from embit.networks import NETWORKS

from flows import (set_key_type, set_network, address_on_screen, connect_wallet, fingerprint_on_screen, load_by_words,
                   review_and_sign, verify_address)
from hostwallet import Seed, Wallet, parse_key_export, psbt_parts, ur_parts, ur_single

MNEMONIC = "crew marble private differ race truly blush basket crater affair prepare unique"
SIGNER = Seed(MNEMONIC)   # the test typed these words in, so it can know the signer's keys
COSIGNERS = [Seed(m) for m in (
    "zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo wrong",
    "legal winner thank year wave sausage worth useful legal winner thank yellow",
    "letter advice cage absurd amount doctor acoustic avoid letter advice cage above",
    "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about",
)]
BIP48 = "48h/1h/0h/2h"   # testnet, account 0, native segwit multisig
STRANGER = "tb1q6rz28mcfaxtmd6v789l9rrlrusdprr9pqcpvkl"


def two_of_three(signer_key=None):
    return Wallet.multisig(2, [signer_key or SIGNER.key(BIP48)] + [c.key(BIP48) for c in COSIGNERS[:2]])


def loaded(dev):
    load_by_words(dev, MNEMONIC)


def scan_descriptor(dev, shown, wallet):
    """Show the descriptor QR (a string, or UR parts) to the open scanner and
    check the Multisig wallet screen against the wallet app, as the user must."""
    if isinstance(shown, str):
        dev.scan(shown)
    else:
        dev.scan_animated(shown)
    assert dev.screen == "MULTISIG", dev.text()
    text = dev.text()
    n = len(wallet.desc.keys)
    assert "%d of %d multisig" % (wallet.desc.miniscript.args[0].num, n) in text
    for fingerprint in wallet.fingerprints():
        assert "%s %s" % (fingerprint[:4], fingerprint[4:]) in text
    assert address_on_screen(dev) == wallet.address(0)


def test_linked_key_to_broadcastable_transaction(dev):
    """Set up a 2-of-3 around the multisig key the signer exports, receive to
    it, and spend from it with the signer and one cosigner."""
    loaded(dev)
    fingerprint, path, key = connect_wallet(dev, multisig=True)   # all the wallet app ever gets from the signer
    assert (fingerprint, path, key) == (SIGNER.fingerprint, BIP48, SIGNER.key(BIP48))
    wallet = two_of_three(key)

    # A multisig transaction is refused until the signer knows the wallet.
    coins = [wallet.fund(0, 500_000), wallet.fund(1, 250_000)]
    psbt = wallet.create_psbt(coins, [(STRANGER, 600_000)], fee=1_200, change_index=0)
    dev.tap("Scan QR")
    dev.scan_animated(psbt_parts(psbt))
    assert dev.screen == "PSBT_ERROR" and "needs its wallet's descriptor" in dev.text()

    # The descriptor: checked on screen, and dropped if the user backs out.
    dev.tap("Scan it")
    scan_descriptor(dev, wallet.descriptor(), wallet)
    assert "{FINGERPRINT}  This seed\n%s %s" % (fingerprint[:4], fingerprint[4:]) in dev.text()
    dev.tap("Cancel")
    dev.scan_animated(psbt_parts(psbt))
    assert dev.screen == "PSBT_ERROR", "an unconfirmed descriptor was used"
    dev.tap("Scan it")
    scan_descriptor(dev, wallet.descriptor(), wallet)
    dev.tap("Use wallet")
    assert dev.screen == "SCAN"

    # Now the same transaction is reviewed against the wallet, and signed.
    dev.scan_animated(psbt_parts(psbt))
    assert dev.screen == "PSBT_OVERVIEW", dev.text()
    signed, (overview, pay, change, confirm) = review_and_sign(dev)
    assert "2 OF 3" in overview and all(a in overview for a in ("₿600,000", "₿1,200", "₿148,800"))
    assert "= " + STRANGER in pay
    assert "YOUR CHANGE" in change and "= " + wallet.address(0, change=True) in change
    assert "₿601,200" in confirm

    # One signature isn't enough; with a cosigner's the app can broadcast.
    wallet.combine(psbt, signed)
    assert wallet.signers(psbt) == {SIGNER.fingerprint}
    with pytest.raises(AssertionError, match="not enough signatures"):
        wallet.finalize(psbt)
    COSIGNERS[1].sign(psbt)
    assert wallet.signers(psbt) == {SIGNER.fingerprint, COSIGNERS[1].fingerprint}
    wallet.finalize(psbt)

    # The wallet stays known for receiving: its addresses verify, others don't.
    assert "2 OF 3" in dev.text()
    result = verify_address(dev, wallet.address(5))
    assert "Address verified" in result and "RECEIVE #5" in result
    result = verify_address(dev, two_of_three(SIGNER.key("84h/1h/0h")).address(5))   # same cosigners, another key of ours
    assert "Not from this seed" in result


def test_connect_wallet_shows_either_key(dev):
    """Single-sig and multisig keys, on both networks, as embit derives them
    from the same words; switching back and forth doesn't mix them up. The
    short key under the QR and the details behind it match the QR."""
    loaded(dev)
    dev.tap("Wallet options")
    seen = {}
    for kind, network in (("Single-sig", "Mainnet"), ("Multisig", "Mainnet"), ("Multisig", "Testnet"), ("Single-sig", "Testnet")):
        set_network(dev, network)
        dev.tap("Connect wallet")
        set_key_type(dev, kind == "Multisig")
        shown = dev.read_qr()
        seen[kind, network] = shown
        key = shown.split("]")[1]
        assert "%s\u2026%s" % (key[:8], key[-4:]) in dev.texts()
        dev.tap("%s\u2026%s" % (key[:8], key[-4:]))   # the details: fingerprint, path, all of the key
        text = dev.text()
        assert "%s %s" % (SIGNER.fingerprint[:4], SIGNER.fingerprint[4:]) in text
        assert "".join(t for t in dev.texts() if len(t) <= 4 and t.isalnum()).endswith(key[-20:])
        dev.back()   # to the QR
        dev.back()   # to Wallet options
    for (kind, network), shown in seen.items():
        net = NETWORKS["main" if network == "Mainnet" else "test"]
        path = ("84h/%s/0h" if kind == "Single-sig" else "48h/%s/0h/2h") % ("0h" if network == "Mainnet" else "1h")
        xpub = SIGNER.root.derive("m/" + path).to_public().to_base58(net["zpub" if kind == "Single-sig" else "Zpub"])
        assert shown == "[%s/%s]%s" % (SIGNER.fingerprint, path, xpub)
    assert seen["Multisig", "Mainnet"].split("]")[1].startswith("Zpub")
    assert seen["Multisig", "Testnet"].split("]")[1].startswith("Vpub")


SPELLINGS = {
    "text": lambda w: w.descriptor(),
    "text, no checksum": lambda w: w.descriptor(checksum=False),
    "text, {0,1}": lambda w: w.descriptor("{0,1}"),
    "text, receive only": lambda w: w.descriptor("0"),
    "crypto-output": lambda w: ur_single(*w.descriptor_ur()),
    "crypto-output, animated": lambda w: ur_parts(*w.descriptor_ur(), fragment_len=80),
    "bytes, animated": lambda w: ur_parts(*w.descriptor_bytes_ur(), fragment_len=120, skip=1),
}


@pytest.mark.parametrize("spelling", SPELLINGS)
def test_descriptor_spellings(dev, spelling):
    """Every way a wallet app writes the descriptor QR gives the same wallet,
    with the signer's key at the usual BIP-48 path."""
    wallet = two_of_three()
    loaded(dev)
    dev.tap("Scan QR")
    scan_descriptor(dev, SPELLINGS[spelling](wallet), wallet)
    dev.tap("Use wallet")
    dev.back()
    result = verify_address(dev, wallet.address(11, change=True))
    assert "Address verified" in result and "CHANGE #11" in result


def test_three_of_five_after_a_cosigner(dev):
    """A bigger wallet, shown as an animated crypto-output. A cosigner has
    signed already: the signer adds its signature and keeps theirs."""
    wallet = Wallet.multisig(3, [c.key(BIP48) for c in COSIGNERS[:2]] + [SIGNER.key(BIP48)] + [c.key(BIP48) for c in COSIGNERS[2:]])
    psbt = wallet.create_psbt([wallet.fund(3, 1_000_000)], [(STRANGER, 999_000)], fee=1_000)   # no change
    COSIGNERS[3].sign(psbt)

    loaded(dev)
    dev.tap("Scan QR")
    scan_descriptor(dev, ur_parts(*wallet.descriptor_ur(), fragment_len=100), wallet)
    dev.tap("Use wallet")
    dev.scan_animated(psbt_parts(psbt, skip=7))
    assert dev.screen == "PSBT_OVERVIEW", dev.text()
    signed, seen = review_and_sign(dev)
    assert "3 OF 5" in seen[0] and "None" in seen[0]   # no change
    assert len(seen) == 3

    assert len(signed.inputs[0].partial_sigs) == 2   # the signer's QR carries both
    psbt.inputs[0].partial_sigs.clear()
    wallet.combine(psbt, signed)
    assert wallet.signers(psbt) == {SIGNER.fingerprint, COSIGNERS[3].fingerprint}
    COSIGNERS[0].sign(psbt)
    wallet.finalize(psbt)


def test_wallets_we_are_not_in_are_refused(dev):
    loaded(dev)
    dev.tap("Scan QR")
    # Three other people's keys.
    dev.scan(Wallet.multisig(2, [c.key(BIP48) for c in COSIGNERS[:3]]).descriptor())
    assert dev.screen == "SCAN" and "Wrong descriptor" in dev.text()
    # Our fingerprint on somebody else's key: the fingerprint proves nothing.
    forged = COSIGNERS[2].key(BIP48).replace(COSIGNERS[2].fingerprint, SIGNER.fingerprint)
    dev.scan(Wallet.multisig(2, [forged] + [c.key(BIP48) for c in COSIGNERS[:2]]).descriptor())
    assert dev.screen == "SCAN" and "Wrong descriptor" in dev.text()
    # Not sortedmulti.
    dev.scan(two_of_three().descriptor().split("#")[0].replace("sortedmulti", "multi"))
    assert dev.screen == "SCAN" and "Not supported yet" in dev.text()
    # A cosigner key without [fingerprint/path], as a wallet app has a bare xpub.
    bare = COSIGNERS[0].key(BIP48).split("]")[1]
    dev.scan(Wallet.multisig(2, [SIGNER.key(BIP48), bare, COSIGNERS[1].key(BIP48)]).descriptor())
    assert dev.screen == "SCAN" and "Key fingerprint missing" in dev.text()
    # Ours with fingerprint 00000000: the signer can't find itself in it.
    zeroed = SIGNER.key(BIP48).replace(SIGNER.fingerprint, "00000000")
    dev.scan(Wallet.multisig(2, [zeroed] + [c.key(BIP48) for c in COSIGNERS[:2]]).descriptor())
    assert dev.screen == "SCAN" and "Key fingerprint missing" in dev.text()
    # The same as a crypto-output UR: a key's origin without its fingerprint,
    # and one with a fingerprint but no path (nothing to find our key at).
    dev.scan(ur_single(*two_of_three().descriptor_ur(no_fingerprint=(1,))))
    assert dev.screen == "SCAN" and "Key fingerprint missing" in dev.text(), dev.text()
    dev.scan(ur_single(*two_of_three().descriptor_ur(no_path=(1,))))
    assert dev.screen == "SCAN" and "Not supported yet" in dev.text(), dev.text()
    # A typo: the checksum catches it.
    good = two_of_three().descriptor()
    dev.scan(good.replace("sortedmulti(2", "sortedmulti(1"))
    assert dev.screen == "SCAN" and "Damaged descriptor" in dev.text()


def test_change_to_another_multisig_blocks_signing(dev):
    """A compromised wallet app swaps the cosigners in the change output for
    its own keys, next to ours. Only the descriptor lets the signer notice."""
    wallet = two_of_three()
    evil = Wallet.multisig(2, [SIGNER.key(BIP48)] + [c.key(BIP48) for c in COSIGNERS[2:]])
    psbt = wallet.create_psbt([wallet.fund(0, 300_000)], [(STRANGER, 10_000)], fee=500, change_index=2)
    theirs = evil.create_psbt([evil.fund(0, 300_000)], [(STRANGER, 10_000)], fee=500, change_index=2)
    psbt.outputs[-1] = theirs.outputs[-1]   # script, witness script and key paths, all consistent

    loaded(dev)
    dev.tap("Scan QR")
    dev.scan(wallet.descriptor())
    dev.tap("Use wallet")
    dev.scan_animated(psbt_parts(psbt))
    assert dev.screen == "PSBT_ERROR", dev.text()
    assert "Don't sign this" in dev.text()


def test_descriptor_goes_with_the_seed(dev):
    """Stateless: after Forget seed the wallet is unknown again, also to the same seed."""
    wallet = two_of_three()
    psbt = wallet.create_psbt([wallet.fund(0, 300_000)], [(STRANGER, 10_000)], fee=500)
    loaded(dev)
    dev.tap("Scan QR")
    dev.scan(wallet.descriptor())
    dev.tap("Use wallet")
    dev.scan_animated(psbt_parts(psbt))
    assert dev.screen == "PSBT_OVERVIEW"
    dev.back()
    dev.tap("Wallet options")
    dev.scroll_down()
    dev.tap("Forget seed")
    dev.tap("Forget")
    assert dev.screen == "HOME"

    loaded(dev)
    assert "2 OF 3" not in dev.text()
    dev.tap("Scan QR")
    dev.scan_animated(psbt_parts(psbt))
    assert dev.screen == "PSBT_ERROR" and "needs its wallet's descriptor" in dev.text()


def test_leave_multisig_keeps_the_seed(dev):
    """While a multisig wallet is in use, Connect wallet shows its kind of key
    only. Leave multisig, in Wallet options, asks first; then the wallet is
    unknown again, with the seed still loaded and its single-key key back."""
    wallet = two_of_three()
    psbt = wallet.create_psbt([wallet.fund(0, 300_000)], [(STRANGER, 10_000)], fee=500)
    loaded(dev)
    dev.tap("Wallet options")
    assert "Leave multisig" not in dev.texts()   # nothing to leave yet
    dev.back()
    dev.tap("Scan QR")
    dev.scan(wallet.descriptor())
    dev.tap("Use wallet")
    dev.back()
    assert dev.screen == "WALLET" and "2 OF 3" in dev.text()

    # Connect wallet: the multisig key, though the single-key one was showing before, and no way to switch.
    dev.tap("Wallet options")
    set_network(dev, "Testnet")
    dev.tap("Connect wallet")
    assert "2 of 3 multisig wallet in use" in dev.texts()
    assert not any(t.startswith("For a ") for t in dev.texts()), dev.text()
    assert parse_key_export(dev.read_qr()) == (SIGNER.fingerprint, BIP48, SIGNER.key(BIP48))
    dev.back()

    # Cancel leaves everything as it is.
    dev.tap("Leave multisig")
    assert "Leave the multisig wallet?" in dev.text()
    dev.tap("Cancel")
    assert dev.screen == "WALLET_OPTIONS"
    dev.back()
    assert "2 OF 3" in dev.text()

    dev.tap("Wallet options")
    dev.tap("Leave multisig")
    dev.tap("Leave")
    assert dev.screen == "WALLET" and "2 OF 3" not in dev.text()
    assert fingerprint_on_screen(dev) == SIGNER.fingerprint   # the same seed

    dev.tap("Wallet options")
    assert "Leave multisig" not in dev.texts()
    dev.tap("Connect wallet")
    assert "For a multisig wallet  {RIGHT}" in dev.texts()
    assert parse_key_export(dev.read_qr()) == (SIGNER.fingerprint, "84h/1h/0h", SIGNER.key("84h/1h/0h"))
    dev.back()
    dev.back()
    dev.tap("Scan QR")
    dev.scan_animated(psbt_parts(psbt))
    assert dev.screen == "PSBT_ERROR" and "needs its wallet's descriptor" in dev.text()


def test_big_transaction_through_a_bad_camera(dev):
    """15 inputs, about 7 KB. The camera reads
    each frame twice and misses 75% of the animation: the fountain-coded
    parts still complete it (they used to fill up the decoder and stall)."""
    import random
    rng = random.Random(1)
    wallet = two_of_three()
    psbt = wallet.create_psbt([wallet.fund(i, 100_000) for i in range(15)], [(STRANGER, 1_000_000)], fee=3_000)
    parts = psbt_parts(psbt)

    def bad_camera():
        while True:
            part = next(parts)
            if rng.random() < 0.25:
                yield part
                yield part

    loaded(dev)
    dev.tap("Scan QR")
    dev.scan(wallet.descriptor())
    dev.tap("Use wallet")
    dev.scan_animated(bad_camera())
    assert dev.screen == "PSBT_OVERVIEW", dev.text()
    signed, _ = review_and_sign(dev)
    wallet.combine(psbt, signed)
    assert wallet.signers(psbt) == {SIGNER.fingerprint}


def test_too_big_transaction_says_so(dev):
    """Past 16 KB the signer can't take a transaction, and tells the user
    how to split it instead of showing nothing."""
    wallet = two_of_three()
    psbt = wallet.create_psbt([wallet.fund(i, 100_000) for i in range(35)], [(STRANGER, 1_000_000)], fee=3_000)
    assert len(psbt.serialize()) > 16384
    loaded(dev)
    dev.tap("Scan QR")
    dev.scan(wallet.descriptor())
    dev.tap("Use wallet")
    parts = psbt_parts(psbt, fragment_len=200)   # 86 parts: the size, not the part count, is too big
    for _ in range(3):
        dev.scan(next(parts), ms=200)
    assert dev.screen == "PSBT_ERROR", dev.text()
    assert "too big" in dev.text() and "fewer coins" in dev.text()
    dev.tap("Back to wallet")
    assert dev.screen == "WALLET"
