"""The signer, as its user and a wallet app's screen and camera meet it.

Drives the desktop simulator (SIM_CONTROL=1, see src/sim/main.cpp) with the
only three things a person has: reading the screen, touching it, and holding
a QR code up to the camera. Nothing here reaches into the firmware.
"""
import re
import subprocess
from collections import namedtuple
from pathlib import Path

import qrcode
import zxingcpp
from PIL import Image

FIRMWARE = Path(__file__).resolve().parent.parent
BINARY = FIRMWARE / ".pio/build/sim/program"

Widget = namedtuple("Widget", "kind x1 y1 x2 y2 on raw text")


def _defines(header, pattern):
    return re.findall(pattern, (FIRMWARE / "src/app" / header).read_text(), re.M)


# Icons are private-use glyphs: show them as {NAME} so screen text is readable.
ICONS = {
    bytes.fromhex(code.replace("\\x", "")).decode(): name
    for name, code in _defines("theme.h", r'^#define ICON_(\w+)\s+"((?:\\x[0-9A-F]{2})+)"')
}
SCREENS = _defines("ui.h", r"^\s+UI_(\w+),")


def _clean(raw):
    for glyph, name in ICONS.items():
        raw = raw.replace(glyph, "{%s}" % name)
    return raw.replace("\\n", "\n").strip()


def qr_image(payload, camera=(240, 180)):
    """A QR code as a wallet app shows it, sized to fill the camera's view
    (the board's 240x180). A dense one is framed tight, part of its quiet zone
    out of view, so its modules are still 2 pixels wide, as a user would hold it."""
    qr = qrcode.QRCode(error_correction=qrcode.constants.ERROR_CORRECT_L, border=4, box_size=1)
    qr.add_data(payload)
    qr.make(fit=True)
    qr.border = max(1, min(4, (min(camera) // 2 - qr.modules_count) // 2))
    side = qr.modules_count + 2 * qr.border
    qr.box_size = min(camera) // side
    if qr.box_size < 2:   # 1-pixel modules, fitted to the view: unreadable, as on the board
        raise ValueError(f"{len(payload)} characters make a {qr.modules_count}-module QR, too dense for a {camera} view")
    return qr.make_image().convert("L")


class Device:
    def __init__(self, workdir):
        self.dir = Path(workdir)
        self.proc = subprocess.Popen([str(BINARY)], cwd=FIRMWARE, env={"SIM_CONTROL": "1"},
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.shots = 0

    def close(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=10)

    def cmd(self, line):
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        out = []
        while True:
            reply = self.proc.stdout.readline()
            if not reply:
                raise RuntimeError("the simulator died on: " + line)
            reply = reply.rstrip("\n")
            if reply == "ok":
                return out
            if reply.startswith("err"):
                raise RuntimeError("%s: %s" % (line, reply))
            out.append(reply)

    # ------------------------------------------------------------ eyes
    @property
    def screen(self):
        return SCREENS[int(self.cmd("screen")[0])]

    def widgets(self):
        out = []
        for line in self.cmd("widgets"):
            kind, x1, y1, x2, y2, flags, raw = line.split("\t")
            out.append(Widget(kind, int(x1), int(y1), int(x2), int(y2), flags != "off", raw, _clean(raw)))
        return out

    def texts(self):
        """Every piece of text on the screen, top to bottom as laid out."""
        return [w.text for w in self.widgets() if w.kind == "label" and w.text]

    def text(self):
        return "\n".join(self.texts())

    def screenshot(self, name=None):
        self.shots += 1
        path = self.dir / (name or "screen_%03d.ppm" % self.shots)
        self.cmd("shot %s" % path)
        return Image.open(path)

    def read_qr(self, raw=False):
        """Scan the QR on the screen, as a wallet app's camera would."""
        img = self.screenshot("qr.ppm").convert("L")
        img = img.resize((img.width * 3, img.height * 3), Image.NEAREST)
        found = zxingcpp.read_barcodes(img)
        assert found, "no QR code on the screen:\n" + self.text()
        return found[0].bytes if raw else found[0].text

    # ------------------------------------------------------------ fingers
    def _find(self, kind, text):
        ws = [w for w in self.widgets() if w.kind == kind]
        hits = [w for w in ws if w.text == text] or [w for w in ws if text in w.text]
        assert hits, "no %s %r on %s:\n%s" % (kind, text, self.screen, self.text())
        hit = hits[0]
        assert hit.on, "%s %r is out of view or disabled on %s" % (kind, text, self.screen)
        return hit

    def _tap(self, w):
        self.cmd("tap %d %d" % ((w.x1 + w.x2) // 2, (w.y1 + w.y2) // 2))

    def tap(self, text):
        """Touch the button (or anything else) labelled `text`."""
        self._tap(self._find("label", text))

    def tap_icon(self, name):
        self.tap("{%s}" % name)

    def back(self):
        self.tap_icon("LEFT")

    def key(self, text):
        """Touch a key of the on-screen keyboard."""
        self._tap(self._find("key", text))

    def has_key(self, text):
        return any(w.kind == "key" and w.text == text and w.on for w in self.widgets())

    def slide(self):
        """Slide the "Slide to sign" knob all the way."""
        s = next(w for w in self.widgets() if w.kind == "slider")
        y = (s.y1 + s.y2) // 2
        self.cmd("drag %d %d %d %d" % (s.x1 + 28, y, s.x2 - 2, y))

    def scroll_down(self):
        """Drag a finger up the screen, as when scrolling a list down
        (starting above where a bottom button sits)."""
        self.cmd("drag 120 230 120 50")

    def scroll_up(self):
        """Drag a finger down the screen, back to the top of a list."""
        self.cmd("drag 120 80 120 280")

    def wait(self, ms):
        self.cmd("wait %d" % ms)

    def wait_for(self, done, ms=20000, what="the screen to change"):
        for _ in range(ms // 50):
            if done():
                return
            self.wait(50)
        raise AssertionError("gave up waiting for %s on %s:\n%s" % (what, self.screen, self.text()))

    # ------------------------------------------------------------ camera
    def show(self, image):
        """Hold a picture up to the camera."""
        path = self.dir / "camera.png"
        image.save(path)
        self.cmd("camera %s" % path)

    def show_page(self, path):
        """Hold a written page up to the camera; its picture file is also the still."""
        self.cmd("page %s" % path)

    def look_away(self):
        self.cmd("camera")

    def scan(self, payload, ms=600):
        """Show one QR code and give the scanner time to read it."""
        self.show(qr_image(payload))
        self.wait(ms)
        self.look_away()

    def scan_animated(self, parts, ms_per_frame=200, max_frames=400):
        """Show an animated QR, frame after frame, until the scanner has seen
        enough and leaves its screen. `parts` is an endless iterator."""
        scanner = self.screen
        assert scanner.startswith("SCAN"), scanner
        for frames in range(1, max_frames + 1):
            self.show(qr_image(next(parts)))
            self.wait(ms_per_frame)
            if self.screen != scanner:
                self.look_away()
                return frames
        raise AssertionError("the animated QR was never completed:\n" + self.text())
