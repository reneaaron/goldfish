#!/usr/bin/env python3
# Write starting corpora for the fuzzers from the repo's own test vectors.
# Usage: uv run --with segno fuzz/seeds.py   (from firmware/)
import base64, re, pathlib, hashlib, random
import segno

FW = pathlib.Path(__file__).resolve().parent.parent
CORPUS = FW / "fuzz" / "corpus"
fixt = (FW / "test/fixtures/seedsigner_vectors.h").read_text()

def put(name, data):
    d = CORPUS / name
    d.mkdir(parents=True, exist_ok=True)
    (d / hashlib.sha1(data).hexdigest()[:16]).write_bytes(data)

def cstr(name):
    m = re.search(r'%s\s*=\s*((?:"[^"]*"\s*)+);' % name, fixt)
    return "".join(re.findall(r'"([^"]*)"', m.group(1)))

single_b64 = cstr("SINGLESIG_FRAME")
multi_b64 = cstr("UR_MULTISIG_BASE64")
parts = re.findall(r"\"(UR:[^\"]+)\"", fixt, re.I)

# psbt: mode byte 0 = raw, 1 = base64
for b64 in (single_b64, multi_b64):
    put("psbt", b"\x00" + base64.b64decode(b64))
    put("psbt", b"\x01" + b64.encode())

# ur: text (all parts, a prefix, single part)
put("ur", b"\x00" + "\n".join(parts).encode())
put("ur", b"\x00" + "\n".join(parts[:5]).encode())
put("ur", b"\x00" + parts[0].encode())

# ur structured: bodies are the bytewords payloads with the CRC stripped
words = re.findall(r'"([a-z]{4})"', (FW / "src/app/ur.cpp").read_text())
assert len(words) == 256, len(words)
minimal = {w[0] + w[3]: i for i, w in enumerate(words)}
def bw_decode(s):
    b = bytes(minimal[s[i:i + 2].lower()] for i in range(0, len(s), 2))
    return b[:-4]
bodies = [bw_decode(p.rsplit("/", 1)[1]) for p in parts]
put("ur", b"\x01" + b"".join(bytes([len(b)]) + b for b in bodies if len(b) < 256))
put("ur", b"\x01" + b"".join(bytes([len(b)]) + b for b in bodies[:3] if len(b) < 256))
# ur round trip
put("ur", b"\x02" + bytes([40, 7]) + base64.b64decode(single_b64))
put("ur", b"\x02" + bytes([0, 1]) + b"hello")

# seedqr
src = (FW / "test/test_seedqr/test_main.cpp").read_text()
for digits in re.findall(r'"(\d{48}|\d{96})"', src):
    put("seedqr", digits.encode())
put("seedqr", bytes(range(16)))
put("seedqr", bytes(range(32)))

# address
for a in ["bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu",
          "bitcoin:BC1Q8C6FSHW2DLWUN7EKN9QWF37CU2RN755UPCP6EL?amount=0.1",
          "tb1qar0srrr7xfkvy5l643lydnw9re59gtzzwf5mdq", "1BoatSLRHtKNngkdXEeobR76b53LETtpyT",
          "bc1p5d7rjq7g6rdk2yhzks9smlaqtedr4dekq08ge8ztwac72sfr9rusxg3297"]:
    put("address", a.encode())

# qr: mode 0 = cell grid, mode 1 = small image
random.seed(1)
payloads = [single_b64[:200], parts[0].upper(), "011513251154012711900771041507421289190620080870026613431420201617920614089619290300152408010643", "bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu", bytes(range(16))]
for p in payloads:
    q = segno.make_qr(p, error="l")
    m = [list(r) for r in q.matrix]
    size = len(m)
    version = (size - 17) // 4
    bits = bytearray((size * size + 7) // 8)
    for y in range(size):
        for x in range(size):
            if m[y][x]:
                i = y * size + x
                bits[i >> 3] |= 1 << (i & 7)
    put("qr", bytes([0, (version - 1) % 40]) + bytes(bits))
    # image: 2 px per module + 2-module quiet zone, must fit 21+15*8 = 141 px
    scale, quiet = 2, 2
    px = (size + 2 * quiet) * scale
    if px <= 141:
        k = (px - 21 + 7) // 8
        w = 21 + k * 8
        img = bytearray([255]) * (w * w)
        for y in range(size):
            for x in range(size):
                if m[y][x]:
                    for dy in range(scale):
                        for dx in range(scale):
                            img[((y + quiet) * scale + dy) * w + (x + quiet) * scale + dx] = 0
        put("qr", bytes([1, k | (k << 4)]) + bytes(img))

for d in sorted(CORPUS.iterdir()):
    print(d.name, len(list(d.iterdir())))
