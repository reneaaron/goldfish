"""New seed -> On this device: its warning first, then the chip, camera, shaking
and a scribble (in the simulator the host's random source stands in for the
chip, the motion sensor and the camera's noise), then the usual words."""
from embit import bip39


def read_words(dev):
    assert "Keep it offline" in dev.text()
    dev.tap("I understand")
    texts = dev.texts()   # all the words on one list, scrolled or not
    return " ".join(texts[i + 1] for i, t in enumerate(texts) if t.isdigit())


def generate(dev):
    dev.tap("Create new seed")
    dev.tap("On this device")
    assert dev.screen == "DEVICE_WARNING"
    dev.tap("Skip dice")
    assert dev.screen == "ENTROPY_DEVICE"
    # Chip, camera and shaking run by themselves, each up for a few seconds.
    assert "The chip" in dev.texts()   # step 1 is on screen too, not over before it's drawn
    dev.wait_for(lambda: "Camera noise" in dev.texts(), ms=6000)
    dev.wait(2000)
    assert "Camera noise" in dev.texts()   # not over in a flash
    dev.wait_for(lambda: "Scribble" in dev.texts(), ms=15000, what="camera and shaking to finish")
    for i in range(40):
        if dev.screen != "ENTROPY_DEVICE":
            break
        x = 30 + (i * 37) % 170
        dev.cmd("drag %d 200 %d 290" % (x, 210 - x // 2))
    assert dev.screen == "BACKUP_WARNING", dev.text()
    return read_words(dev)


def test_warning_comes_first(dev):
    dev.tap("Create new seed")
    dev.tap("On this device")
    text = dev.text()
    assert "Skip the dice?" in text
    assert "no way to confirm" in text
    dev.tap("Back")   # nothing read yet, nothing made
    assert dev.screen == "NEW_SEED"


def test_generate_on_device(dev):
    first = generate(dev)
    assert len(first.split()) == 12 and bip39.mnemonic_is_valid(first), first
    dev.back()             # the word list -> Keep it offline
    dev.tap("Discard")     # -> New seed, the words gone
    assert dev.screen == "NEW_SEED"
    dev.back()
    second = generate(dev)
    assert second != first


def test_leaving_midway_makes_nothing(dev):
    dev.tap("Create new seed")
    dev.tap("On this device")
    dev.tap("Skip dice")
    dev.wait_for(lambda: "Shake it" in dev.texts(), ms=10000, what="the camera step to finish")
    dev.wait(200)   # past the fade-in
    dev.back()
    assert dev.screen == "NEW_SEED"
    dev.tap("On this device")
    assert dev.screen == "DEVICE_WARNING"   # the warning again, every time
