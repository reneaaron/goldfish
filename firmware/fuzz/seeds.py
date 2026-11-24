#!/usr/bin/env python3
# Write starting corpora for the fuzzers from the repo's own test vectors.
# Usage: uv run --with segno --with embit fuzz/seeds.py   (from firmware/)
import base64, re, pathlib, hashlib, random, copy, struct, zlib
import segno
from embit.psbt import PSBT, DerivationPath
from embit.ec import PrivateKey

FW = pathlib.Path(__file__).resolve().parent.parent
CORPUS = FW / "fuzz" / "corpus"
fixt = (FW / "test/fixtures/seedsigner_vectors.h").read_text()
msfixt = (FW / "test/fixtures/multisig_vectors.h").read_text()

def put(name, data):
    d = CORPUS / name
    d.mkdir(parents=True, exist_ok=True)
    (d / hashlib.sha1(data).hexdigest()[:16]).write_bytes(data)

def cstr(name, text=None):
    m = re.search(r'%s\s*=\s*((?:"[^"]*"\s*)+);' % name, text or fixt)
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

# ------------------------------------------------------------ malformed input
# Shapes that crashed SeedSigner (Python exceptions there, possible memory
# bugs here): a cosigner key without a key origin (SeedSigner #1045), empty or
# missing derivation paths (#1074, #1076), sign-message QRs (#1058, #1062: the
# device has no sign-message, so they must be refused as text), QR payloads
# that fail structural validation (#1029), plus UR fragments past the limits,
# wrong CRCs, PSBTs at and past PSBT_MAX_OUTPUTS and odd key types.
H = 0x80000000

def bw_encode(body):   # bytewords minimal, with the CRC32 bc-ur appends
    full = body + struct.pack(">I", zlib.crc32(body))
    return "".join(words[b][0] + words[b][3] for b in full)

def cb_head(major, v):
    if v < 24: return bytes([major << 5 | v])
    for n, ai in ((1, 24), (2, 25), (4, 26), (8, 27)):
        if v < 1 << (8 * n): return bytes([major << 5 | ai]) + v.to_bytes(n, "big")
def cb_uint(v): return cb_head(0, v)
def cb_bytes(b): return cb_head(2, len(b)) + b
def cb_arr(items): return cb_head(4, len(items)) + b"".join(items)
def cb_map(pairs): return cb_head(5, len(pairs)) + b"".join(cb_uint(k) + v for k, v in pairs)
def cb_tag(t, item): return cb_head(6, t) + item
def cb_bool(b): return bytes([0xf5 if b else 0xf4])

def ur_part(typ, seq, seq_len, msg_len, checksum, frag):
    body = cb_arr([cb_uint(seq), cb_uint(seq_len), cb_uint(msg_len), cb_uint(checksum), cb_bytes(frag)])
    return f"ur:{typ}/{seq}-{seq_len}/" + bw_encode(body)

