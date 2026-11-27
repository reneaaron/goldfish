"""The companion and Goldfish (the desktop simulator) together: the companion
links the key from Connect wallet, builds a PSBT and shows it as an animated
UR, Goldfish signs it, and the companion reads the signed animation back and
finalizes a transaction. Everything crosses as QR codes; no network.

Run from companion/:
    uv run --with-requirements ../firmware/e2e/requirements.txt pytest e2e
"""
import itertools
import subprocess
import sys
from pathlib import Path

COMPANION = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(COMPANION.parent / "firmware" / "e2e"))

from device import BINARY, FIRMWARE, Device  # noqa: E402
from flows import load_by_words, set_network  # noqa: E402
from hostwallet import Seed, Wallet  # noqa: E402

MNEMONIC = "crew marble private differ race truly blush basket crater affair prepare unique"
STRANGER = Wallet.singlesig(Seed("abandon " * 11 + "about").key("84h/1h/0h")).address(0)
CLI = COMPANION / "src-tauri" / "target" / "debug" / "companion-cli"


def cli(*args, stdin=None):
    out = subprocess.run([str(CLI), *args], input=stdin, capture_output=True, text=True)
    assert out.returncode == 0, out.stderr
    return dict(line.split(" ", 1) for line in out.stdout.splitlines()), out.stdout.splitlines()


def test_companion_psbt_is_signed_by_goldfish(tmp_path):
    subprocess.run(["pio", "run", "-e", "sim", "-s"], cwd=FIRMWARE, check=True)
    subprocess.run(["cargo", "build", "-q", "--bin", "companion-cli"], cwd=COMPANION / "src-tauri", check=True)
    assert BINARY.exists()
    dev = Device(tmp_path)
    try:
        # Link: the companion reads the key off Connect wallet.
        load_by_words(dev, MNEMONIC)
        dev.tap("Wallet options")
        set_network(dev, "Testnet")
        dev.tap("Connect wallet")
        export = dev.read_qr()
        dev.back()
        dev.back()

        # Send: the companion's animated PSBT, scanned by Goldfish.
        fields, lines = cli("demo-psbt", export, STRANGER, "30000")
        frames = [l.split(" ", 1)[1] for l in lines if l.startswith("FRAME ")]
        dev.tap("Scan QR")
        dev.scan_animated(itertools.cycle(frames))
        assert dev.screen == "PSBT_OVERVIEW", dev.text()
        overview = dev.text()
        assert "₿30,000" in overview and "TESTNET" in overview

        dev.tap("Check addresses")
        while dev.screen == "PSBT_OUTPUT":
            dev.tap("Next address" if "Next address" in dev.texts() else "Continue")
        assert dev.screen == "PSBT_SIGN", dev.text()
        dev.slide()
        assert dev.screen == "PSBT_SIGNED", dev.text()

        # Signed: the companion reads Goldfish's animation off its screen.
        signed_frames = []
        for _ in range(120):
            signed_frames.append(dev.read_qr())
            dev.wait(200)
        result, _ = cli("finalize", export, fields["PSBT"], stdin="\n".join(signed_frames) + "\n")
        assert len(result["TXID"]) == 64
        assert result["TX"].startswith("02000000000101")   # v2, segwit, one input
        dev.tap("Done")
    finally:
        dev.close()


def test_companion_multisig_two_of_three(tmp_path):
    """A 2-of-3 the companion sets up around Goldfish's multisig key: Goldfish
    confirms the companion's descriptor QR, signs, and the companion merges
    in a second cosigner's signature before it can finish the transaction."""
    from base64 import b64decode, b64encode

    from embit.psbt import PSBT

    subprocess.run(["pio", "run", "-e", "sim", "-s"], cwd=FIRMWARE, check=True)
    subprocess.run(["cargo", "build", "-q", "--bin", "companion-cli"], cwd=COMPANION / "src-tauri", check=True)
    cosigners = [Seed("zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo wrong"),
                 Seed("legal winner thank year wave sausage worth useful legal winner thank yellow")]
    dev = Device(tmp_path)
    try:
        # Goldfish's multisig key, as the companion scans it off Multisig -> Show my key.
        load_by_words(dev, MNEMONIC)
        dev.tap("Wallet options")
        set_network(dev, "Testnet")
        dev.tap("Multisig")
        dev.tap("Show my key")
        goldfish_key = dev.read_qr()
        assert "/48h/1h/0h/2h]Vpub" in goldfish_key
        while dev.screen != "WALLET":
            dev.back()

        made, _ = cli("multisig", "2", goldfish_key, *(c.key("48h/1h/0h/2h") for c in cosigners))
        descriptor = made["DESCRIPTOR"]

        # Goldfish learns the wallet from the companion's descriptor QR.
        dev.tap("Scan QR")
        dev.scan(descriptor)
        assert dev.screen == "MULTISIG", dev.text()
        assert "2 of 3 multisig" in dev.text()
        dev.tap("Use wallet")
        if dev.screen != "SCAN":
            dev.tap("Scan QR")

        # The companion's PSBT, reviewed as the 2-of-3 and signed.
        fields, lines = cli("demo-psbt", descriptor, STRANGER, "30000")
        frames = [l.split(" ", 1)[1] for l in lines if l.startswith("FRAME ")]
        dev.scan_animated(itertools.cycle(frames))
        assert dev.screen == "PSBT_OVERVIEW", dev.text()
        assert "2 OF 3" in dev.text() and "₿30,000" in dev.text()
        dev.tap("Check addresses")
        while dev.screen == "PSBT_OUTPUT":
            dev.tap("Next address" if "Next address" in dev.texts() else "Continue")
        dev.slide()
        assert dev.screen == "PSBT_SIGNED", dev.text()
        signed_frames = []
        for _ in range(120):
            signed_frames.append(dev.read_qr())
            dev.wait(200)
        stdin = "\n".join(signed_frames) + "\n"

        # Goldfish's signature alone is one of two.
        alone, _ = cli("finalize", descriptor, fields["PSBT"], stdin=stdin)
        assert alone["SIGNED"].startswith("1/2 ") and "TXID" not in alone

        # With a cosigner's, the companion finishes the transaction.
        psbt = PSBT.parse(b64decode(fields["PSBT"]))
        cosigners[1].sign(psbt)
        both, _ = cli("finalize", descriptor, b64encode(psbt.serialize()).decode(), stdin=stdin)
        assert both["SIGNED"].startswith("2/2 ") and len(both["TXID"]) == 64
        dev.tap("Done")
    finally:
        dev.close()
