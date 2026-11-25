"""Seed words shown on screen don't stay in the signer's memory once the
screen is gone: LVGL copies label text into its heap and frees it without
wiping, so freed blocks are zeroed after each screen change. Reads the
simulator's memory (Linux only).

The page is a public test seed (handwriting/make_page.py). Never real funds.
"""
import re
import sys

import pytest

from device import FIRMWARE
from embit.wordlists.bip39 import WORDLIST

PAGE = FIRMWARE / "sim" / "samples" / "handwritten_page.png"

pytestmark = pytest.mark.skipif(not sys.platform.startswith("linux"), reason="reads /proc/<pid>/mem")


def writable_memory(pid):
    """Every writable mapping of the process, as one list of byte strings."""
    chunks = []
    with open(f"/proc/{pid}/maps") as maps, open(f"/proc/{pid}/mem", "rb", 0) as mem:
        for line in maps:
            m = re.match(r"([0-9a-f]+)-([0-9a-f]+) (\S+)", line)
            if "w" not in m.group(3) or "[vvar" in line or "[vsyscall" in line:
                continue
            start, end = int(m.group(1), 16), int(m.group(2), 16)
            try:
                mem.seek(start)
                chunks.append(mem.read(end - start))
            except OSError:
                pass
    return chunks


def words_left(dev, words):
    """Which of `words` are still in memory, each on its own as a C string."""
    mem = writable_memory(dev.proc.pid)
    return [w for w in words if any(re.search(rb"\0%s\0" % w.encode(), c) for c in mem)]


def test_written_words_wiped_after_leaving(dev):
    dev.tap("Scan")
    dev.show_page(PAGE)
    dev.wait_for(lambda: dev.screen == "PAGE_FOUND", ms=5000, what="the words to be found")
    dev.wait(200)                       # past the fade-in
    dev.tap("Read")
    dev.wait_for(lambda: dev.screen == "PAGE_REVIEW", ms=20000, what="the words to be read")
    shown = [t for t in dev.texts() if t in WORDLIST]
    assert len(shown) >= 6, dev.text()
    assert words_left(dev, shown), "the check finds nothing even while the words are on screen"

    while dev.screen != "HOME":
        dev.back()
    dev.wait(1000)                      # the fade, the old screen deleted, the wipe after it
    assert words_left(dev, shown) == []
