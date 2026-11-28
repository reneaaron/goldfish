"""Back up: the ways to copy a seed off the signer, from Wallet options for
the loaded seed and from Seed ready for one just made. SeedQR is the card
flow with its optional check; Word numbers is each word's place in the BIP-39
list.

All seeds here are public test seeds. Never real funds.
"""
from embit import bip39

from flows import fingerprint_on_screen, load_by_words
from hostwallet import Seed, seedqr

MNEMONIC = "crew marble private differ race truly blush basket crater affair prepare unique"
STRANGER = Seed("abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about")


def numbers(words):
    """What Word numbers shows: counted from 1, four digits."""
    return ["%04d" % (bip39.WORDLIST.index(w) + 1) for w in words]


def shown_numbers(dev):
    return [t for t in dev.texts() if len(t) == 4 and t.isdigit()]


def new_seed_ready(dev):
    """A new seed from dice, written down, on Seed ready. Returns its words."""
    dev.tap("Create new seed")
    dev.tap("Dice rolls")
    for roll in "63124551326645213355162443316625514432216653411526":
        dev.tap_icon("DICE_" + roll)
    dev.tap("I understand")
    texts = dev.texts()
    words = [texts[i + 1] for i, t in enumerate(texts) if t.isdigit()]
    for _ in range(3):
        dev.scroll_down()
        dev.wait(500)
    dev.tap("Written down")
    assert "Seed ready" in dev.text() and len(words) == 12
    return words


def test_seedqr_card_is_copied_and_checked(dev):
    """Back up, SeedQR: the SeedQR on the signer's screen, copied square by
    square onto a card, passes the card check. A wrong card doesn't."""
    load_by_words(dev, MNEMONIC)
    dev.tap("Wallet options")
    dev.tap("Back up")
    assert dev.screen == "BACKUP_OPTIONS"
    dev.tap("SeedQR")
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


def test_backup_as_word_numbers(dev):
    """Back up, Word numbers: each word's place in the BIP-39 list counted
    from 1, four digits, in the seed's order, after the offline warning.
    Cancel on the warning and Back go to the Back up menu."""
    load_by_words(dev, MNEMONIC)
    dev.tap("Wallet options")
    dev.tap("Back up")
    dev.tap("Word numbers")
    assert dev.screen == "BACKUP_WARNING" and "these numbers" in dev.text()
    dev.tap("Cancel")
    assert dev.screen == "BACKUP_OPTIONS"
    dev.tap("Word numbers")
    dev.tap("I understand")
    assert dev.screen == "BACKUP_NUMBERS"
    want = numbers(MNEMONIC.split())
    assert 1 <= min(map(int, want)) and max(map(int, want)) <= 2048
    assert shown_numbers(dev) == want, dev.text()
    words = [t for t in dev.texts() if t in MNEMONIC.split()]
    assert words == MNEMONIC.split()             # beside each number, to check against
    dev.back()
    assert dev.screen == "BACKUP_OPTIONS"
    dev.tap("Word numbers")
    dev.tap("I understand")
    dev.tap("Done")
    assert dev.screen == "WALLET_OPTIONS"


def test_new_seed_backs_up_from_seed_ready(dev):
    """Seed ready has the same Back up menu, for the seed just made. It isn't
    loaded: Cancel, Back and Done all come back to Seed ready with the same
    fingerprint, and Done there still forgets it."""
    words = new_seed_ready(dev)
    fingerprint = Seed(" ".join(words)).fingerprint
    assert fingerprint_on_screen(dev) == fingerprint
    dev.tap("Back up")
    assert dev.screen == "BACKUP_OPTIONS"
    dev.tap("Word numbers")
    dev.tap("Cancel")
    assert dev.screen == "BACKUP_OPTIONS"
    dev.tap("Word numbers")
    dev.tap("I understand")
    assert shown_numbers(dev) == numbers(words), dev.text()
    dev.tap("Done")
    assert "Seed ready" in dev.text() and fingerprint_on_screen(dev) == fingerprint
    dev.tap("Back up")
    dev.back()
    assert "Seed ready" in dev.text()
    dev.tap("Done")
    assert dev.screen == "HOME"                  # a new seed is never loaded by itself


def test_new_seed_card_check_returns_to_seed_ready(dev):
    """The SeedQR card of a seed just made, checked: back to Seed ready, since
    there is no wallet to go to."""
    words = new_seed_ready(dev)
    dev.tap("Back up")
    dev.tap("SeedQR")
    dev.tap("I understand")
    card = dev.read_qr(raw=True)
    assert card == seedqr(" ".join(words), compact=True)
    dev.tap("Start copying")
    while "Check card" not in dev.texts():
        dev.tap("Next")
    dev.tap("Check card")
    dev.scan(seedqr(STRANGER.mnemonic, compact=True))
    assert dev.screen == "BACKUP_SCAN"
    dev.scan(card)
    assert "Seed ready" in dev.text()
    assert fingerprint_on_screen(dev) == Seed(" ".join(words)).fingerprint
