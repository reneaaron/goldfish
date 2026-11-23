"""Single-sig, end to end: a seed is made or loaded on the signer, the wallet
app links it from the Connect wallet QR, and everything after that goes
through QR codes in both directions, like with Sparrow.

All seeds here are public test seeds. Never real funds.
"""
import pytest
from embit import bip39

from flows import (connect_wallet, fingerprint_on_screen, load_by_qr, load_by_words,
                   review_and_sign, verify_address)
from hostwallet import NET, Seed, Wallet, psbt_parts, seedqr

MNEMONIC = "crew marble private differ race truly blush basket crater affair prepare unique"
STRANGER = Seed("abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about")


def stranger_address(index=0):
    return Wallet.singlesig(STRANGER.key("84h/1h/0h")).address(index)


def linked_wallet(dev, mnemonic=MNEMONIC):
    """Load the seed by typing it, then link a wallet app to the signer."""
    load_by_words(dev, mnemonic)
    fingerprint, path, key = connect_wallet(dev)
    assert (fingerprint, path) == (Seed(mnemonic).fingerprint, "84h/1h/0h")
    return Wallet.singlesig(key)


def test_new_seed_to_signed_transaction(dev):
    """The whole life of a wallet: roll dice for a new seed, write down the
    words, load them, link the wallet app, check a receive address, sign."""
    # New seed from 50 dice rolls. The signer shows the words to write down.
    dev.tap("Create new seed")
    dev.tap("Dice rolls")
    for roll in "63124551326645213355162443316625514432216653411526":
        dev.tap_icon("DICE_" + roll)
    assert "Keep it offline" in dev.text()
    dev.tap("I understand")
    words = []
    while dev.screen == "BACKUP_WORDS":
        texts = dev.texts()
        words += [texts[i + 1] for i, t in enumerate(texts) if t.isdigit()]
        dev.tap("Next" if "Next" in texts else "Written down")
    mnemonic = " ".join(words)
    assert len(words) == 12 and bip39.mnemonic_is_valid(mnemonic)
    dev.tap("Skip")   # no QR card
    assert "Seed ready" in dev.text()
    assert fingerprint_on_screen(dev) == Seed(mnemonic).fingerprint
    dev.tap("Done")
    assert dev.screen == "HOME"   # a new seed is never loaded by itself

    # Load it from the words just written down; link the wallet app.
    wallet = linked_wallet(dev, mnemonic)
    assert fingerprint_on_screen(dev) == Seed(mnemonic).fingerprint

    # Receive: the address the app shows is checked on the signer.
    result = verify_address(dev, "bitcoin:%s?amount=0.0015" % wallet.address(7))
    assert "Address verified" in result and "RECEIVE #7" in result and "TESTNET" in result
    assert "₿150,000" in result

    # Spend: two coins in, a payment and change out.
    coins = [wallet.fund(7, 150_000), wallet.fund(2, 40_000, change=True)]
    psbt = wallet.create_psbt(coins, [(stranger_address(), 120_000)], fee=830, change_index=3)
    dev.tap("Scan QR")
    dev.scan_animated(psbt_parts(psbt))
    assert dev.screen == "PSBT_OVERVIEW", dev.text()
    signed, seen = review_and_sign(dev)
    overview, pay, change, confirm = seen
    assert "YOU'RE SENDING" in overview and "TESTNET" in overview
    assert all(amount in overview for amount in ("₿120,000", "₿830", "₿69,170"))
    assert "₿120,000" in pay and "YOUR CHANGE" not in pay and "= " + stranger_address() in pay
    assert "YOUR CHANGE" in change and "₿69,170" in change and "= " + wallet.address(3, change=True) in change
    assert "₿120,830" in confirm   # total with the fee

    # The wallet app takes the signed transaction back and can broadcast it.
    wallet.combine(psbt, signed)
    assert wallet.signers(psbt) == {Seed(mnemonic).fingerprint}
    wallet.finalize(psbt)
    assert dev.screen == "WALLET"


@pytest.mark.parametrize("compact", [False, True], ids=["standard", "compact"])
def test_seedqr_loads_the_same_wallet(dev, compact):
    """A SeedQR, Standard or Compact, gives the wallet the typed words give."""
    load_by_qr(dev, MNEMONIC, compact)
    assert fingerprint_on_screen(dev) == Seed(MNEMONIC).fingerprint
    _, _, key = connect_wallet(dev)
    assert key == Seed(MNEMONIC).key("84h/1h/0h")


def test_seedqr_card_is_copied_and_checked(dev):
    """Back up as QR: the SeedQR on the signer's screen, copied square by
    square onto a card, passes the card check. A wrong card doesn't."""
    load_by_words(dev, MNEMONIC)
    dev.tap("Wallet options")
    dev.tap("Back up as QR")
    dev.tap("I understand")
    card = dev.read_qr(raw=True)               # what ends up on the card
    assert card == seedqr(MNEMONIC, compact=True)
    dev.tap("Start copying")
    parts = 0
    while "Check card" not in dev.texts():
        dev.tap("Next")
        parts += 1
    assert "Part 16 of 16" in dev.text() and parts == 15   # 21x21 in 5x5 parts
    dev.tap("Check card")
    assert dev.screen == "BACKUP_SCAN", dev.text()
    dev.scan(seedqr(STRANGER.mnemonic, compact=True))
    assert dev.screen == "BACKUP_SCAN" and "Backup checked" not in dev.text()
    dev.scan(card)
    assert "Backup checked" in dev.text()
    assert fingerprint_on_screen(dev) == Seed(MNEMONIC).fingerprint


