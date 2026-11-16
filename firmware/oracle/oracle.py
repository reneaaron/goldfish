#!/usr/bin/env python3
"""Differential test of the firmware's PSBT review and signing against embit.

Generates random single-sig PSBTs (honest ones and attacks), runs them through
oracle/out/psbt_cli (the firmware code, natively), and checks:

  hard  what the screen shows matches the transaction: amounts, addresses,
        change/self/spend split, fee; bad PSBTs are refused; every signature
        is valid for exactly the reviewed transaction.
  soft  policy issues a careful signer would warn about (change in another
        account, change far beyond the gap limit, mixed networks). Counted,
        not fatal.

Usage (from firmware/):
  oracle/build.sh
  uv run --with embit oracle/oracle.py [-n 2000] [--seed 1] [--show 3]
"""
import argparse, collections, json, os, random, subprocess, sys
from pathlib import Path

from embit import bip32, bip39, ec, script
from embit.networks import NETWORKS
from embit.psbt import PSBT, DerivationPath
from embit.script import Script
from embit.transaction import Transaction, TransactionInput, TransactionOutput

H = 0x80000000
CLI = Path(__file__).resolve().parent / "out" / "psbt_cli"
OK, PARSE_FAILED, NOT_OUR_SEED, UNSUPPORTED, MISSING_AMOUNTS, SUSPICIOUS = range(6)
MAX_OUTPUTS = 8
MAX_MONEY = 21_000_000 * 100_000_000


class Case:
    def __init__(self, rng):
        self.rng = rng
        self.mnemonic = bip39.mnemonic_from_bytes(bytes(rng.getrandbits(8) for _ in range(rng.choice([16, 32]))))
        self.root = bip32.HDKey.from_seed(bip39.mnemonic_to_seed(self.mnemonic))
        self.fp = self.root.my_fingerprint
        self.coin = rng.choice([0, 1])
        self.tags = []                 # what the generator did, for reports
        self.must_reject = []          # reasons a correct signer refuses this
        self.soft = []                 # policy warnings expected

    def ours(self, path):
        return self.root.derive(path).key.get_public_key()

    def foreign_pub(self):
        return ec.PrivateKey(bytes(self.rng.getrandbits(8) for _ in range(32))).get_public_key()

    def amount(self):
        r = self.rng.random()
        if r < 0.03: return self.rng.choice([0, 1, MAX_MONEY, MAX_MONEY + 1, 2**63 - 1, 2**64 - 1])
        if r < 0.3: return self.rng.randint(1, 10_000)
        return self.rng.randint(546, 5 * 10**8)


def foreign_script(c, kind):
    pub = c.foreign_pub()
    return {
        "p2wpkh": lambda: script.p2wpkh(pub),
        "p2tr": lambda: script.p2tr(pub),
        "p2pkh": lambda: script.p2pkh(pub),
        "p2sh": lambda: script.p2sh(script.p2wpkh(pub)),
        "p2wsh": lambda: script.p2wsh(script.multisig(1, [pub])),
        "op_return": lambda: Script(b"\x6a\x04test"),
    }[kind]()


