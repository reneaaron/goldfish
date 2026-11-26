"""Single-sig, end to end: a seed is made or loaded on the signer, the wallet
app links it from the Connect wallet QR, and everything after that goes
through QR codes in both directions, like with Sparrow.

All seeds here are public test seeds. Never real funds.
"""
import pytest
from embit import bip39

from flows import (address_on_screen, connect_wallet, fingerprint_on_screen, load_by_qr, load_by_words,
                   review_and_sign, verify_address)
from hostwallet import NET, Seed, Wallet, psbt_parts, read_psbt, seedqr

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
    texts = dev.texts()   # all the words on one list, scrolled or not
    words = [texts[i + 1] for i, t in enumerate(texts) if t.isdigit()]
    dev.tap("Written down")
    assert dev.screen == "BACKUP_WORDS"   # not before the list was scrolled to its end
    for _ in range(3):
        dev.scroll_down()
        dev.wait(500)
    dev.tap("Written down")
    mnemonic = " ".join(words)
    assert len(words) == 12 and bip39.mnemonic_is_valid(mnemonic)
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
    assert "Can't sign this" in dev.text() and "different wallet" in dev.text()
    dev.tap("Back to wallet")
    assert dev.screen == "WALLET"


def test_key_without_fingerprint_is_explained(dev):
    """A wallet app that has the key as a bare xpub writes fingerprint
    00000000 (or no key path): refused, but the screen says why and what to do,
    instead of "different wallet"."""
    load_by_words(dev, MNEMONIC)
    bare = Wallet.singlesig(Seed(MNEMONIC).key("84h/1h/0h").replace(Seed(MNEMONIC).fingerprint, "00000000"))
    psbt = bare.create_psbt([bare.fund(0, 50_000)], [(stranger_address(), 20_000)], fee=300)
    dev.tap("Scan QR")
    dev.scan_animated(psbt_parts(psbt))
    assert dev.screen == "PSBT_ERROR", dev.text()
    assert "without its fingerprint" in dev.text() and "Connect wallet" in dev.text()
    dev.tap("Back to wallet")

    for scope in psbt.inputs:
        scope.bip32_derivations.clear()
    dev.tap("Scan QR")
    dev.scan_animated(psbt_parts(psbt))
    assert dev.screen == "PSBT_ERROR" and "without its fingerprint" in dev.text(), dev.text()


def test_too_many_outputs_says_so(dev):
    wallet = linked_wallet(dev)
    psbt = wallet.create_psbt([wallet.fund(0, 500_000)], [(stranger_address(i), 10_000) for i in range(17)], fee=500)
    dev.tap("Scan QR")
    dev.scan_animated(psbt_parts(psbt))
    assert dev.screen == "PSBT_ERROR", dev.text()
    assert "over 16 outputs" in dev.text()


def test_other_sighash_says_so(dev):
    """SIGHASH_NONE would leave the outputs unsigned: refused, and the screen
    names the sighash instead of "native SegWit only"."""
    wallet = linked_wallet(dev)
    psbt = wallet.create_psbt([wallet.fund(0, 90_000)], [(stranger_address(), 10_000)], fee=500)
    psbt.inputs[0].sighash_type = 2   # SIGHASH_NONE
    dev.tap("Scan QR")
    dev.scan_animated(psbt_parts(psbt))
    assert dev.screen == "PSBT_ERROR", dev.text()
    assert "sighash other than ALL" in dev.text()


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


