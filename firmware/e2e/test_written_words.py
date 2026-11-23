"""Scan seed: one screen for a SeedQR or a written backup. Written words are
read without a tap once the camera holds still on them; a QR never starts it.

The page is a public test seed (handwriting/make_page.py). Never real funds.
"""
from PIL import Image

from device import FIRMWARE

PAGE = FIRMWARE / "sim" / "samples" / "handwritten_page.png"
GREY = Image.new("L", (320, 240), 128)


def test_written_page_starts_by_itself(dev):
    dev.tap("Scan seed")
    assert dev.screen == "SCAN_SEED"
    dev.show_page(PAGE)
    dev.wait_for(lambda: dev.screen == "PAGE_FOUND", ms=5000, what="the words to be found")
    assert "found" in dev.text()

    # Retake: the same view isn't taken again until the camera moves.
    dev.wait(200)   # past the fade-in: a tap during it is dropped
    dev.tap("Retake")
    assert dev.screen == "SCAN_SEED"
    dev.wait(3000)
    assert dev.screen == "SCAN_SEED", dev.text()
    dev.show(GREY)
    dev.wait(300)
    dev.show_page(PAGE)
    dev.wait_for(lambda: dev.screen == "PAGE_FOUND", ms=5000, what="the words to be found again")


def test_qr_held_still_does_not_read_words(dev):
    """A QR that isn't a seed stays a QR: its modules are never taken for words."""
    dev.tap("Scan seed")
    dev.show(Image.open(FIRMWARE / "sim" / "samples" / "testpsbt_height_demise_base64.png").convert("L"))
    dev.wait(3000)
    assert dev.screen == "SCAN_SEED", dev.text()
    assert "That's a transaction" in dev.text()
