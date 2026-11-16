"""What the user does on the signer, step by step, shared by the tests."""
import re

from hostwallet import parse_key_export, read_psbt, seedqr


def fingerprint_on_screen(dev):
    """The fingerprint as displayed, e.g. "73c5 da0a", without the space."""
    shown = [t for t in dev.texts() if re.fullmatch(r"[0-9a-f]{4} [0-9a-f]{4}", t)]
    assert len(shown) == 1, dev.text()
    return shown[0].replace(" ", "")


def type_words(dev, mnemonic):
    """Enter words: type each word's letters until it is offered, then tap it."""
    for word in mnemonic.split():
        for letter in word:
            if word in dev.texts():
                break
            dev.key(letter)
        dev.tap(word)


def load_by_words(dev, mnemonic):
    dev.tap("Enter words")
    dev.tap("%d words" % len(mnemonic.split()))
    type_words(dev, mnemonic)
    assert dev.screen == "SEED_LOADED", dev.text()


def load_by_qr(dev, mnemonic, compact=False):
    dev.tap("Scan QR")
    dev.scan(seedqr(mnemonic, compact))
    assert dev.screen == "SEED_LOADED", dev.text()


def connect_wallet(dev):
    """Wallet menu -> Connect wallet on testnet. Returns the key the wallet
    app reads from the signer's screen, and leaves the signer on its menu."""
    dev.tap("Connect wallet")
    if "Network: Testnet" not in dev.text():
        dev.tap("Network: Mainnet")
        dev.tap("Testnet")
    export = dev.read_qr()
    dev.back()
    return parse_key_export(export)


def address_on_screen(dev):
    """An address as displayed, in groups of four, put back together."""
    texts = dev.texts()
    at = texts.index("ADDRESS") + 1 if "ADDRESS" in texts else next(i for i, t in enumerate(texts) if t in ("tb1q", "bc1q"))
    chunks = []
    for t in texts[at:]:
        if not re.fullmatch(r"[a-z0-9]{1,4}", t):
            break
        chunks.append(t)
    return "".join(chunks)


def verify_address(dev, shown):
    """Wallet menu -> Verify address with the QR `shown` by the wallet app.
    Returns the result screen's text."""
    dev.tap("Verify address")
    dev.scan(shown)
    assert dev.screen == "VERIFY_RESULT", dev.text()
    dev.wait_for(lambda: "Done" in dev.texts(), what="the address search")
    result = dev.text()
    dev.tap("Done")
    return result


def review_and_sign(dev):
    """From Review: step through every output, slide to sign, and scan the
    signed transaction off the screen. Returns it, with the texts of each
    screen passed on the way for the test to check."""
    seen = [dev.text()]
    dev.tap("Check addresses")
    while dev.screen == "PSBT_OUTPUT":
        seen.append(dev.text() + "\n= " + address_on_screen(dev))   # the address in one piece, for the test
        dev.tap("Next address" if "Next address" in dev.texts() else "Continue")
    assert dev.screen == "PSBT_SIGN", dev.text()
    seen.append(dev.text())
    dev.slide()
    assert dev.screen == "PSBT_SIGNED", dev.text()

    def next_frame():
        frame = dev.read_qr()
        dev.wait(200)
        return frame

    signed = read_psbt(next_frame)
    dev.tap("Done")
    return signed, seen
