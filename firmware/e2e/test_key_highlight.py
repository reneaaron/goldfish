"""Keypad keys (dice, coins, the child number): the pressed colour stays for a
moment after the finger lifts, since the finger covers the key until then."""


def check_lingers(dev, w, at):
    """`w` is a label on the key, `at` a point on the key's background."""
    colour = lambda: dev.screenshot().getpixel(at)
    idle = colour()
    dev.cmd("press %d %d" % ((w.x1 + w.x2) // 2, (w.y1 + w.y2) // 2))
    pressed = colour()
    assert pressed != idle
    dev.cmd("release 400")                       # lifted: still shown
    assert colour() == pressed
    dev.wait(400)                                # 800 ms after lifting: gone
    assert colour() == idle


def test_dice_key_stays_lit_after_release(dev):
    dev.tap("Create new seed")
    dev.tap("Dice rolls")
    w = dev._find("label", "{DICE_4}")
    check_lingers(dev, w, (w.x1 - 4, (w.y1 + w.y2) // 2))   # beside the die face
    assert any(t.endswith("4") for t in dev.texts())    # and the roll counted once


def test_coin_key_stays_lit_after_release(dev):
    dev.tap("Create new seed")
    dev.tap("Coin flips")
    w = dev._find("label", "Heads = 1")
    check_lingers(dev, w, ((w.x1 + w.x2) // 2, w.y1 - 3))    # between the coin and its text