# ur text: truncated, wrong CRC, and parts at and past the limits
p0 = parts[0]
put("ur", b"\x00" + p0[: len(p0) // 2].encode())
put("ur", b"\x00" + (p0[:-1] + ("a" if p0[-1] != "a" else "e")).encode())   # CRC no longer matches
put("ur", b"\x00" + "\n".join(parts[:3] + [parts[1][:-8]]).encode())
put("ur", b"\x00ur:crypto-psbt/")
put("ur", b"\x00ur:crypto-psbt/1-1/")
put("ur", b"\x00ur:crypto-psbt/0-0/" + bw_encode(b"").encode())
for seq, seq_len, msg_len, frag_len in [
        (1, 128, 8192, 64),     # exactly UR_MAX_MESSAGE in UR_MAX_FRAGMENTS fragments
        (1, 33, 8193, 256),     # one byte past UR_MAX_MESSAGE
        (1, 129, 1290, 10),     # past UR_MAX_FRAGMENTS
        (1, 1, 257, 257),       # fragment longer than 256
        (0, 1, 10, 10),         # seq 0
        (1, 0, 0, 0),           # zero-length message and fragment
        (1, 2, 10, 10),         # seq_len that doesn't match msg_len / frag_len
        (200, 2, 20, 10),       # a mixed part
        (2**32 - 1, 3, 30, 10)]:
    frag = bytes(random.randrange(256) for _ in range(frag_len))
    for typ in ("crypto-psbt", "crypto-output", "bytes"):
        put("ur", b"\x00" + ur_part(typ, seq, seq_len, msg_len, 0x12345678, frag).encode())
big = bytes([0x59, 0x20, 0x00]) + bytes(8192)          # a CBOR byte string of 8192 bytes, single part
put("ur", b"\x00ur:crypto-psbt/" + bw_encode(big).encode())
put("ur", b"\x02" + bytes([54, 3]) + bytes(random.randrange(256) for _ in range(8192)))   # round trip, 128 x 64

# seedqr: lengths around the valid ones, index 2048, non-digits
for n in (0, 1, 15, 17, 24, 31, 33, 47, 49, 95, 97):
    put("seedqr", bytes(random.randrange(256) for _ in range(n)))
    put("seedqr", ("1" * n).encode())
put("seedqr", b"2048" * 12)
put("seedqr", b"0000" * 11 + b"000\x00")
put("seedqr", b" " + b"0" * 47)

# address: sign-message QRs and odd URIs
for a in ["signmessage m/84h/0h/0h/0/0 ascii:hello", "signmessage m/84h/0h/0h/0/0 ascii:", "signmessage  ascii:x",
          "signmessage m/ ascii:x", "signmessage", "bitcoin:", "bitcoin:?amount=1", "bitcoin:bc1q?amount=",
          "bitcoin:bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu?amount=21000000.00000001&label=x",
          "bitcoin:bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu?amount=.&amount=1",
          "bc1q", "tb1q" + "q" * 58, "BC1Q" + "Q" * 91, ""]:
    put("address", a.encode())

# ---- multisig: [mode][...] for fuzz_multisig
ms_desc = cstr("MS_DESCRIPTOR", msfixt)
def ms(mode, data): put("multisig", bytes([mode]) + data)
keys = re.findall(r"\[([0-9a-f]{8})((?:/\d+h?)*)\](tpub\w+)/<0;1>/\*", ms_desc)
assert len(keys) == 3, keys
def desc(ks, m=2, children="/<0;1>/*"):
    return "wsh(sortedmulti(%d,%s))" % (m, ",".join(
        (f"[{fp}{path}]" if fp is not None else "") + xpub + children for fp, path, xpub in ks))
k0, k1, k2 = keys
text_variants = [
    ms_desc, ms_desc.split("#")[0], desc(keys), desc(keys, children="/0/*"), desc(keys, children="/{0,1}/*"),
    desc(keys, children="/<0;1;2>/*"), desc(keys, children="/*"), desc(keys, children=""),
    desc([k0, (None, "", k1[2]), k2]),                     # #1045: a cosigner without key origin
    desc([(None, "", k0[2]), k1, k2]),                     # our own key without origin
    desc([(k0[0], "", k0[2]), k1, k2]),                    # #1074: origin with an empty path
    desc([k0, (k1[0], "", k1[2]), k2]),
    desc([(k0[0], "/", k0[2]), k1, k2]), desc([(k0[0], "//48h", k0[2]), k1, k2]),
    desc([(k0[0], "/48h/1h/0h/2h/0/0/0/0/0", k0[2]), k1, k2]),   # longer than MAX_PATH_LEN
    desc([(k0[0], "/2147483648", k0[2]), k1, k2]), desc([(k0[0], "/4294967295h", k0[2]), k1, k2]),
    desc(keys, m=0), desc(keys, m=4), desc(keys, m=-1), desc([k0, k0, k1]), desc([k0] * 16, m=1),
    desc([k0]), "wsh(sortedmulti(2,))", "wsh(sortedmulti())", "wsh()", "wsh(", "wsh(multi(2,%s))" % desc(keys)[18:-2],
    "sh(wsh(sortedmulti(2,%s)))" % desc(keys)[18:-2], "wsh(sortedmulti(2,%s))" % ",".join(k[2] for k in keys),
    ms_desc.replace("tpub", "xpub"), ms_desc.replace("tpub", "tprv"), ms_desc[:-1] + "x",
    ms_desc.replace("/<0;1>/*", "/<0;1>/*h"), "  \n\t" + ms_desc + "\r\n", ms_desc + "\x00junk",
    "wsh(sortedmulti(2," + "[" * 200 + "))", "wsh(" * 300,
    "signmessage m/48h/1h/0h/2h/0/0 ascii:hello",          # #1058/#1062: sign-message is not supported
]
for t in text_variants:
    ms(0, t.encode())

# crypto-output CBOR built from the same keys (BCR-2020-010)
B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
def b58check(s):
    n = 0
    for ch in s: n = n * 58 + B58.index(ch)
    raw = n.to_bytes(82, "big")
    return raw[:78]
def keypath(comps, fp=None, extra=()):
    pairs = [(1, cb_arr([x for c in comps for x in (cb_uint(c & ~H), cb_bool(c & H))]))]
    if fp is not None: pairs.append((2, cb_uint(fp)))
    return cb_tag(304, cb_map(pairs + list(extra)))
def path_of(s): return [int(c.rstrip("h")) | (H if c.endswith("h") else 0) for c in s.split("/") if c]
def hdkey(k, origin="full", children="0/*", keylen=33, cc_len=32, network=True):
    fp, path, xpub = k
    ser = b58check(xpub)
    pairs = [(3, cb_bytes(ser[45:78][:keylen] + bytes(max(0, keylen - 33)))), (4, cb_bytes(ser[13:45][:cc_len]))]
    if network: pairs.append((5, cb_tag(305, cb_map([(2, cb_uint(1))]))))
    comps = path_of(path)
    if origin == "full": pairs.append((6, keypath(comps, int(fp, 16))))
    elif origin == "empty": pairs.append((6, keypath([], int(fp, 16))))       # #1074
    elif origin == "nofp": pairs.append((6, keypath(comps)))
    elif origin == "long": pairs.append((6, keypath(comps + [0] * 6, int(fp, 16))))
    elif origin == "odd": pairs.append((6, cb_tag(304, cb_map([(1, cb_arr([cb_uint(48)])), (2, cb_uint(int(fp, 16)))]))))
    # origin == "none": #1045, no key origin at all
    if children == "0/*": pairs.append((7, cb_tag(304, cb_map([(1, cb_arr([cb_uint(0), cb_bool(False), cb_arr([]), cb_bool(False)]))]))))
    elif children == "<0;1>/*": pairs.append((7, cb_tag(304, cb_map([(1, cb_arr([cb_arr([cb_uint(0), cb_bool(False), cb_uint(1), cb_bool(False)]), cb_arr([]), cb_bool(False)]))]))))
    elif children == "1/*": pairs.append((7, cb_tag(304, cb_map([(1, cb_arr([cb_uint(1), cb_bool(False), cb_arr([]), cb_bool(False)]))]))))
    pairs.append((8, cb_uint(int.from_bytes(ser[5:9], "big"))))
    return cb_tag(303, cb_map(pairs))
def output(hdkeys, m=2, n=None, outer=401, inner=407, trailing=b""):
    return cb_tag(outer, cb_tag(inner, cb_map([(1, cb_uint(m)), (2, cb_head(4, len(hdkeys) if n is None else n) + b"".join(hdkeys))]))) + trailing
good = [hdkey(k) for k in keys]
cbor_variants = [
    output(good), output([hdkey(k, children=None) for k in keys]), output([hdkey(k, children="<0;1>/*") for k in keys]),
    output([good[0], hdkey(k1, origin="none"), good[2]]),      # #1045
    output([hdkey(k0, origin="none")] + good[1:]),
    output([hdkey(k0, origin="empty")] + good[1:]),            # #1074
    output([good[0], hdkey(k1, origin="empty"), good[2]]),
    output([hdkey(k0, origin="nofp")] + good[1:]), output([hdkey(k0, origin="long")] + good[1:]),
    output([hdkey(k0, origin="odd")] + good[1:]), output([hdkey(k0, children="1/*")] + good[1:]),
    output([hdkey(k0, keylen=32)] + good[1:]), output([hdkey(k0, keylen=65)] + good[1:]),
    output([hdkey(k0, cc_len=31)] + good[1:]), output([hdkey(k0, network=False)] + good[1:]),
    output(good, m=0), output(good, m=4), output(good, n=4), output(good, n=0), output([good[0]] * 16, m=1),
    output(good, trailing=b"\x00"), output(good, outer=400), output(good, inner=406), output(good[:2], n=3),
    cb_tag(401, cb_tag(407, cb_map([(1, cb_uint(2))]))), b"", b"\xd9\x01\x91",
    cb_tag(401, cb_tag(407, cb_map([(1, cb_uint(2)), (2, b"\x9f" + b"".join(good) + b"\xff")]))),   # indefinite array
    output([cb_tag(303, cb_map([(9, b"\x81" * 40 + b"\x00")]))] + good[1:]),                         # deep unknown field
]
for c in cbor_variants:
    ms(1, c)
for name in ("MS_UR_OUTPUT", "SPARROW_UR_OUTPUT"):
    body = cstr(name, msfixt).rsplit("/", 1)[1]
    ms(1, bw_decode(body))
    put("ur", b"\x00" + cstr(name, msfixt).encode())

# PSBTs with tweaked metadata, for fuzz_psbt (single-sig) and fuzz_multisig mode 2
def psbt_variants(raw, our_fp):
    def tweak(f):
        p = PSBT.parse(raw)
        f(p)
        return p.serialize()
    def ours(scope):
        return [k for k, v in scope.bip32_derivations.items() if v.fingerprint == our_fp]
    def set_path(scope, path):
        for k in ours(scope): scope.bip32_derivations[k] = DerivationPath(our_fp, path)
    def outs(p, n):   # n outputs: copies of the last, paid for out of the first
        while len(p.outputs) < n:
            o = copy.deepcopy(p.outputs[-1]); o.value = 546; p.outputs[0].value -= 546; p.outputs.append(o)
    def claims(scope, n):
        k = ours(scope)[0]
        for i in range(n):
            scope.bip32_derivations[PrivateKey((i + 1).to_bytes(32, "big")).get_public_key()] = scope.bip32_derivations[k]
    def change(p):   # the output claiming our key; one of them made to, if none does
        for o in p.outputs:
            if ours(o): return o
        k = ours(p.inputs[0])[0]
        p.outputs[0].bip32_derivations[k] = p.inputs[0].bip32_derivations[k]
        return p.outputs[0]
    yield raw
    yield tweak(lambda p: set_path(p.inputs[0], []))                    # #1074/#1076: empty path for our key
    yield tweak(lambda p: set_path(change(p), []))
    yield tweak(lambda p: set_path(p.inputs[0], [H + 84]))
    yield tweak(lambda p: set_path(p.inputs[0], list(range(11))))      # past MAX_PATH_LEN
    yield tweak(lambda p: [set_path(s, []) for s in p.inputs + p.outputs])
    yield tweak(lambda p: p.inputs[0].bip32_derivations.clear())
    yield tweak(lambda p: [d for d in (p.inputs[0].bip32_derivations,) if [d.pop(k) for k in list(d) if d[k].fingerprint != our_fp]])
    yield tweak(lambda p: claims(p.inputs[0], 3))                       # MAX_CLAIMS 4: at and past
    yield tweak(lambda p: claims(p.inputs[0], 4))
    yield tweak(lambda p: claims(change(p), 4))
    yield tweak(lambda p: outs(p, 8))                                   # PSBT_MAX_OUTPUTS: at and past
    yield tweak(lambda p: outs(p, 9))
    yield tweak(lambda p: setattr(p.outputs[-1], "value", 0))
    yield tweak(lambda p: setattr(p.inputs[0], "witness_script", None))
    yield tweak(lambda p: setattr(p.inputs[0], "witness_utxo", None))
    yield tweak(lambda p: setattr(p.inputs[0], "sighash_type", 2))
    yield tweak(lambda p: setattr(p.inputs[0], "sighash_type", 0x83))
    yield tweak(lambda p: p.inputs.append(copy.deepcopy(p.inputs[0])))
ss_raw = base64.b64decode(single_b64)
for v in psbt_variants(ss_raw, bytes.fromhex("8b218e81")):
    put("psbt", b"\x00" + v)
    put("psbt", b"\x01" + base64.b64encode(v))
for name in ("MS_PSBT", "MS_PSBT_TAMPERED", "MS_PSBT_COSIGNED"):
    for v in psbt_variants(base64.b64decode(cstr(name, msfixt)), bytes.fromhex("0fb882ff")):
        ms(2, b"\x00" + v)
    ms(2, b"\x01" + cstr(name, msfixt).encode())
ms(2, b"\x00" + ss_raw)
ms(2, b"\x00psbt\xff")
ms(2, b"\x00psbt\xff\x00")

for a in ["MS_ADDR_RECEIVE_0", "MS_ADDR_CHANGE_4", "SPARROW_ADDR_RECEIVE_0"]:
    ms(3, cstr(a, msfixt).encode())
    ms(3, ("bitcoin:" + cstr(a, msfixt).upper() + "?amount=0.5").encode())
ms(3, b"bc1q" + b"q" * 58)
ms(3, b"signmessage m/48h/1h/0h/2h/0/0 ascii:hi")

# inputs that once crashed a fuzzer, kept so every run starts from them
for d in sorted((FW / "fuzz" / "regressions").iterdir()):
    for f in sorted(d.iterdir()):
        put(d.name, f.read_bytes())

for d in sorted(CORPUS.iterdir()):
    print(d.name, len(list(d.iterdir())))
