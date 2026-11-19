"""Update firmware: the case has no BOOT button, so the home screen restarts
the board into its USB bootloader. The simulator quits where the board would
come back as a drive."""
import pytest


def version(dev):
    """The version label in the top right corner of Home."""
    return min((w for w in dev.widgets() if w.kind == "label" and w.text and w.x1 > 120), key=lambda w: w.y1)


def test_version_opens_update_and_cancel_returns(dev):
    assert dev.screen == "HOME"
    dev._tap(version(dev))
    assert dev.screen == "UPDATE", dev.text()
    assert "USB drive" in dev.text()
    dev.tap("Cancel")
    assert dev.screen == "HOME"


def test_restart_leaves_the_firmware(dev):
    dev._tap(version(dev))
    with pytest.raises(RuntimeError, match="died"):
        dev.tap("Restart")
    assert dev.proc.wait(timeout=10) == 0