def make_case(rng):
    c = Case(rng)
    acct = [84 + H, c.coin + H, 0 + H]
    n_in = rng.randint(1, 4)
    n_out = rng.choice([1, 2, 2, 3, 4, 8, 9])
    vin, utxos, in_derivs = [], [], []
    in_total = 0
    for i in range(n_in):
        vin.append(TransactionInput(bytes(rng.getrandbits(8) for _ in range(32)), rng.randint(0, 3)))
        amt = c.amount()
        in_total += amt
        path = acct + [rng.choice([0, 1]), rng.randint(0, 50)]
        kind = rng.random()
        if kind < 0.05:      # input from someone else's wallet
            pub = c.foreign_pub()
            utxos.append(TransactionOutput(amt, script.p2wpkh(pub)))
            in_derivs.append({pub: DerivationPath(b"\xde\xad\xbe\xef", path)})
            c.tags.append("foreign-input"); c.must_reject.append("input not ours")
        elif kind < 0.09:    # our input, but taproot
            pub = c.ours(path)
            utxos.append(TransactionOutput(amt, script.p2tr(pub)))
            in_derivs.append({pub: DerivationPath(c.fp, path)})
            c.tags.append("taproot-input"); c.must_reject.append("input not p2wpkh")
        elif kind < 0.13:    # claims our fingerprint, but the script is someone else's
            pub = c.ours(path)
            utxos.append(TransactionOutput(amt, foreign_script(c, "p2wpkh")))
            in_derivs.append({pub: DerivationPath(c.fp, path)})
            c.tags.append("input-key-mismatch"); c.must_reject.append("input script isn't our key's")
        elif kind < 0.17:    # other coin type than the rest
            p2 = [84 + H, (1 - c.coin) + H, H] + path[3:]
            pub = c.ours(p2)
            utxos.append(TransactionOutput(amt, script.p2wpkh(pub)))
            in_derivs.append({pub: DerivationPath(c.fp, p2)})
            c.tags.append("mixed-network-input"); c.soft.append("inputs from mixed networks")
        else:
            pub = c.ours(path)
            utxos.append(TransactionOutput(amt, script.p2wpkh(pub)))
            in_derivs.append({pub: DerivationPath(c.fp, path)})
        if amt == 0:
            c.must_reject.append("input amount 0")

    vout, out_derivs, expect = [], [], []
    for i in range(n_out):
        amt = c.amount()
        r = rng.random()
        deriv = {}
        if r < 0.45:
            kind = rng.choice(["p2wpkh", "p2tr", "p2pkh", "p2sh", "p2wsh", "op_return"])
            sc = foreign_script(c, kind)
            role = "spend"
        elif r < 0.65:       # honest change
            path = acct + [1, rng.randint(0, 50)]
            pub = c.ours(path); sc = script.p2wpkh(pub)
            deriv = {pub: DerivationPath(c.fp, path)}; role = "change"
        elif r < 0.72:       # send to own receive address
            path = acct + [0, rng.randint(0, 50)]
            pub = c.ours(path); sc = script.p2wpkh(pub)
            deriv = {pub: DerivationPath(c.fp, path)}; role = "self"
        elif r < 0.80:       # fake change: claims our path, pays someone else
            path = acct + [1, rng.randint(0, 50)]
            pub = c.ours(path); sc = foreign_script(c, "p2wpkh")
            deriv = {pub: DerivationPath(c.fp, path)}; role = "suspicious"
            c.tags.append("fake-change"); c.must_reject.append("fake change")
        elif r < 0.86:       # real change, but in another account
            path = [84 + H, c.coin + H, rng.randint(1, 1000) + H, 1, rng.randint(0, 50)]
            pub = c.ours(path); sc = script.p2wpkh(pub)
            deriv = {pub: DerivationPath(c.fp, path)}; role = "change"
            c.tags.append("change-other-account"); c.soft.append("change in another account")
        elif r < 0.92:       # real change, far beyond any gap limit
            path = acct + [1, rng.randint(10_000, H - 1)]
            pub = c.ours(path); sc = script.p2wpkh(pub)
            deriv = {pub: DerivationPath(c.fp, path)}; role = "change"
            c.tags.append("change-huge-index"); c.soft.append("change index beyond gap limit")
        elif r < 0.96:       # real change on the other network's coin type / other purpose
            path = [rng.choice([44, 49, 84, 86]) + H, (1 - c.coin) + H, H, 1, rng.randint(0, 50)]
            pub = c.ours(path); sc = script.p2wpkh(pub)
            deriv = {pub: DerivationPath(c.fp, path)}; role = "suspicious"
            c.tags.append("change-odd-path"); c.must_reject.append("change outside the BIP84 account")
        else:                # our key as a non-p2wpkh output, claimed as ours
            path = acct + [1, rng.randint(0, 50)]
            pub = c.ours(path); sc = script.p2pkh(pub)
            deriv = {pub: DerivationPath(c.fp, path)}; role = "suspicious"
            c.tags.append("change-wrong-script"); c.must_reject.append("change with non-p2wpkh script")
        vout.append(TransactionOutput(amt, sc))
        out_derivs.append(deriv)
        expect.append({"role": role, "amount": amt, "script": sc})

    out_total = sum(o.value for o in vout)
    if n_out > MAX_OUTPUTS: c.must_reject.append("too many outputs")
    if out_total > in_total: c.must_reject.append("outputs exceed inputs")
    if in_total > MAX_MONEY or out_total > MAX_MONEY:
        c.tags.append("over-21M"); c.must_reject.append("amounts above 21M BTC")

    tx = Transaction(version=2, vin=vin, vout=vout, locktime=0)
    p = PSBT(tx)
    for i in range(n_in):
        p.inputs[i].witness_utxo = utxos[i]
        p.inputs[i].bip32_derivations.update(in_derivs[i])
    for i in range(n_out):
        p.outputs[i].bip32_derivations.update(out_derivs[i])
    try:
        b64 = p.to_string()
    except Exception as e:   # embit refuses some values (e.g. > 2^64)
        return None
    c.psbt, c.tx, c.utxos, c.expect = p, tx, utxos, expect
    c.in_total, c.out_total = in_total, out_total
    c.b64 = b64
    return c


