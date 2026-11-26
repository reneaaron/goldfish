"""Scan seed: one screen for a SeedQR or a written backup. Written words are
read without a tap once the camera holds still on them; a QR never starts it.

The page is a public test seed (handwriting/make_page.py). Never real funds.
"""
from PIL import Image

from device import FIRMWARE
from flows import review_and_sign

PAGE = FIRMWARE / "sim" / "samples" / "handwritten_page.png"
GREY = Image.new("L", (240, 180), 128)


def test_written_page_starts_by_itself(dev):
    dev.tap("Scan")
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


def test_written_page_is_read(dev):
    """Read takes the words to the list, in the board's memory (the simulator
    has the board's heap: a step that doesn't fit shows "Out of memory")."""
    dev.tap("Scan")
    dev.show_page(PAGE)
    dev.wait_for(lambda: dev.screen == "PAGE_FOUND", ms=5000, what="the words to be found")
    dev.wait(200)   # past the fade-in
    dev.tap("Read")
    assert dev.screen == "PAGE_READING", dev.text()
    dev.wait_for(lambda: dev.screen == "PAGE_REVIEW", ms=20000, what="the words to be read")


def test_qr_held_still_does_not_read_words(dev):
    """A QR that isn't a seed stays a QR: its modules are never taken for words."""
    dev.tap("Scan")
    dev.show(Image.open(FIRMWARE / "sim" / "samples" / "testpsbt_height_demise_base64.png").convert("L"))
    dev.wait(3000)
    assert dev.screen == "SCAN_SEED", dev.text()
    assert "That's a transaction" in dev.text()


DEMO_PSBT = FIRMWARE / "sim" / "samples" / "mainpsbt_height_demise_base64.png"


def sign_demo_transaction(dev):
    dev.look_away()
    dev.wait(200)   # past the fade-in
    dev.tap("Scan QR")
    dev.show(Image.open(DEMO_PSBT).convert("L"))
    dev.wait_for(lambda: dev.screen != "SCAN", ms=5000, what="the transaction to be read")
    assert dev.screen == "PSBT_OVERVIEW", dev.text()
    dev.wait(200)   # past the fade-in
    signed, _ = review_and_sign(dev)
    assert len(signed.inputs[0].partial_sigs) == 1
    assert dev.screen == "WALLET"


def test_demo_page_signs_the_demo_transaction(dev):
    """The landing page's hero offers a paper backup and a transaction: the
    page holds the seed the transaction spends from.
    Few layouts of these words read 12 of 12 (handwriting/make_page.py)."""
    dev.tap("Scan")
    dev.show_page(FIRMWARE / "sim" / "samples" / "handwritten_page_demo.png")
    dev.wait_for(lambda: dev.screen == "PAGE_FOUND", ms=5000, what="the words to be found")
    dev.wait(200)   # past the fade-in
    dev.tap("Read")
    dev.wait_for(lambda: dev.screen == "PAGE_REVIEW", ms=20000, what="the words to be read")
    assert "All 12 read" in dev.text(), dev.text()
    dev.wait(200)   # past the fade-in
    dev.tap("Matches my backup")
    dev.wait_for(lambda: dev.screen == "WALLET", what="the seed to load")
    sign_demo_transaction(dev)


def test_demo_seedqr_signs_the_demo_transaction(dev):
    dev.tap("Scan")
    dev.show(Image.open(FIRMWARE / "sim" / "samples" / "testseed_height_demise_standard_12word.png").convert("L"))
    dev.wait_for(lambda: dev.screen == "WALLET", what="the seed to load")
    sign_demo_transaction(dev)
