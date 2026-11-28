"""Settings, behind the version on Home. Update firmware: the case has no BOOT
button, so it restarts the board into its USB bootloader. The simulator quits where the board would
come back as a drive."""
import pytest


def version(dev):
    """The version label in the top right corner of Home."""
    return min((w for w in dev.widgets() if w.kind == "label" and w.text and w.x1 > 120), key=lambda w: w.y1)


def test_version_opens_update_and_cancel_returns(dev):
    assert dev.screen == "HOME"
    dev._tap(version(dev))
    assert dev.screen == "SETTINGS", dev.text()
    dev.tap("Update firmware")
    assert dev.screen == "UPDATE", dev.text()
    assert "USB drive" in dev.text()
    dev.tap("Cancel")
    assert dev.screen == "SETTINGS"
    dev.back()
    assert dev.screen == "HOME"


def test_restart_leaves_the_firmware(dev):
    dev._tap(version(dev))
    dev.tap("Update firmware")
    with pytest.raises(RuntimeError, match="died"):
        dev.tap("Restart")
    assert dev.proc.wait(timeout=10) == 0



def test_settings_from_wallet_has_no_update(dev):
    """The version is on Wallet too: the same Settings, without Update
    firmware while a seed is loaded, and back to Wallet."""
    from flows import load_by_words
    load_by_words(dev, "crew marble private differ race truly blush basket crater affair prepare unique")
    assert dev.screen == "WALLET"
    dev._tap(version(dev))
    assert dev.screen == "SETTINGS" and "Update firmware" not in dev.texts()
    assert "Touch guides" in dev.texts()
    dev.back()
    assert dev.screen == "WALLET"
