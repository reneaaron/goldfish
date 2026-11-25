"""New seed: rolls or flips that look invented get a warning once all are in,
never while typing. Either way the seed comes from exactly what was typed."""
from embit import bip39

COUNTED = "12345612345612345612345612345612345612345612345612"   # SeedSigner's vector
COUNTED_WORDS = "unveil nice picture region tragic fault cream strike tourist control recipe tourist"


def roll(dev, rolls):
    for r in rolls:
        dev.tap_icon("DICE_" + r)


def flip(dev, flips):
    for f in flips:
        dev.tap_icon("HEADS" if f == "1" else "TAILS")


def words_on_screen(dev):
    texts = dev.texts()
    return " ".join(texts[i + 1] for i, t in enumerate(texts) if t.isdigit())


def test_counted_dice_warned_then_start_over(dev):
    dev.tap("Create new seed")
    dev.tap("Dice rolls")
    roll(dev, COUNTED[:49])
    assert dev.screen == "ENTROPY_DICE"   # nothing said before the last roll
    roll(dev, COUNTED[49])
    assert dev.screen == "ENTROPY_SUSPECT"
    assert "These rolls look made up" in dev.text()
    dev.tap("Start over")
    assert dev.screen == "ENTROPY_DICE"
    assert "0 of 50" in dev.text()


def test_counted_dice_used_anyway(dev):
    dev.tap("Create new seed")
    dev.tap("Dice rolls")
    roll(dev, COUNTED)
    dev.tap("I really rolled these")
    assert "Keep it offline" in dev.text()
    dev.tap("I understand")
    words = words_on_screen(dev)
    assert words == COUNTED_WORDS and bip39.mnemonic_is_valid(words)


def test_fair_dice_no_warning(dev):
    dev.tap("Create new seed")
    dev.tap("Dice rolls")
    roll(dev, "22242454136553153234335553456544655622342664655563")
    assert "Keep it offline" in dev.text()


def test_patterned_coins_warned(dev):
    dev.tap("Create new seed")
    dev.tap("Coin flips")
    flip(dev, "1101" * 32)
    assert dev.screen == "ENTROPY_SUSPECT"
    assert "These flips look made up" in dev.text()
    dev.tap("Start over")
    assert dev.screen == "ENTROPY_COINS"
    assert "0 of 128" in dev.text()