def check(c, got):
    hard, soft = [], []
    err = got["error"]
    if c.must_reject:
        if err == OK:
            hard.append("accepted although: " + ", ".join(sorted(set(c.must_reject))))
        return hard, soft
    if err != OK:
        hard.append(f"refused a valid PSBT with error {err}")
        return hard, soft

    net = NETWORKS["test" if got["testnet"] else "main"]
    want_net = NETWORKS["test" if c.coin else "main"]
    if net is not want_net:
        soft.append("display network differs from the first input's coin type")
    if len(got["outputs"]) != len(c.expect):
        hard.append(f"shows {len(got['outputs'])} outputs, tx has {len(c.expect)}")
        return hard, soft
    sums = collections.Counter()
    for i, (g, e) in enumerate(zip(got["outputs"], c.expect)):
        if g["amount"] != e["amount"]:
            hard.append(f"output {i}: shows {g['amount']} sats, pays {e['amount']}")
        try:
            shown = Script.from_address(g["address"])
            if shown.data != e["script"].data:
                hard.append(f"output {i}: shows address {g['address']} for a different script")
        except Exception:
            if e["script"].script_type() is not None:
                hard.append(f"output {i}: standard {e['script'].script_type()} output shown as {g['address']!r}")
        role = "change" if g["change"] else "self" if g["self"] else "suspicious" if g["suspicious"] else "spend"
        if role != e["role"]:
            hard.append(f"output {i}: shown as {role}, really {e['role']}")
        sums[e["role"]] += e["amount"]
    for k in ("spend", "self", "change"):
        if got[k] != sums[k]:
            hard.append(f"{k} total shows {got[k]}, really {sums[k]}")
    if got["input_total"] != c.in_total:
        hard.append(f"input total shows {got['input_total']}, really {c.in_total}")
    if got["fee"] != c.in_total - c.out_total:
        hard.append(f"fee shows {got['fee']}, really {c.in_total - c.out_total}")

    # Signatures: valid ECDSA/SIGHASH_ALL over exactly this transaction.
    if not got["signed"]:
        hard.append("accepted but produced no signed PSBT")
        return hard, soft
    signed = PSBT.from_string(got["signed"])
    if signed.tx.txid() != c.tx.txid():
        hard.append("signed PSBT is a different transaction")
        return hard, soft
    for i, utxo in enumerate(c.utxos):
        sigs = signed.inputs[i].partial_sigs
        mine = [pub for pub in c.psbt.inputs[i].bip32_derivations]
        if not sigs:
            hard.append(f"input {i}: not signed")
            continue
        for pub, sig in sigs.items():
            if pub not in mine:
                hard.append(f"input {i}: signed with a key not in its derivations")
                continue
            if sig[-1] != 1:
                hard.append(f"input {i}: sighash type {sig[-1]} instead of ALL")
            msg = c.tx.sighash_segwit(i, script.p2pkh(pub), utxo.value)
            valid = pub.verify(ec.Signature.parse(sig[:-1]), msg)
            matches_script = script.p2wpkh(pub).data == utxo.script_pubkey.data
            if not valid:
                hard.append(f"input {i}: signature doesn't verify")
            elif not matches_script:
                soft.append("signed an input whose script isn't ours")
    return hard, soft


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-n", type=int, default=1000)
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--show", type=int, default=2, help="examples to print per finding")
    ap.add_argument("--save", default=None, help="directory to save failing PSBTs")
    a = ap.parse_args()
    seed = a.seed if a.seed is not None else int.from_bytes(os.urandom(4), "big")
    rng = random.Random(seed)
    print(f"seed {seed}, {a.n} cases")

    cases = []
    while len(cases) < a.n:
        c = make_case(rng)
        if c: cases.append(c)
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
    hard_found, soft_found = collections.defaultdict(list), collections.defaultdict(list)
    results, todo = [], list(cases)
    while todo:   # restart psbt_cli after a crash, recording the crashing case
        stdin = "".join(f"{c.mnemonic}\t{c.b64}\n" for c in todo)
        proc = subprocess.run([str(CLI)], input=stdin, capture_output=True, text=True, env=env)
        lines = [l for l in proc.stdout.splitlines() if l.endswith("}")]   # a crash leaves a partial line
        results += zip(todo, lines)
        if len(lines) == len(todo):
            break
        bad = todo[len(lines)]
        summary = next((l for l in proc.stderr.splitlines() if l.startswith("SUMMARY")), "crashed")
        hard_found["CRASH " + summary.split(" in ")[0]].append((bad, summary, None))
        todo = todo[len(lines) + 1:]

    for c, line in results:
        got = json.loads(line)
        hard, soft = check(c, got)
        for h in hard:
            key = h.split(":")[0] if h.startswith(("output", "input")) else h
            key = " ".join(w for w in key.split() if not w.isdigit())
            hard_found[key].append((c, h, got))
        for s in set(soft) | set(c.soft):
            if got["error"] == OK:
                soft_found[s].append(c)

    print(f"\nHARD mismatches ({sum(map(len, hard_found.values()))} cases)")
    for k, v in sorted(hard_found.items(), key=lambda kv: -len(kv[1])):
        print(f"  {len(v):5d}  {k}")
        for c, h, got in v[:a.show]:
            print(f"         - {h}  [tags: {', '.join(c.tags) or 'none'}]")
    print(f"\nSOFT: accepted for signing despite ({sum(map(len, soft_found.values()))})")
    for k, v in sorted(soft_found.items(), key=lambda kv: -len(kv[1])):
        print(f"  {len(v):5d}  {k}")
    if a.save:
        d = Path(a.save); d.mkdir(parents=True, exist_ok=True)
        for k, v in hard_found.items():
            for j, (c, h, got) in enumerate(v[:a.show]):
                (d / f"{abs(hash(k)) % 10**8}_{j}.txt").write_text(f"{h}\n{c.mnemonic}\n{c.b64}\n")
        for k, v in soft_found.items():
            for j, c in enumerate(v[:a.show]):
                (d / f"soft_{abs(hash(k)) % 10**8}_{j}.txt").write_text(f"{k}\n{c.mnemonic}\n{c.b64}\n")
    sys.exit(1 if hard_found else 0)


if __name__ == "__main__":
    main()