def test_addresses_of_other_wallets_are_refused(dev):
    wallet = linked_wallet(dev)
    result = verify_address(dev, wallet.address(42, change=True))
    assert "Address verified" in result and "CHANGE #42" in result
    result = verify_address(dev, stranger_address(3))
    assert "Not from this seed" in result


def test_transaction_of_another_seed_is_refused(dev):
    load_by_words(dev, MNEMONIC)
    theirs = Wallet.singlesig(STRANGER.key("84h/1h/0h"))
    psbt = theirs.create_psbt([theirs.fund(0, 50_000)], [(theirs.address(1), 20_000)], fee=300)
    dev.tap("Scan QR")
    dev.scan_animated(psbt_parts(psbt))
    assert dev.screen == "PSBT_ERROR"
    assert "Can't sign this" in dev.text()
    dev.tap("Back to wallet")
    assert dev.screen == "WALLET"


def test_fake_change_blocks_signing(dev):
    """Change that claims to be ours but pays another script: the classic
    attack by a compromised wallet app. The signer must refuse."""
    wallet = linked_wallet(dev)
    psbt = wallet.create_psbt([wallet.fund(0, 90_000)], [(stranger_address(), 10_000)], fee=500, change_index=5)
    theirs = Wallet.singlesig(STRANGER.key("84h/1h/0h"))
    psbt.outputs[-1].script_pubkey = theirs.desc.derive(5, branch_index=1).script_pubkey()
    assert psbt.tx.vout[-1].script_pubkey.address(NET) == theirs.address(5, change=True)
    dev.tap("Scan QR")
    dev.scan_animated(psbt_parts(psbt))
    assert dev.screen == "PSBT_ERROR", dev.text()
    assert "Don't sign this" in dev.text()


def test_psbt_joined_mid_animation(dev):
    """The camera rarely catches the first frame: the scanner has to finish
    from fountain-coded parts alone, and so does the wallet app reading the
    signed one back. Also a single base64 frame."""
    wallet = linked_wallet(dev)
    psbt = wallet.create_psbt([wallet.fund(1, 60_000)], [(stranger_address(), 25_000)], fee=400)
    dev.tap("Scan QR")
    dev.scan_animated(psbt_parts(psbt, fragment_len=60, skip=40))
    assert dev.screen == "PSBT_OVERVIEW", dev.text()
    signed, _ = review_and_sign(dev, skip_frames=20)   # past every pure fragment
    wallet.combine(psbt, signed)
    assert wallet.signers(psbt) == {Seed(MNEMONIC).fingerprint}

    dev.tap("Scan QR")
    dev.scan(psbt.to_base64())
    assert dev.screen == "PSBT_OVERVIEW", dev.text()


def test_24_words_by_hand(dev):
    """Enter words starts at 12; "24 words" on the first word switches for a
    24-word backup, and the seed loads like any other."""
    words = "zoo " * 23 + "vote"
    load_by_words(dev, words)
    assert fingerprint_on_screen(dev) == Seed(words).fingerprint


def test_child_seed_from_the_keypad(dev):
    """Child seed: type a number on the keypad and confirm with its OK key."""
    load_by_words(dev, MNEMONIC)
    dev.tap("Wallet options")
    dev.tap("Child seed")
    assert dev.screen == "CHILD_INDEX", dev.text()
    dev.tap("3")
    dev.tap("OK")
    assert dev.screen == "BACKUP_WARNING", dev.text()


def test_passphrase_gives_its_own_wallet(dev):
    """A BIP-39 passphrase typed on the signer gives the wallet any other
    BIP-39 wallet derives from the same words and passphrase, signs for it,
    and leaving it empty goes back to the seed alone."""
    load_by_words(dev, MNEMONIC)
    dev.tap("Wallet options")
    dev.tap("Passphrase")
    dev.key("ABC")
    for letter in "TREZOR":
        dev.key(letter)
    dev.key("OK")
    assert dev.screen == "WALLET", dev.text()
    assert "PASSPHRASE" in dev.text()
    with_pass = Seed(MNEMONIC, "TREZOR")
    assert with_pass.fingerprint != Seed(MNEMONIC).fingerprint
    assert fingerprint_on_screen(dev) == with_pass.fingerprint

    fingerprint, path, key = connect_wallet(dev)
    assert (fingerprint, path) == (with_pass.fingerprint, "84h/1h/0h")
    assert key == with_pass.key("84h/1h/0h")
    wallet = Wallet.singlesig(key)
    psbt = wallet.create_psbt([wallet.fund(0, 50_000)], [(stranger_address(), 40_000)], fee=500, change_index=0)
    dev.tap("Scan QR")
    dev.scan(psbt.to_base64())
    assert dev.screen == "PSBT_OVERVIEW", dev.text()
    signed, _ = review_and_sign(dev)
    wallet.combine(psbt, signed)
    assert wallet.signers(psbt) == {with_pass.fingerprint}

    dev.tap("Wallet options")
    dev.tap("Passphrase")
    dev.key("OK")                     # empty: back to the seed alone
    assert dev.screen == "WALLET", dev.text()
    assert fingerprint_on_screen(dev) == Seed(MNEMONIC).fingerprint
