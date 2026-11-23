"""Brightness: a drag down from the top edge opens a panel with a slider, on
any screen. Closing it leaves the screen under it as it was, and the drag that
opened it doesn't press what it started on."""


def panel_open(dev):
    return any(w.kind == "slider" for w in dev.widgets()) and "BRIGHTNESS" in dev.texts()


def test_pull_down_opens_and_tap_below_closes(dev):
    assert not panel_open(dev)
    dev.cmd("drag 120 4 120 100")
    assert panel_open(dev), dev.text()
    dev.tap("Scan seed")             # under the shade: closes it, doesn't scan
    assert not panel_open(dev)
    assert dev.screen == "HOME"


def test_drag_from_back_button_opens_panel_not_back(dev):
    dev.tap("Scan seed")
    assert dev.screen == "SCAN_SEED"
    dev.cmd("drag 24 10 24 120")   # starts on the back chevron
    assert panel_open(dev)
    assert dev.screen == "SCAN_SEED"


def test_drag_panel_back_up_closes(dev):
    dev.cmd("drag 120 4 120 100")
    assert panel_open(dev)
    dev.cmd("drag 120 85 120 5")
    assert not panel_open(dev)


def test_drag_lower_down_is_not_a_pull(dev):
    dev.cmd("drag 120 60 120 200")
    assert not panel_open(dev)
