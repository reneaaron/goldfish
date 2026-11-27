"""Touch guides: while a finger is down, a line across and a line down the
whole screen cross where the touch is registered. Off until switched on in
Settings."""


def lines(before, now):
    """The rows and columns of the screen that changed nearly from edge to edge."""
    a, b = before.load(), now.load()
    rows = [y for y in range(320) if sum(a[x, y] != b[x, y] for x in range(240)) > 200]
    cols = [x for x in range(240) if sum(a[x, y] != b[x, y] for y in range(320)) > 280]
    return rows, cols


def test_guides_cross_at_the_touch_and_follow_it(dev):
    before = dev.screenshot()
    dev.cmd("press 70 100")                      # off at start: nothing shows
    assert dev.screenshot() == before
    dev.cmd("release")
    version = min((w for w in dev.widgets() if w.kind == "label" and w.text and w.x1 > 120), key=lambda w: w.y1)
    dev._tap(version)
    assert "Off" in dev.texts()
    dev.tap("Touch guides")
    assert "On: lines at the touch" in dev.texts()
    dev.back()
    assert dev.screenshot() == before
    dev.cmd("press 70 100")                      # on nothing: the lines are all that changes
    assert lines(before, dev.screenshot()) == ([100], [70])
    dev.cmd("press 150 60")
    assert lines(before, dev.screenshot()) == ([60], [150])
    dev.cmd("release")
    assert dev.screenshot() == before
    assert dev.screen == "HOME"