def test_change_far_down_its_list_asks_first(dev):
    """Change that really is ours, but at an index no wallet app scans to: a
    compromised app could park coins there and ask a ransom for the number.
    The signer shows the path and asks; a coin from near that index is the
    one thing that makes it ordinary."""
    wallet = linked_wallet(dev)
    far = 2_000_000_000
    psbt = wallet.create_psbt([wallet.fund(0, 90_000)], [(stranger_address(), 10_000)], fee=500, change_index=far)
    dev.tap("Scan QR")
    dev.scan_animated(psbt_parts(psbt))
    assert dev.screen == "PSBT_OVERVIEW", dev.text()
    dev.tap("Check addresses")
    dev.tap("Next address")
    warning = dev.text()
    assert "YOUR CHANGE" in warning and "₿79,500" in warning
    assert "may not find these coins" in warning and "m/84h/1h/0h/1/%d" % far in warning
    assert address_on_screen(dev) == wallet.address(far, change=True)
    dev.tap("Cancel")
    assert dev.screen == "WALLET", dev.text()

    # Accepted, it is signed like any other.
    dev.tap("Scan QR")
    dev.scan_animated(psbt_parts(psbt))
    signed, seen = review_and_sign(dev)
    wallet.combine(psbt, signed)
    wallet.finalize(psbt)

    # Spending change 150, change 151 is where a wallet app is by now: no question.
    psbt = wallet.create_psbt([wallet.fund(150, 90_000, change=True)], [(stranger_address(), 10_000)], fee=500, change_index=151)
    dev.tap("Scan QR")
    dev.scan_animated(psbt_parts(psbt))
    signed, (overview, pay, change, confirm) = review_and_sign(dev)
    assert "YOUR CHANGE" in change and "Cancel" not in change and "PATH" not in change


def test_change_in_another_account_is_refused(dev):
    """Our own key, but in an account the transaction doesn't spend from:
    with the account free as well, nobody finds it again."""
    wallet = linked_wallet(dev)
    other = Wallet.singlesig(Seed(MNEMONIC).key("84h/1h/1h"))
    psbt = wallet.create_psbt([wallet.fund(0, 90_000)], [(stranger_address(), 10_000)], fee=500)
    theirs = other.create_psbt([other.fund(0, 90_000)], [(stranger_address(), 10_000)], fee=500)
    psbt.outputs[-1] = theirs.outputs[-1]
    dev.tap("Scan QR")
    dev.scan_animated(psbt_parts(psbt))
    assert dev.screen == "PSBT_ERROR", dev.text()
    assert "Can't sign this" in dev.text() and "another account" in dev.text()


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


def test_signed_qr_full_screen(dev):
    """The signed QR fills the screen from the bar's button (or a tap on the
    QR), the wallet app reads the whole transaction there, and a tap goes back."""
    wallet = linked_wallet(dev)
    psbt = wallet.create_psbt([wallet.fund(4, 80_000)], [(stranger_address(), 50_000)], fee=500)
    dev.tap("Scan QR")
    dev.scan_animated(psbt_parts(psbt))
    dev.tap("Check addresses")
    while dev.screen == "PSBT_OUTPUT":
        dev.tap("Next address" if "Next address" in dev.texts() else "Continue")
    dev.slide()
    dev.tap_icon("MAXIMIZE")
    assert dev.screenshot().convert("L").getpixel((120, 4)) == 0   # black above the QR, over the bar

    def next_frame():
        frame = dev.read_qr()
        dev.wait(300)
        return frame

    wallet.combine(psbt, read_psbt(next_frame))
    assert wallet.signers(psbt) == {Seed(MNEMONIC).fingerprint}
    dev.cmd("tap 120 160")
    assert dev.screenshot().convert("L").getpixel((120, 4)) > 200
    dev.tap("Done")
    assert dev.screen == "WALLET"


def test_24_words_by_hand(dev):
    """Enter words starts at 12; "24 words" on the first word switches for a
    24-word backup, and the seed loads like any other."""
    words = "zoo " * 23 + "vote"
    load_by_words(dev, words)
    assert fingerprint_on_screen(dev) == Seed(words).fingerprint


