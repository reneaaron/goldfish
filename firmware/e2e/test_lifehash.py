"""LifeHash: the picture beside a fingerprint is the one Sparrow shows for it
(version 2 of the fingerprint's four bytes). The pictures to compare with were
made by Sparrow's own library, toucan."""
from pathlib import Path

from flows import load_by_words

MNEMONIC = "crew marble private differ race truly blush basket crater affair prepare unique"
SPARROW = {}
for line in (Path(__file__).parent / "fixtures" / "lifehash_toucan.txt").read_text().splitlines():
    if not line.startswith("#"):
        fp, rgb = line.split()
        SPARROW[fp] = bytes.fromhex(rgb)


def pictures(dev):
    return [w for w in dev.widgets() if w.kind == "lifehash" and w.on]


def assert_is(dev, w, fingerprint, scale=1):
    """The screen at `w` is Sparrow's picture, as far as the screen's 16-bit colour goes."""
    want, shot = SPARROW[fingerprint], dev.screenshot().convert("RGB")
    assert (w.x2 - w.x1 + 1, w.y2 - w.y1 + 1) == (32 * scale, 32 * scale)
    for y in range(32):
        for x in range(32):
            got = shot.getpixel((w.x1 + x * scale + scale // 2, w.y1 + y * scale + scale // 2))
            px = want[(y * 32 + x) * 3:(y * 32 + x) * 3 + 3]
            assert all(abs(g - p) <= 8 for g, p in zip(got, px)), (x, y, got, tuple(px))


def test_wallet_shows_sparrows_picture_and_doubles_on_tap(dev):
    load_by_words(dev, MNEMONIC)
    assert dev.screen == "WALLET"
    (pic,) = pictures(dev)
    assert_is(dev, pic, "d4ff2c22")
    dev._tap(pic)                                # double size, with the fingerprint under it
    small, big = sorted(pictures(dev), key=lambda w: w.x2 - w.x1)
    assert_is(dev, big, "d4ff2c22", scale=2)
    assert dev.texts().count("d4ff 2c22") == 2
    dev.cmd("tap 120 300")                       # anywhere: away again
    assert len(pictures(dev)) == 1 and dev.screen == "WALLET"


def test_passphrase_changes_the_picture(dev):
    load_by_words(dev, MNEMONIC)
    dev.tap("Wallet options")
    dev.tap("Passphrase")
    for letter in "goldfish":
        dev.key(letter)
    dev.key("OK")
    assert dev.screen == "WALLET" and "ffb9 e779" in dev.text()
    (pic,) = pictures(dev)
    assert_is(dev, pic, "ffb9e779")


def test_forget_seed_shows_it_too(dev):
    load_by_words(dev, MNEMONIC)
    dev.tap("Wallet options")
    for _ in range(3):
        dev.scroll_down()
    dev.tap("Forget seed")
    (pic,) = pictures(dev)
    assert_is(dev, pic, "d4ff2c22")
