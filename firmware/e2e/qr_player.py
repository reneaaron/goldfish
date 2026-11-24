"""A stand-in for Sparrow's animated QR window, for testing the board's camera.

Writes one self-contained HTML page: the test SeedQR (load it on the board
first), then testnet PSBTs for that seed in a few sizes, as animated UR at
Sparrow's three densities, with its speed and a size slider. Hold the board up
to the screen; a long press on the scan preview shows its scan speed.

    uv run --with-requirements e2e/requirements.txt python e2e/qr_player.py --out /tmp/qr_player.html
"""
import argparse
import html
import json

import qrcode

from hostwallet import Seed, Wallet, psbt_parts, seedqr
from embit import bip39

# sim/samples/testseed_height_demise_standard_12word.png: SeedSigner's public test seed
SEEDQR = "085304661920185008241041073712260976035218490601"
MNEMONIC = " ".join(bip39.WORDLIST[int(SEEDQR[i:i + 4])] for i in range(0, len(SEEDQR), 4))
STRANGER = Seed("zoo " * 11 + "wrong")

# Sparrow's QRDensity: max UR fragment length in bytes
DENSITIES = {"Low": 80, "Medium": 400, "High": 1000, "128 B (not in Sparrow)": 128}
# (inputs, payments): Sparrow's own PSBTs carry the full previous transactions too
SIZES = {"1 in, 1 out": (1, 1), "2 in, 3 out": (2, 3), "4 in, 8 out": (4, 8),
         # the limits: 16 outputs (15 payments and change); at Low, 128 fragments of 80 bytes;
         # and the most the heap signs (past that, "Too big for this signer")
         "1 in, 15 out": (1, 15), "max at Low: 45 in, 1 out": (45, 1), "max: 65 in, 1 out": (65, 1)}


def svg(text):
    q = qrcode.QRCode(error_correction=qrcode.constants.ERROR_CORRECT_L, border=4)
    q.add_data(text)
    q.make(fit=True)
    m = q.get_matrix()
    path = "".join("M%d %dh1v1h-1z" % (x, y) for y, row in enumerate(m) for x, on in enumerate(row) if on)
    return '<svg viewBox="0 0 %d %d" shape-rendering="crispEdges"><path d="%s"/></svg>' % (len(m), len(m), path)


def psbts():
    wallet = Wallet.singlesig(Seed(MNEMONIC).key("84h/1h/0h"))
    stranger = Wallet.singlesig(STRANGER.key("84h/1h/0h"))
    for name, (n_in, n_out) in SIZES.items():
        coins = [wallet.fund(i, 500_000) for i in range(n_in)]
        pays = [(stranger.address(i), 20_000) for i in range(n_out)]
        yield name, wallet.create_psbt(coins, pays, fee=200 * n_in + 50 * n_out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="qr_player.html")
    out = ap.parse_args().out

    data = {"seed": svg(SEEDQR), "mnemonic": MNEMONIC, "psbts": {}}
    for name, psbt in psbts():
        size = len(psbt.serialize())
        data["psbts"][name] = {"bytes": size, "densities": {}}
        for dname, frag in DENSITIES.items():
            gen = psbt_parts(psbt, fragment_len=frag)
            first = next(gen)
            # "ur:crypto-psbt/3-5/..." is one of 5 parts; a single part has no counter
            seq = first.split("/")[1]
            n = int(seq.split("-")[1]) if "-" in seq else 1
            # Sparrow shows the pure parts, then fountain-coded ones without end: loop a stretch of both
            parts = [first] + [next(gen) for _ in range(0 if n == 1 else min(3 * n + 5, 2 * n))]
            data["psbts"][name]["densities"][dname] = {"parts": n, "frames": [svg(p) for p in parts],
                                                       "chars": len(first)}
    with open(out, "w") as f:
        f.write(PAGE.replace("__DATA__", json.dumps(data)).replace("__MNEMONIC__", html.escape(MNEMONIC)))
    print(out)


PAGE = """<!doctype html><html><head><meta charset="utf-8"><title>QR player</title>
<style>
body{margin:0;background:#fff;color:#222;font:14px system-ui,sans-serif;display:flex;flex-direction:column;align-items:center}
.bar{display:flex;flex-wrap:wrap;gap:10px 16px;align-items:center;padding:10px 16px;max-width:900px}
label{display:flex;gap:6px;align-items:center}
#qr{width:var(--s,420px);height:var(--s,420px);margin:8px}#qr svg{width:100%;height:100%}
#info{font-variant-numeric:tabular-nums;color:#555}
small{color:#777}
</style></head><body>
<div class="bar">
 <label><input type="radio" name="what" value="seed" checked> Seed</label>
 <label><input type="radio" name="what" value="psbt"> PSBT</label>
 <label>size <select id="size"></select></label>
 <label>density <select id="dens"></select></label>
 <label>ms/frame <input id="ms" type="number" value="200" min="20" step="10" style="width:5em"></label>
 <label>QR size <input id="px" type="range" min="150" max="900" value="420"></label>
 <button id="pause">Pause</button>
</div>
<div id="info"></div>
<div id="qr"></div>
<small>Test seed (never real funds): __MNEMONIC__</small>
<script>
const D=__DATA__;
const $=id=>document.getElementById(id);
for(const k in D.psbts)$('size').add(new Option(k+' ('+D.psbts[k].bytes+' B)',k));
for(const k in D.psbts[Object.keys(D.psbts)[0]].densities)$('dens').add(new Option(k,k));
$('dens').value='Low';
let i=0,timer=null,paused=false;
function cur(){return D.psbts[$('size').value].densities[$('dens').value]}
function show(){
  const seed=document.querySelector('[name=what]:checked').value==='seed';
  if(seed){$('qr').innerHTML=D.seed;$('info').textContent='SeedQR, 12 words';return}
  const c=cur();i%=c.frames.length;$('qr').innerHTML=c.frames[i];
  $('info').textContent=(c.parts>1?'part '+(i+1)+' of a loop of '+c.frames.length+', '+c.parts+' fragments':'one static part')+', '+c.chars+' characters';
  i++;
}
// each frame lasts 75-125% of ms/frame: at a steady rate a camera that samples at its own steady
// rate locks in step with the frames and misses the same ones on every pass
function tick(){show();const ms=+$('ms').value||200;timer=setTimeout(tick,ms*(0.75+Math.random()/2))}
function restart(){clearTimeout(timer);i=0;if(paused)show();else tick()}
document.querySelectorAll('select,input[name=what],#ms').forEach(e=>e.onchange=restart);
// picking a size or density means the PSBT, not the seed
$('size').onchange=$('dens').onchange=()=>{document.querySelector('[value=psbt]').checked=true;restart()};
$('px').oninput=()=>document.body.style.setProperty('--s',$('px').value+'px');
$('pause').onclick=()=>{paused=!paused;$('pause').textContent=paused?'Play':'Pause';restart()};
restart();
</script></body></html>"""

if __name__ == "__main__":
    main()