def test_keyboard_types_on_release(dev):
    """A letter key is typed when the finger lifts, not when it lands. While
    held, the letter is shown in a balloon above the key. Sliding to the next
    letter corrects a miss; sliding onto a key that is off, or onto Del, keeps
    the letter; sliding well above the keyboard calls it off. A finger that
    slips onto the next key as it lifts still types the key it was holding."""
    dev.tap("Enter words")
    key = lambda k: next(w for w in dev.widgets() if w.kind == "key" and w.text == k)
    mid = lambda k: ((key(k).x1 + key(k).x2) // 2, (key(k).y1 + key(k).y2) // 2)
    press = lambda k: dev.cmd("press %d %d" % mid(k))
    field = lambda: next(t for t in dev.texts() if t.endswith("_"))
    above = (0, key("q").y1 - 44, 240, key("q").y1)   # the strip over the top row
    row = dev.screenshot().crop(above)

    press("q")
    assert field() == "_"
    assert dev.screenshot().crop(above) != row   # the balloon
    press("w")
    dev.wait(200)
    dev.cmd("release")
    assert field() == "w_"
    assert dev.screenshot().crop(above) == row

    press("a")                                   # slips onto s (off after "w") while lifting
    press("s")
    dev.cmd("release")
    assert field() == "wa_"
    dev.key("Del")

    press("e")                                   # slips onto r, a live key, while lifting
    dev.wait(200)
    press("r")
    dev.cmd("release")
    assert field() == "we_"
    dev.key("Del")

    press("a")                                   # called off
    dev.cmd("press 120 %d" % (above[1] - 16))
    dev.wait(200)
    dev.cmd("release")
    assert field() == "w_"

    press("i")                                   # onto Del: the letter stays, nothing is deleted
    dev.wait(200)
    press("Del")
    dev.wait(200)
    dev.cmd("release")
    assert field() == "wi_"

    press("Del")                                 # Del itself is as before: at once, and no letter after it
    assert field() == "w_"
    press("a")
    dev.wait(200)
    dev.cmd("release")
    assert field() == "w_"


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
    # TREZOR is the passphrase of every BIP-39 test vector (Trezor's
    # vectors.json, which BIP-39 links to), the same one the unit tests use.
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


def test_passphrase_layer_keys_type_nothing(dev):
    """A layer key swaps the keys under the finger on touch-down; lifting the
    finger then types nothing, whatever key now sits where it was."""
    load_by_words(dev, MNEMONIC)
    dev.tap("Wallet options")
    dev.tap("Passphrase")
    for k in ["123", "#+=", "123", "#+=", "abc"]:
        dev.key(k)
    assert next(t for t in dev.texts() if t.endswith("_")) == "_"


def test_largest_transactions(dev):
    """What the board's heap still signs: 60 inputs, and 16 outputs (15
    payments and change)."""
    wallet = linked_wallet(dev)
    for n_in, n_out in [(60, 1), (1, 15)]:
        psbt = wallet.create_psbt([wallet.fund(i, 500_000) for i in range(n_in)],
                                  [(stranger_address(j), 10_000) for j in range(n_out)], fee=300 * n_in)
        dev.tap("Scan QR")
        dev.scan_animated(psbt_parts(psbt, fragment_len=200))
        signed, _ = review_and_sign(dev)
        wallet.combine(psbt, signed)
        assert wallet.signers(psbt) == {Seed(MNEMONIC).fingerprint}


def test_out_of_memory_signing_says_too_big(dev):
    """70 inputs pass review but don't fit the board's heap while signing: the
    signer says it's too big (and what to do), not that it's invalid."""
    wallet = linked_wallet(dev)
    psbt = wallet.create_psbt([wallet.fund(i, 500_000) for i in range(70)], [(stranger_address(), 10_000)], fee=21_000)
    dev.tap("Scan QR")
    dev.scan_animated(psbt_parts(psbt, fragment_len=200))
    dev.tap("Check addresses")
    while dev.screen == "PSBT_OUTPUT":
        dev.tap("Next address" if "Next address" in dev.texts() else "Continue")
    dev.slide()
    assert dev.screen == "PSBT_ERROR" and "too big" in dev.text() and "fewer coins" in dev.text(), dev.text()
