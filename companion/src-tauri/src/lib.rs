//! The companion's wallet logic, without any UI: the watch-only wallet built
//! from Goldfish's Connect wallet QR, PSBTs as animated UR QR codes out, the
//! signed PSBT back in, finalize and broadcast. The seed never comes here.

pub mod camera;
pub mod firmware;
pub mod p2p;
pub mod tor;

use std::sync::Arc;

use base64::{engine::general_purpose::STANDARD as B64, Engine};
use bdk_electrum::electrum_client::{self, ElectrumApi};
use bdk_electrum::BdkElectrumClient;
use bdk_esplora::{esplora_client, EsploraExt};
use bdk_wallet::bitcoin::{
    absolute, base58, bip32::Xpub, transaction, Address, Amount, BlockHash, FeeRate, Network,
    OutPoint, Psbt, Transaction, TxIn, TxOut, Txid,
};
use bdk_wallet::chain::spk_client::{FullScanRequest, FullScanResponse};
use bdk_wallet::chain::{BlockId, ConfirmationBlockTime, TxUpdate};
use bdk_wallet::{miniscript, KeychainKind, SignOptions, Update, Wallet};
use serde::{Deserialize, Serialize};

pub type Res<T> = Result<T, String>;

fn err<E: std::fmt::Display>(e: E) -> String {
    e.to_string()
}

// ------------------------------------------------------------ the account

/// The wallet the companion watches, as a two-path descriptor (`/<0;1>/*`,
/// with checksum): single-sig `wpkh` from Goldfish's Connect wallet key, or
/// a `wsh(sortedmulti)` multisig that Goldfish is one cosigner of.
#[derive(Clone, Debug)]
pub struct Account {
    pub descriptor: String,
    pub network: Network,
    /// one per key, in the descriptor's order
    pub fingerprints: Vec<String>,
    /// signatures a spend needs: 1 for single-sig
    pub threshold: usize,
    /// what to save to reopen it: the key export, or the descriptor
    pub saved: String,
}

const XPUB: [u8; 4] = [0x04, 0x88, 0xB2, 0x1E];
const ZPUB: [u8; 4] = [0x04, 0xB2, 0x47, 0x46];
const ZPUB_MS: [u8; 4] = [0x02, 0xAA, 0x7E, 0xD3]; // Zpub: P2WSH multisig, mainnet
const TPUB: [u8; 4] = [0x04, 0x35, 0x87, 0xCF];
const VPUB: [u8; 4] = [0x04, 0x5F, 0x1C, 0xF6];
const VPUB_MS: [u8; 4] = [0x02, 0x57, 0x54, 0x83]; // Vpub: P2WSH multisig, testnet

/// One `[fingerprint/path]key` with its key in xpub/tpub spelling.
struct Key {
    fingerprint: String,
    path: String,
    xpub: Xpub,
    mainnet: bool,
}

impl Key {
    fn parse(text: &str) -> Res<Key> {
        let text = text.trim();
        let bad = || format!("not a key with its origin, like [fingerprint/48h/1h/0h/2h]Vpub…: {:?}", short(text));
        let rest = text.strip_prefix('[').ok_or_else(|| {
            format!("every key needs its [fingerprint/path] in front, so the signers can find their own: {:?}", short(text))
        })?;
        let (origin, key) = rest.split_once(']').ok_or_else(bad)?;
        let (fingerprint, path) = origin.split_once('/').ok_or_else(bad)?;
        if fingerprint.len() != 8 || !fingerprint.chars().all(|c| c.is_ascii_hexdigit()) {
            return Err(bad());
        }
        // The key, then the children every wallet app spells its own way.
        let (key, children) = key.split_once('/').map(|(k, c)| (k, format!("/{c}"))).unwrap_or((key, String::new()));
        if !["", "/<0;1>/*", "/{0,1}/*", "/0/*"].contains(&children.as_str()) {
            return Err(format!("unexpected key children {children:?}: expected /<0;1>/*"));
        }
        // SLIP-132 zpub/vpub/Zpub/Vpub are the same key under another version.
        let mut raw = base58::decode_check(key).map_err(|_| bad())?;
        if raw.len() != 78 {
            return Err(bad());
        }
        let mainnet = match <[u8; 4]>::try_from(&raw[..4]).unwrap() {
            XPUB | ZPUB | ZPUB_MS => true,
            TPUB | VPUB | VPUB_MS => false,
            _ => return Err("unknown key version (expected xpub, zpub, Zpub, tpub, vpub or Vpub)".into()),
        };
        raw[..4].copy_from_slice(if mainnet { &XPUB } else { &TPUB });
        let xpub = Xpub::decode(&raw).map_err(err)?;
        let path = path.replace('\'', "h");
        // Coin type 0 is mainnet, 1 every test network: it has to agree with the key.
        match path.split('/').nth(1) {
            Some("0h") if !mainnet => return Err("a testnet key on a mainnet path".into()),
            Some("1h") if mainnet => return Err("a mainnet key on a testnet path".into()),
            _ => {}
        }
        Ok(Key { fingerprint: fingerprint.to_lowercase(), path, xpub, mainnet })
    }

    fn spelled(&self) -> String {
        format!("[{}/{}]{}/<0;1>/*", self.fingerprint, self.path, self.xpub)
    }

    /// This spike talks to mainnet or signet.
    fn network(&self) -> Network {
        if self.mainnet { Network::Bitcoin } else { Network::Signet }
    }
}

fn short(s: &str) -> String {
    if s.chars().count() > 40 { format!("{}…", s.chars().take(40).collect::<String>()) } else { s.to_string() }
}

fn with_checksum(desc: &str) -> Res<String> {
    Ok(format!("{desc}#{}", miniscript::descriptor::checksum::desc_checksum(desc).map_err(err)?))
}

/// Goldfish's single-sig Connect wallet key: `[fingerprint/84h/1h/0h]vpub…`
/// (or zpub/xpub/tpub, `'` for `h`).
pub fn parse_key_export(text: &str) -> Res<Account> {
    let key = Key::parse(text)?;
    let parts: Vec<&str> = key.path.split('/').collect();
    if parts.len() == 4 && parts[0] == "48h" {
        return Err("that's a multisig key: link a multisig wallet and add the other cosigners' keys".into());
    }
    if parts.len() != 3 || parts[0] != "84h" || parts[2] != "0h" {
        return Err(format!("single-sig keys are native segwit, 84h/…/0h; got {}", key.path));
    }
    Ok(Account {
        descriptor: with_checksum(&format!("wpkh({})", key.spelled()))?,
        network: key.network(),
        fingerprints: vec![key.fingerprint.clone()],
        threshold: 1,
        saved: text.trim().to_string(),
    })
}

/// A `threshold`-of-n `wsh(sortedmulti)` wallet from its cosigners' keys,
/// each `[fingerprint/path]key` (Goldfish's key from Multisig -> Show my key is one).
pub fn multisig_from_keys(threshold: usize, keys: &[&str]) -> Res<Account> {
    let keys = keys.iter().filter(|k| !k.trim().is_empty()).map(|k| Key::parse(k)).collect::<Res<Vec<_>>>()?;
    let n = keys.len();
    if n < 2 {
        return Err("a multisig wallet needs at least two keys".into());
    }
    if n > 15 {
        return Err("at most 15 keys".into());
    }
    if threshold < 1 || threshold > n {
        return Err(format!("signatures needed must be between 1 and {n}"));
    }
    // wsh(sortedmulti) keys come from BIP48's script type 2: 48h/coin/account/2h
    if let Some(k) = keys.iter().find(|k| {
        let p: Vec<&str> = k.path.split('/').collect();
        p.len() != 4 || p[0] != "48h" || p[3] != "2h"
    }) {
        return Err(format!(
            "The key from {} isn't a multisig key (its path is {}, a multisig key's is 48h/…/2h).",
            k.fingerprint, k.path
        ));
    }
    if keys.iter().any(|k| k.mainnet != keys[0].mainnet) {
        return Err("the keys are on different networks".into());
    }
    for (i, a) in keys.iter().enumerate() {
        if keys[..i].iter().any(|b| b.xpub == a.xpub) {
            return Err(format!("the key of {} is in twice", a.fingerprint));
        }
    }
    let spelled: Vec<String> = keys.iter().map(Key::spelled).collect();
    let descriptor = with_checksum(&format!("wsh(sortedmulti({threshold},{}))", spelled.join(",")))?;
    Ok(Account {
        saved: descriptor.clone(),
        descriptor,
        network: keys[0].network(),
        fingerprints: keys.iter().map(|k| k.fingerprint.clone()).collect(),
        threshold,
    })
}

/// A multisig wallet's output descriptor, as wallet apps show it:
/// `wsh(sortedmulti(2,[…]xpub/<0;1>/*,…))#checksum`, children spelled
/// `<0;1>`, `{0,1}`, `0` or left out, checksum optional.
pub fn parse_descriptor(text: &str) -> Res<Account> {
    let text: String = text.split_whitespace().collect();
    let (body, checksum) = text.split_once('#').map(|(b, c)| (b, Some(c))).unwrap_or((&text, None));
    if let Some(c) = checksum {
        if miniscript::descriptor::checksum::desc_checksum(body).map_err(err)? != c {
            return Err("damaged descriptor: its checksum doesn't match. Copy or scan it again".into());
        }
    }
    if body.starts_with("wsh(multi(") {
        return Err("only sortedmulti: Goldfish signs wsh(sortedmulti(…)) wallets".into());
    }
    let inner = body
        .strip_prefix("wsh(sortedmulti(")
        .and_then(|b| b.strip_suffix("))"))
        .ok_or("only native segwit multisig, wsh(sortedmulti(…)), for now")?;
    // `{0,1}` would split at its comma: spell it `<0;1>` first
    let inner = inner.replace("/{0,1}/*", "/<0;1>/*");
    let mut parts = inner.split(',');
    let threshold = parts.next().unwrap_or("").parse::<usize>().map_err(|_| "the descriptor's threshold isn't a number")?;
    multisig_from_keys(threshold, &parts.collect::<Vec<_>>())
}

/// Whatever the user links: a Goldfish key export, or a multisig descriptor.
pub fn parse_wallet(text: &str) -> Res<Account> {
    let t = text.trim();
    if t.starts_with("wsh(") || t.starts_with("wpkh(") || t.contains("multi(") {
        if t.starts_with("wpkh(") {
            return Err("for single-sig, scan or paste Goldfish's Connect wallet key".into());
        }
        return parse_descriptor(t);
    }
    if t.lines().filter(|l| !l.trim().is_empty()).count() > 1 {
        return Err("several keys: that's a multisig wallet, say how many signatures it needs".into());
    }
    parse_key_export(t)
}

impl Account {
    pub fn wallet(&self) -> Res<Wallet> {
        Wallet::create_from_two_path_descriptor(self.descriptor.clone())
            .network(self.network)
            .create_wallet_no_persist()
            .map_err(err)
    }

    pub fn is_multisig(&self) -> bool {
        self.descriptor.starts_with("wsh(")
    }

    /// "2 of 3 multisig", the way Goldfish says it.
    pub fn kind(&self) -> String {
        if self.is_multisig() {
            format!("{} of {} multisig", self.threshold, self.fingerprints.len())
        } else {
            "single-sig".into()
        }
    }
}

// ------------------------------------------------------------ backup

/// One cosigner's key, as a backup lists it.
#[derive(Serialize, Clone, Debug, PartialEq)]
pub struct Cosigner {
    pub fingerprint: String,
    /// `m/48h/1h/0h/2h`
    pub path: String,
    /// xpub (mainnet) or tpub (test networks) spelling
    pub xpub: String,
}

/// What a multisig wallet's backup holds, beside the seeds: everything a
/// wallet app needs to find the coins again, and nothing that spends them.
#[derive(Serialize, Clone, Debug)]
pub struct BackupSheet {
    pub threshold: usize,
    pub cosigners: Vec<Cosigner>,
    /// "Mainnet" or "Signet"
    pub network: String,
    pub descriptor: String,
    pub first_address: String,
    /// the day the sheet was made, `YYYY-MM-DD`
    pub date: String,
}

/// The backup of a multisig `account`, made on `date`.
pub fn backup_sheet(account: &Account, date: &str) -> Res<BackupSheet> {
    if !account.is_multisig() {
        return Err("one signer's seed is its own backup: there's nothing else to save".into());
    }
    let body = account.descriptor.split('#').next().unwrap_or_default();
    let inner = body.strip_prefix("wsh(sortedmulti(").and_then(|b| b.strip_suffix("))")).ok_or("not a sortedmulti descriptor")?;
    let cosigners = inner
        .split(',')
        .skip(1)
        .map(|k| {
            let k = Key::parse(k)?;
            Ok(Cosigner { fingerprint: k.fingerprint, path: format!("m/{}", k.path), xpub: k.xpub.to_string() })
        })
        .collect::<Res<Vec<_>>>()?;
    let first_address = account.wallet()?.peek_address(KeychainKind::External, 0).address.to_string();
    Ok(BackupSheet {
        threshold: account.threshold,
        cosigners,
        network: if account.network == Network::Bitcoin { "Mainnet" } else { "Signet" }.into(),
        descriptor: account.descriptor.clone(),
        first_address,
        date: date.to_string(),
    })
}

fn escape(s: &str) -> String {
    s.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;").replace('"', "&quot;")
}

impl BackupSheet {
    /// The descriptor file: one line, with its checksum, the way wallet apps
    /// import it.
    pub fn text(&self) -> String {
        format!("{}\n", self.descriptor)
    }

    /// A page to print: self-contained, no requests. It looks like Goldfish's
    /// printable cards (`cards/index.html`): the same ink, greys, alert,
    /// boxed strips, numbered steps and storing tips, in mm for print.
    pub fn html(&self) -> String {
        let (m, n) = (self.threshold, self.cosigners.len());
        // keys in groups of four, easier to copy by hand; spans add no spaces when copied
        let grouped = |s: &str| -> String {
            s.as_bytes().chunks(4).map(|c| format!("<i>{}</i>", escape(std::str::from_utf8(c).unwrap_or_default()))).collect()
        };
        let signers: String = self
            .cosigners
            .iter()
            .enumerate()
            .map(|(i, c)| {
                format!(
                    r#"<div class="signer"><span class="no">{no}</span>
  <div><span class="cap">Fingerprint</span><span class="mono fp">{fp}</span></div>
  <div><span class="cap">Path</span><span class="mono">{path}</span></div>
  <div><span class="cap">Held by</span><span class="line"></span></div>
  <div class="key"><span class="cap">Public key</span><span class="mono">{xpub}</span></div>
</div>
"#,
                    no = i + 1,
                    fp = grouped(&c.fingerprint),
                    path = escape(&c.path),
                    xpub = grouped(&c.xpub)
                )
            })
            .collect();
        // the descriptor's QR, to scan it back in; a very long one may not fit a code
        let qr = qr_svg(&self.descriptor).map(|s| format!("<div class=\"qr\">{s}</div>")).unwrap_or_default();
        let fewer = if m < n { format!(" With only {m} of the {n} seeds, you need this sheet too.") } else { String::new() };
        format!(
            r##"<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Multisig wallet backup ({m} of {n})</title>
<style>
  /* The look of Goldfish's printable cards (cards/index.html): ink for what
     matters, --grey for text and labels, --line for rules and boxes. */
  :root {{
    --ink: #14222B; --grey: #7a766e; --line: #d2cdc4;
    --bg: #FFFFFF; --raised: #EDF2F4; --rule: #D9E2E6; --muted: #5A6B75;
    --sans: Inter, "Helvetica Neue", Arial, ui-sans-serif, system-ui, sans-serif;
    --mono: ui-monospace, "SF Mono", Menlo, Consolas, "DejaVu Sans Mono", monospace;
    --pw: 210mm; --ph: 279.4mm; /* A4's width and Letter's height: fits either paper */
  }}
  * {{ box-sizing: border-box; }}
  body {{ margin: 0; background: var(--raised); color: var(--ink); font: 15px/1.5 var(--sans); -webkit-font-smoothing: antialiased; }}
  .bar {{ max-width: var(--pw); margin: 0 auto; padding: 32px 0 24px; display: flex; flex-wrap: wrap; gap: 12px 24px; align-items: center; justify-content: space-between; }}
  .bar p {{ margin: 0; color: var(--muted); }}
  .print {{ display: inline-flex; align-items: center; gap: 10px; height: 50px; padding: 0 24px; border: 0; border-radius: 12px;
           background: var(--ink); color: #fff; font: inherit; font-size: 16px; font-weight: 600; cursor: pointer; }}
  .print svg {{ width: 18px; height: 18px; }}
  .sheet {{ background: #fff; width: var(--pw); min-height: var(--ph); margin: 0 auto 72px; padding: 12mm 14mm;
           box-shadow: 0 2px 16px rgb(20 34 43 / .14); display: flex; flex-direction: column; gap: 3.5mm; }}
  @media (max-width: 860px) {{ .sheet {{ zoom: .5; }} .bar {{ padding: 24px 16px; }} }}
  @media print {{
    /* margins on the paper, so a long sheet that runs onto page 2 has them too */
    @page {{ margin: 12mm 0; size: 210mm 279.4mm; }}
    body {{ background: #fff; }}
    .bar {{ display: none; }}
    .sheet {{ box-shadow: none; margin: 0; zoom: 1; padding: 0 14mm; min-height: calc(var(--ph) - 24mm); }}
    * {{ -webkit-print-color-adjust: exact; print-color-adjust: exact; }}
  }}

  .cap {{ font-size: 2.6mm; letter-spacing: .14em; text-transform: uppercase; color: var(--grey); }}
  .mono {{ font-family: var(--mono); font-size: 3mm; color: var(--ink); word-break: break-all; }}
  .mono i {{ font-style: normal; margin-right: .9mm; }}
  .alert {{ display: flex; align-items: center; gap: 3mm; padding-bottom: 3.5mm; border-bottom: .3mm solid var(--ink);
           font-size: 3.6mm; font-weight: 600; }}
  .alert::before {{ content: "!"; flex: none; width: 5.6mm; height: 5.6mm; border-radius: 50%; background: var(--ink); color: #fff;
                   display: flex; align-items: center; justify-content: center; font-size: 3.4mm; font-weight: 700; }}
  .alert span {{ font-weight: 400; color: var(--grey); }}
  h1 {{ margin: 0; font-size: 7mm; font-weight: 700; letter-spacing: -0.01em; line-height: 1.1; }}
  .sec {{ color: var(--ink); font-weight: 600; margin: 0 0 -2mm; }}

  /* boxed strips with hairline dividers, as on the cards */
  .strip {{ display: grid; grid-template-columns: repeat(4, auto); border: .3mm solid var(--line); border-radius: 2mm; }}
  .strip > div {{ padding: 2.6mm 4mm 3mm; display: flex; flex-direction: column; gap: 1.2mm; font-size: 3.2mm; min-width: 0; }}
  .strip > div + div {{ border-left: .2mm solid var(--line); }}
  .strip .wide {{ grid-column: 1 / -1; border-left: 0; border-top: .2mm solid var(--line); }}
  .line {{ display: block; height: 5mm; border-bottom: .2mm solid var(--line); }}

  .signers {{ display: flex; flex-direction: column; gap: 2mm; }}
  .signer {{ display: grid; grid-template-columns: 9mm 32mm 38mm 1fr; border: .3mm solid var(--line); border-radius: 2mm; break-inside: avoid; }}
  .signer > div {{ padding: 2mm 3.5mm 2.2mm; display: flex; flex-direction: column; gap: 1mm; border-left: .2mm solid var(--line); min-width: 0; }}
  .signer .no {{ grid-row: 1 / 3; display: flex; align-items: center; justify-content: center; font-size: 3.2mm; font-weight: 600; }}
  .signer .key {{ grid-column: 2 / -1; border-top: .2mm solid var(--line); }}
  .signer .fp {{ font-size: 3.6mm; font-weight: 600; }}

  .desc {{ display: grid; grid-template-columns: 38mm 1fr; border: .3mm solid var(--line); border-radius: 2mm; break-inside: avoid; }}
  .desc > div {{ padding: 3mm 4mm; display: flex; flex-direction: column; gap: 1.2mm; min-width: 0; }}
  .desc > div + div {{ border-left: .2mm solid var(--line); }}
  .keep {{ display: flex; flex-direction: column; gap: 1.5mm; break-inside: avoid; }}
  .keep .sec {{ margin: 0; }}
  .qr svg {{ width: 30mm; height: auto; display: block; }}
  .desc .mono {{ font-size: 2.7mm; line-height: 1.45; }}

  .steps {{ display: grid; grid-template-columns: repeat(3, 1fr); gap: 6mm; margin: 0; padding: 0; list-style: none;
           counter-reset: step; font-size: 2.9mm; line-height: 1.45; color: var(--grey); break-inside: avoid; }}
  .steps li {{ counter-increment: step; display: grid; grid-template-columns: 6mm 1fr; grid-template-rows: min-content 1fr; column-gap: 2mm; }}
  .steps li::before {{ content: counter(step); grid-row: 1 / 3; width: 5.4mm; height: 5.4mm; border-radius: 50%;
                      border: .25mm solid var(--ink); color: var(--ink); font-size: 2.8mm; font-weight: 600;
                      display: flex; align-items: center; justify-content: center; }}
  .steps strong, .store strong {{ color: var(--ink); font-weight: 600; font-size: 3.1mm; }}
  .store {{ margin-top: auto; padding-top: 3mm; border-top: .2mm solid var(--line); display: grid; grid-template-columns: repeat(3, 1fr);
           gap: 2.5mm 6mm; font-size: 2.6mm; line-height: 1.4; color: var(--grey); break-inside: avoid; }}
  .store > .cap {{ grid-column: 1 / -1; color: var(--ink); font-weight: 600; }}
  .store > div {{ display: grid; grid-template-columns: 6mm 1fr; grid-template-rows: min-content 1fr; column-gap: 2.5mm; }}
  .store svg {{ width: 6mm; height: 6mm; grid-row: 1 / 3; fill: none; stroke: var(--ink); stroke-width: 1.6; stroke-linecap: round; stroke-linejoin: round; }}
  .store strong {{ font-size: 2.85mm; }}
</style></head><body>
<div class="bar"><p>Print it, then keep a copy with each seed backup.</p>
<button class="print" onclick="print()"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M6 9V3h12v6"/><rect x="3" y="9" width="18" height="8" rx="2"/><path d="M6 14h12v7H6z"/></svg>Print</button></div>
<main class="sheet">
<div class="alert">This sheet can't spend your bitcoin, but it shows all of it. <span>No seed or private key is on it. Keep it private.</span></div>
<h1>Multisig wallet backup</h1>
<div class="strip">
  <div><span class="cap">Type</span><span>{m} of {n}, <span class="mono">wsh(sortedmulti)</span></span></div>
  <div><span class="cap">Network</span><span>{network}</span></div>
  <div><span class="cap">Made</span><span>{date}</span></div>
  <div><span class="cap">Label</span><span class="line"></span></div>
</div>
<span class="cap sec">Signers</span>
<div class="signers">
{signers}</div>
<div class="keep"><span class="cap sec">Descriptor</span>
<div class="desc">
  <div>{qr}</div>
  <div><span class="mono">{descriptor}</span>
  <span class="cap" style="margin-top:2mm">First address</span><span class="mono">{address}</span></div>
</div></div>
<ol class="steps">
  <li><strong>Add the wallet</strong><span>In a wallet app that takes a descriptor, such as Dove or Sparrow: scan the QR or type it in.</span></li>
  <li><strong>Check the address</strong><span>The app's first receive address must match the one on this sheet.</span></li>
  <li><strong>Sign with any {m}</strong><span>Any {m} of the {n} seeds sign a payment.{fewer}</span></li>
</ol>
<div class="store">
  <span class="cap">Storing it</span>
  <div><svg viewBox="0 0 24 24" aria-hidden="true"><path d="M7 20s-4.5-4.6-4.5-8.3a4.5 4.5 0 0 1 9 0C11.5 15.4 7 20 7 20z"/><circle cx="7" cy="11.7" r="1.5"/><path d="M17 15s-4.5-4.6-4.5-8.3a4.5 4.5 0 0 1 9 0C21.5 10.4 17 15 17 15z"/><circle cx="17" cy="6.7" r="1.5"/></svg><strong>One with each seed</strong><span>Then any {m} of the places hold all you need to get your coins back.</span></div>
  <div><svg viewBox="0 0 24 24" aria-hidden="true"><path d="M3 12s3.5-6 9-6 9 6 9 6-3.5 6-9 6-9-6-9-6z"/><circle cx="12" cy="12" r="2.5"/><path d="M4 20 20 4"/></svg><strong>Private, not secret</strong><span>It can't move coins, but it shows your balance and every payment.</span></div>
  <div><svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="9" cy="8" r="3.2"/><path d="M3 20a6 6 0 0 1 12 0"/><circle cx="17" cy="9.5" r="2.5"/><path d="M15.5 14.4A5 5 0 0 1 21 19"/></svg><strong>Plan for heirs</strong><span>Someone you trust should know this sheet and the seeds exist, and where.</span></div>
</div>
</main>
</body></html>
"##,
            network = self.network,
            date = escape(&self.date),
            address = grouped(&self.first_address),
            descriptor = escape(&self.descriptor),
        )
    }
}

// ------------------------------------------------------------ chain

/// Where the wallet syncs, and broadcasts when it's your own server (else
/// they go to peers over Tor, see [`p2p`]): an Esplora API (`https://…`) or an
/// Electrum server (`ssl://host:port`, `tcp://host:port`), optionally through
/// a proxy. Empty `url` means a public Electrum server for the key's network.
#[derive(Clone, Debug, Default, Serialize, Deserialize, PartialEq)]
pub struct ServerConfig {
    pub url: String,
    pub proxy: String,
}

pub enum Backend {
    Esplora(esplora_client::BlockingClient),
    Electrum(BdkElectrumClient<electrum_client::Client>),
}

const TIMEOUT_S: u64 = 30;

/// Public Electrum servers. Not Esplora on mempool.space: its `/blocks`,
/// which the Esplora full scan asks for, hangs in minreq (curl gets it in
/// under a second; cause not found yet).
pub fn default_server(network: Network) -> &'static str {
    match network {
        Network::Bitcoin => "ssl://electrum.blockstream.info:50002",
        _ => "ssl://mempool.space:60602",
    }
}

/// A self-signed certificate is normal on your own node: on .onion, local
/// and private addresses Electrum's TLS is not checked against a domain.
fn own_host(host: &str) -> bool {
    let host = host.trim_start_matches('[').trim_end_matches(']');
    host.ends_with(".onion")
        || host.ends_with(".local")
        || host == "localhost"
        || match host.parse::<std::net::IpAddr>() {
            Ok(std::net::IpAddr::V4(ip)) => ip.is_private() || ip.is_loopback() || ip.is_link_local(),
            Ok(std::net::IpAddr::V6(ip)) => ip.is_loopback() || (ip.segments()[0] & 0xfe00) == 0xfc00,
            Err(_) => false,
        }
}

impl ServerConfig {
    /// Your own server (on .onion, .local or a private address): broadcasts
    /// go to it, not over the built-in Tor, since it already knows the
    /// wallet and it's how a regtest or local-only setup reaches its node.
    pub fn own_server(&self) -> bool {
        let url = self.url.trim();
        let rest = ["ssl://", "tcp://", "https://", "http://"].iter().find_map(|p| url.strip_prefix(p)).unwrap_or("");
        let authority = rest.split('/').next().unwrap_or("");
        let host = authority.rsplit_once(':').filter(|(_, port)| port.chars().all(|c| c.is_ascii_digit())).map_or(authority, |(h, _)| h);
        !host.is_empty() && own_host(host)
    }

    pub fn connect(&self, network: Network) -> Res<Backend> {
        let url = self.url.trim();
        let url = if url.is_empty() { default_server(network) } else { url };
        let proxy = self.proxy.trim();
        if let Some(rest) = url.strip_prefix("ssl://").or_else(|| url.strip_prefix("tcp://")) {
            let host = rest.rsplit_once(':').map(|(h, _)| h).ok_or("an Electrum server needs a port, like ssl://host:50002")?;
            let mut config = electrum_client::ConfigBuilder::new()
                .timeout(Some(std::time::Duration::from_secs(TIMEOUT_S)))
                .validate_domain(!own_host(host));
            if !proxy.is_empty() {
                if proxy.contains("://") && !proxy.starts_with("socks5://") && !proxy.starts_with("socks5h://") {
                    return Err("an Electrum server takes a SOCKS5 proxy, like socks5://127.0.0.1:9050 (Tor)".into());
                }
                // the client only strips "socks5://"; Tor resolves names either way
                config = config.socks5(Some(electrum_client::Socks5Config::new(proxy.replacen("socks5h://", "", 1))));
            }
            let client = electrum_client::Client::from_config(url, config.build()).map_err(err)?;
            return Ok(Backend::Electrum(BdkElectrumClient::new(client)));
        }
        if !(url.starts_with("https://") || url.starts_with("http://")) {
            return Err("server: use https://… for Esplora, or ssl://host:port or tcp://host:port for Electrum".into());
        }
        let mut builder = esplora_client::Builder::new(url).timeout(TIMEOUT_S);
        if !proxy.is_empty() {
            if proxy.starts_with("socks") {
                return Err("Esplora takes an HTTP proxy. For Tor, set HTTPTunnelPort 9080 in torrc and use http://127.0.0.1:9080, or use an Electrum server with socks5://127.0.0.1:9050".into());
            }
            builder = builder.proxy(proxy);
        }
        Ok(Backend::Esplora(builder.build_blocking()))
    }
}

/// Suggested fee rates in sat/vB: next block, about an hour, about a day.
#[derive(Serialize, Clone, Debug)]
pub struct FeeRates {
    pub fast: f64,
    pub normal: f64,
    pub slow: f64,
}

impl Backend {
    /// Full scan. The request is built from the wallet beforehand, so a
    /// caller can drop its lock over the network part.
    pub fn full_scan(&self, request: FullScanRequest<KeychainKind>) -> Res<FullScanResponse<KeychainKind>> {
        match self {
            Backend::Esplora(c) => c.full_scan(request, 20, 4).map_err(err),
            Backend::Electrum(c) => c.full_scan(request, 20, 10, true).map_err(err),
        }
    }

    /// Through the server instead of over Tor: for your own server, or when
    /// Tor or the peers can't be reached and you choose to.
    pub fn broadcast(&self, tx: &Transaction) -> Res<Txid> {
        match self {
            Backend::Esplora(c) => c.broadcast(tx).map_err(err)?,
            Backend::Electrum(c) => {
                c.transaction_broadcast(tx).map_err(err)?;
            }
        }
        Ok(tx.compute_txid())
    }

    /// The server's chain, from its genesis block, has to be the wallet's:
    /// a mainnet server shows a test wallet empty, and the other way round.
    pub fn check_network(&self, network: Network) -> Res<()> {
        let genesis = match self {
            Backend::Esplora(c) => c.get_block_hash(0).map_err(err)?,
            Backend::Electrum(c) => c.inner.block_header(0).map_err(err)?.block_hash(),
        };
        network_mismatch(genesis, network).map_or(Ok(()), Err)
    }

    pub fn fee_rates(&self) -> Res<FeeRates> {
        let at = |target: u16| -> Res<f64> {
            match self {
                Backend::Esplora(c) => {
                    // Fetched by hand: mempool.space answers this one with
                    // 203, which the client's get_fee_estimates rejects.
                    let resp = c.get_request("/fee-estimates").map_err(err)?.send().map_err(err)?;
                    if !(200..300).contains(&resp.status_code) {
                        return Err(format!("fee estimates: HTTP {}", resp.status_code));
                    }
                    let map: std::collections::HashMap<String, f64> = serde_json::from_slice(resp.as_bytes()).map_err(err)?;
                    let map = map.into_iter().filter_map(|(k, v)| Some((k.parse::<u16>().ok()?, v)));
                    // the estimate for the nearest target at or above `target`
                    let mut targets: Vec<_> = map.into_iter().collect();
                    targets.sort_by_key(|(t, _)| *t);
                    targets.iter().find(|(t, _)| *t >= target).or(targets.last()).map(|(_, r)| *r).ok_or("no fee estimates".into())
                }
                Backend::Electrum(c) => {
                    // BTC/kvB; -1 when the server has no estimate
                    let btc_kvb = c.inner.estimate_fee(target as usize, None).map_err(err)?;
                    if btc_kvb <= 0.0 { Err("no fee estimates".into()) } else { Ok(btc_kvb * 1e5) }
                }
            }
        };
        let round = |r: f64| (r * 10.0).ceil() / 10.0;
        let (fast, normal, slow) = (at(1)?, at(6)?, at(144)?);
        Ok(FeeRates { fast: round(fast), normal: round(normal.min(fast)), slow: round(slow.min(normal)) })
    }
}

/// Test networks all count as one: a test key doesn't say which.
fn network_mismatch(genesis: BlockHash, network: Network) -> Option<String> {
    use bdk_wallet::bitcoin::constants::genesis_block;
    let server = [Network::Bitcoin, Network::Testnet, Network::Testnet4, Network::Signet, Network::Regtest]
        .into_iter()
        .find(|n| genesis_block(*n).block_hash() == genesis);
    let name = |n: Network| if n == Network::Bitcoin { "mainnet".to_string() } else { n.to_string() };
    match server {
        Some(s) if (s == Network::Bitcoin) == (network == Network::Bitcoin) => None,
        Some(s) => Some(format!("This server is on {}, but this wallet is on {}.", name(s), name(network))),
        None if network == Network::Bitcoin => Some("This server isn't on mainnet.".into()),
        None => None, // a custom signet
    }
}

#[derive(Serialize, Clone)]
pub struct TxRow {
    pub txid: String,
    pub net_sat: i64,
    pub confirmed: bool,
    /// sat/vB it pays, when the wallet knows all its inputs
    pub fee_rate: Option<f64>,
    /// unconfirmed and sent from this wallet: its fee can be raised (RBF)
    pub can_bump: bool,
    /// unix seconds: when its block was mined, or when it was first seen
    pub time: Option<u64>,
}

#[derive(Serialize, Clone)]
pub struct Overview {
    pub network: String,
    pub kind: String,
    pub multisig: bool,
    pub threshold: usize,
    pub fingerprints: Vec<String>,
    pub confirmed_sat: u64,
    pub pending_sat: u64,
    pub txs: Vec<TxRow>,
}

pub fn overview(account: &Account, wallet: &Wallet) -> Overview {
    let b = wallet.balance();
    let mut txs: Vec<TxRow> = wallet
        .transactions()
        .map(|t| {
            let (sent, received) = wallet.sent_and_received(&t.tx_node.tx);
            let confirmed = t.chain_position.is_confirmed();
            let time = match &t.chain_position {
                bdk_wallet::chain::ChainPosition::Confirmed { anchor, .. } => Some(anchor.confirmation_time),
                bdk_wallet::chain::ChainPosition::Unconfirmed { first_seen, last_seen } => first_seen.or(*last_seen),
            };
            TxRow {
                txid: t.tx_node.txid.to_string(),
                net_sat: received.to_sat() as i64 - sent.to_sat() as i64,
                confirmed,
                fee_rate: wallet.calculate_fee_rate(&t.tx_node.tx).ok().map(sat_per_vb),
                can_bump: !confirmed && sent.to_sat() > 0 && t.tx_node.tx.is_explicitly_rbf(),
                time,
            }
        })
        .collect();
    // pending first, then newest first
    txs.sort_by_key(|t| (t.confirmed, std::cmp::Reverse(t.time.unwrap_or(u64::MAX))));
    Overview {
        network: account.network.to_string(),
        kind: account.kind(),
        multisig: account.is_multisig(),
        threshold: account.threshold,
        fingerprints: account.fingerprints.clone(),
        confirmed_sat: b.confirmed.to_sat(),
        pending_sat: (b.trusted_pending + b.untrusted_pending).to_sat(),
        txs,
    }
}

// ------------------------------------------------------------ receiving

/// Unused receive addresses in a row that other wallets, and a restore from
/// the seed, keep looking through: Sparrow's default, and Dove's own scan
/// (which also covers every address it has handed out).
pub const GAP_LIMIT: u32 = 20;

/// Receive addresses handed out after the last one that got paid.
pub fn unused_in_a_row(wallet: &Wallet) -> u32 {
    let Some(last) = wallet.derivation_index(KeychainKind::External) else { return 0 };
    match wallet.spk_index().last_used_index(KeychainKind::External) {
        Some(used) => last.saturating_sub(used),
        None => last + 1,
    }
}

/// The address Receive shows: the newest one handed out while nothing has
/// paid to it, else the next one. Never an older unused one, which may
/// already have gone to someone else.
pub fn receive_address(wallet: &mut Wallet) -> bdk_wallet::AddressInfo {
    if let Some(last) = wallet.derivation_index(KeychainKind::External) {
        if !wallet.spk_index().is_used(KeychainKind::External, last) {
            return wallet.peek_address(KeychainKind::External, last);
        }
    }
    wallet.reveal_next_address(KeychainKind::External)
}

/// A fresh address for the next payer, unless GAP_LIMIT are already waiting
/// unused: past that, a restore elsewhere could miss coins sent to it.
pub fn new_receive_address(wallet: &mut Wallet) -> Res<bdk_wallet::AddressInfo> {
    if unused_in_a_row(wallet) >= GAP_LIMIT {
        return Err(format!("{GAP_LIMIT} addresses are waiting unused: use one first"));
    }
    Ok(wallet.reveal_next_address(KeychainKind::External))
}

// ------------------------------------------------------------ spending

#[derive(Serialize, Clone)]
pub struct Recipient {
    pub address: String,
    pub sat: u64,
    /// one of this wallet's own receive addresses (a send to yourself)
    pub yours: bool,
}

#[derive(Serialize, Clone)]
pub struct Review {
    pub txid: String,
    pub recipients: Vec<Recipient>,
    pub change_sat: u64,
    pub fee_sat: u64,
    pub frames: usize,
}

fn sat_per_vb(r: FeeRate) -> f64 {
    r.to_sat_per_kwu() as f64 / 250.0
}

/// sat/vB, fractions allowed (0.1 sat/vB is relayed by current nodes).
fn fee_rate(sat_per_vb: f64) -> Res<FeeRate> {
    if !(sat_per_vb.is_finite() && sat_per_vb > 0.0 && sat_per_vb < 10_000.0) {
        return Err("fee rate: give sat/vB between 0.1 and 10,000".into());
    }
    Ok(FeeRate::from_sat_per_kwu((sat_per_vb * 250.0).round() as u64))
}

/// The same payment again at a higher fee rate, replacing the pending `txid`
/// (RBF). Goldfish signs it like any other transaction.
pub fn build_bump(wallet: &mut Wallet, txid: &str, sat_per_vb: f64) -> Res<Psbt> {
    let txid: Txid = txid.parse().map_err(err)?;
    use bdk_wallet::error::BuildFeeBumpError::*;
    let mut builder = wallet.build_fee_bump(txid).map_err(|e| match e {
        TransactionConfirmed(_) => "It's already confirmed, so there's nothing to speed up.".to_string(),
        IrreplaceableTransaction(_) => "This one can't be sped up: it wasn't sent as replaceable.".to_string(),
        TransactionNotFound(_) => "That transaction isn't in this wallet any more. Refresh and try again.".to_string(),
        e => e.to_string(),
    })?;
    builder.fee_rate(fee_rate(sat_per_vb)?).only_witness_utxo();
    builder.finish().map_err(spend_error)
}

pub fn build_psbt(wallet: &mut Wallet, to: &str, amount_sat: u64, sat_per_vb: f64) -> Res<Psbt> {
    let address = to
        .trim()
        .parse::<Address<_>>()
        .map_err(err)?
        .require_network(wallet.network())
        .map_err(err)?;
    // Nodes don't relay outputs below the dust limit: say so before building.
    let min = address.script_pubkey().minimal_non_dust().to_sat();
    if amount_sat < min {
        return Err(format!("That's too small to send. The minimum to this address is ₿{min}."));
    }
    let fee_rate = fee_rate(sat_per_vb)?;
    let mut builder = wallet.build_tx();
    builder
        .add_recipient(address.script_pubkey(), Amount::from_sat(amount_sat))
        .fee_rate(fee_rate)
        // Witness UTXOs only: a smaller PSBT, fewer QR frames. Goldfish
        // checks the amount it signs for against the witness UTXO.
        .only_witness_utxo();
    builder.finish().map_err(spend_error)
}

/// BDK's errors in words: the two a person can act on.
fn spend_error(e: bdk_wallet::error::CreateTxError) -> String {
    use bdk_wallet::error::CreateTxError::*;
    match e {
        CoinSelection(f) => format!(
            "Not enough bitcoin. This needs ₿{} with the fee, and the wallet has ₿{}.",
            f.needed.to_sat(),
            f.available.to_sat()
        ),
        OutputBelowDustLimit(_) => "That's too small to send.".into(),
        FeeRateTooLow { required } => format!("Pick at least {:.1} sat/vB.", required.to_sat_per_kwu() as f64 / 250.0),
        FeeTooLow { required } => format!("The fee has to be at least ₿{}.", required.to_sat()),
        e => e.to_string(),
    }
}

pub fn review(wallet: &Wallet, psbt: &Psbt, frames: usize) -> Review {
    let mut recipients = vec![];
    let mut change_sat = 0;
    for out in &psbt.unsigned_tx.output {
        // Change is only what goes back to the change chain: a payment to one
        // of our own receive addresses is still the amount being sent.
        let ours = wallet.derivation_of_spk(out.script_pubkey.clone());
        if matches!(ours, Some((KeychainKind::Internal, _))) {
            change_sat += out.value.to_sat();
        } else {
            let address = Address::from_script(&out.script_pubkey, wallet.network())
                .map(|a| a.to_string())
                .unwrap_or_else(|_| out.script_pubkey.to_hex_string());
            recipients.push(Recipient { address, sat: out.value.to_sat(), yours: ours.is_some() });
        }
    }
    Review {
        txid: psbt.unsigned_tx.compute_txid().to_string(),
        recipients,
        change_sat,
        fee_sat: psbt.fee().map(|f| f.to_sat()).unwrap_or(0),
        frames,
    }
}

/// Who has signed so far: the fingerprints whose key signed every input.
#[derive(Serialize, Clone, Debug, Default)]
pub struct Signatures {
    pub signers: Vec<String>,
    pub have: usize,
    pub needed: usize,
}

/// Every partial signature must sign this transaction's input with its key:
/// a bad one would count toward m of n and only fail at broadcast.
fn verify_signatures(psbt: &Psbt) -> Res<()> {
    let secp = bdk_wallet::bitcoin::secp256k1::Secp256k1::verification_only();
    let mut cache = bdk_wallet::bitcoin::sighash::SighashCache::new(&psbt.unsigned_tx);
    for (i, input) in psbt.inputs.iter().enumerate() {
        if input.partial_sigs.is_empty() {
            continue;
        }
        let (msg, _) = psbt.sighash_ecdsa(i, &mut cache).map_err(err)?;
        for (pk, sig) in &input.partial_sigs {
            if secp.verify_ecdsa(&msg, &sig.signature, &pk.inner).is_err() {
                let who = input.bip32_derivation.get(&pk.inner).map(|(fp, _)| fp.to_string()).unwrap_or_else(|| "a cosigner".into());
                return Err(format!("The signature from {who} doesn't check out. Sign again on that device."));
            }
        }
    }
    Ok(())
}

/// Inputs another wallet already finalized (Sparrow, once it has enough
/// signatures), back to partial signatures: each one in the witness must
/// check out against one of `ours`' keys for that input, and the witness must
/// end in our own script (or key, for one signer). The finished witness is
/// dropped; ours is built again from our script on finalizing, so a witness
/// in the wrong shape can't slip through or stick.
fn unfinalize(merged: &mut Psbt, ours: &Psbt) -> Res<()> {
    let bad = || "A signer sent back a finished transaction that doesn't check out. Sign again on that device.".to_string();
    let secp = bdk_wallet::bitcoin::secp256k1::Secp256k1::verification_only();
    let mut cache = bdk_wallet::bitcoin::sighash::SighashCache::new(&ours.unsigned_tx);
    for (i, (input, our)) in merged.inputs.iter_mut().zip(&ours.inputs).enumerate() {
        let Some(witness) = input.final_script_witness.take() else { continue };
        input.final_script_sig = None;
        let items: Vec<&[u8]> = witness.iter().collect();
        let (last, sigs) = items.split_last().ok_or_else(bad)?;
        let script_ok = match &our.witness_script {
            Some(script) => script.as_bytes() == *last,
            None => our.bip32_derivation.keys().any(|k| k.serialize()[..] == **last),
        };
        if !script_ok {
            return Err(bad());
        }
        let (msg, _) = ours.sighash_ecdsa(i, &mut cache).map_err(err)?;
        for sig in sigs.iter().filter(|s| !s.is_empty()) {
            let sig = bdk_wallet::bitcoin::ecdsa::Signature::from_slice(sig).map_err(|_| bad())?;
            let pk = our.bip32_derivation.keys().find(|pk| secp.verify_ecdsa(&msg, &sig.signature, pk).is_ok()).ok_or_else(bad)?;
            input.partial_sigs.insert(bdk_wallet::bitcoin::PublicKey::new(*pk), sig);
        }
    }
    Ok(())
}

/// Merge a signed copy of `psbt` (from Goldfish or another cosigner) into
/// it. Once `needed` keys have signed every input, also the finished
/// transaction, ready to broadcast.
pub fn add_signatures(wallet: &Wallet, psbt: &mut Psbt, mut signed: Psbt, needed: usize) -> Res<(Signatures, Option<Transaction>)> {
    if signed.unsigned_tx.compute_txid() != psbt.unsigned_tx.compute_txid() {
        return Err("That's a different transaction. Sign this one, then bring it back.".into());
    }
    // previous transactions an export added: not on to the next QR
    for (input, ours) in signed.inputs.iter_mut().zip(&psbt.inputs) {
        if ours.non_witness_utxo.is_none() {
            input.non_witness_utxo = None;
        }
    }
    let mut merged = psbt.clone();
    merged.combine(signed).map_err(err)?;
    unfinalize(&mut merged, psbt)?;
    verify_signatures(&merged)?;
    *psbt = merged;
    let per_input: Vec<std::collections::BTreeSet<String>> = psbt
        .inputs
        .iter()
        .map(|i| i.partial_sigs.keys().filter_map(|pk| i.bip32_derivation.get(&pk.inner).map(|(fp, _)| fp.to_string())).collect())
        .collect();
    let signers: Vec<String> = match per_input.split_first() {
        Some((first, rest)) => first.iter().filter(|fp| rest.iter().all(|s| s.contains(*fp))).cloned().collect(),
        None => vec![],
    };
    let have = per_input.iter().map(|s| s.len()).min().unwrap_or(0);
    let sigs = Signatures { signers, have: have.min(needed), needed };
    if have < needed {
        return Ok((sigs, None));
    }
    let mut done = psbt.clone();
    let opts = SignOptions { trust_witness_utxo: true, ..Default::default() };
    if !wallet.finalize_psbt(&mut done, opts).map_err(err)? {
        return Err("the signatures are there, but the transaction doesn't finalize".into());
    }
    Ok((sigs, Some(done.extract_tx().map_err(err)?)))
}

/// For tests without a chain: a confirmed coin of `sat` on receive address 0.
pub fn fund_for_demo(wallet: &mut Wallet, sat: u64) {
    let addr = wallet.peek_address(KeychainKind::External, 0).address;
    let tx = Transaction {
        version: transaction::Version::TWO,
        lock_time: absolute::LockTime::ZERO,
        input: vec![TxIn {
            previous_output: OutPoint::new(
                "0707070707070707070707070707070707070707070707070707070707070707".parse().unwrap(),
                0,
            ),
            ..Default::default()
        }],
        output: vec![TxOut { value: Amount::from_sat(sat), script_pubkey: addr.script_pubkey() }],
    };
    let block = BlockId {
        height: 1000,
        hash: "0000000000000000000000000000000000000000000000000000000000000abc"
            .parse::<BlockHash>()
            .unwrap(),
    };
    let tip = wallet.latest_checkpoint().insert(block);
    let mut tx_update = TxUpdate::default();
    tx_update.anchors = [(ConfirmationBlockTime { block_id: block, confirmation_time: 1 }, tx.compute_txid())].into();
    tx_update.txs = vec![Arc::new(tx)];
    wallet
        .apply_update(Update { chain: Some(tip), tx_update, ..Default::default() })
        .expect("demo funding");
}

// ------------------------------------------------------------ QR / UR

/// Fragment length of the animated PSBT: what Sparrow and the e2e tests use,
/// comfortably inside Goldfish's 240x180 camera view.
pub const UR_FRAGMENT_LEN: usize = 100;

fn cbor_bytes(data: &[u8]) -> Vec<u8> {
    let n = data.len();
    let mut v = Vec::with_capacity(n + 5);
    match n {
        0..=23 => v.push(0x40 | n as u8),
        24..=0xff => v.extend([0x58, n as u8]),
        0x100..=0xffff => {
            v.push(0x59);
            v.extend((n as u16).to_be_bytes());
        }
        _ => {
            v.push(0x5a);
            v.extend((n as u32).to_be_bytes());
        }
    }
    v.extend_from_slice(data);
    v
}

fn cbor_unbytes(c: &[u8]) -> Res<&[u8]> {
    let bad = || "crypto-psbt: not a CBOR byte string".to_string();
    let (&head, rest) = c.split_first().ok_or_else(bad)?;
    if head >> 5 != 2 {
        return Err(bad());
    }
    let (n, body) = match head & 0x1f {
        n @ 0..=23 => (n as usize, rest),
        24 => (*rest.first().ok_or_else(bad)? as usize, &rest[1..]),
        25 if rest.len() >= 2 => (u16::from_be_bytes([rest[0], rest[1]]) as usize, &rest[2..]),
        26 if rest.len() >= 4 => (u32::from_be_bytes(rest[..4].try_into().unwrap()) as usize, &rest[4..]),
        _ => return Err(bad()),
    };
    body.get(..n).ok_or_else(bad)
}

pub fn psbt_encoder(psbt: &Psbt) -> Res<ur::Encoder<'static>> {
    ur::Encoder::new(&cbor_bytes(&psbt.serialize()), UR_FRAGMENT_LEN, "crypto-psbt").map_err(err)
}

/// Text (a descriptor) as an animated `bytes` UR, which Goldfish reads.
pub fn text_encoder(text: &str) -> Res<ur::Encoder<'static>> {
    ur::Encoder::bytes(&cbor_bytes(text.as_bytes()), UR_FRAGMENT_LEN).map_err(err)
}

/// Parts in one cycle of the animation (the pure fragments).
pub fn fragment_count(first_part: &str) -> usize {
    part_indices(first_part).map(|(_, n)| n).unwrap_or(1)
}

pub fn part_indices(part: &str) -> Option<(usize, usize)> {
    let seq = part.to_lowercase();
    let seq = seq.strip_prefix("ur:")?.split('/').nth(1)?.to_string();
    let (i, n) = seq.split_once('-')?;
    Some((i.parse().ok()?, n.parse().ok()?))
}

/// Upper case, as Sparrow shows it: QR alphanumeric mode, a smaller code.
/// Square modules and finder eyes, as plain as Goldfish's camera needs them
/// (rounded eyes stop it reading the code); only the ink is Dove's navy.
pub fn qr_svg(text: &str) -> Res<String> {
    let text = if text.to_lowercase().starts_with("ur:") { text.to_uppercase() } else { text.to_string() };
    let code = qrcode::QrCode::with_error_correction_level(text, qrcode::EcLevel::L).map_err(err)?;
    Ok(code
        .render::<qrcode::render::svg::Color>()
        .dark_color(qrcode::render::svg::Color("#14222B"))
        .min_dimensions(320, 320)
        .quiet_zone(true)
        .build())
}

/// Collects a PSBT from what the camera reads: an animated or single-part
/// `ur:crypto-psbt`, or a base64 PSBT in one QR.
#[derive(Default)]
pub struct PsbtScanner {
    decoder: ur::Decoder,
    seen: std::collections::BTreeSet<usize>,
    total: usize,
}

#[derive(Serialize, Clone, Debug)]
pub struct ScanProgress {
    pub percent: u8,
    pub done: bool,
}

impl PsbtScanner {
    /// Feed one decoded QR. Ok(Some) once a whole PSBT is in.
    pub fn receive(&mut self, text: &str) -> Res<Option<Psbt>> {
        let t = text.trim();
        if t.starts_with("cHNidP") {
            let raw = B64.decode(t).map_err(err)?;
            return Psbt::deserialize(&raw).map(Some).map_err(err);
        }
        let lower = t.to_lowercase();
        if !lower.starts_with("ur:crypto-psbt/") {
            return Err("That QR isn't a signed transaction.".into());
        }
        let cbor = match part_indices(&lower) {
            None => ur::decode(&lower).map_err(err)?.1,
            Some((i, n)) => {
                // Parts of another animation: start over.
                if self.total != 0 && self.total != n {
                    *self = Self::default();
                }
                self.total = n;
                self.seen.insert(i);
                if self.decoder.receive(&lower).is_err() {
                    // A part of a different message with the same part count (the
                    // decoder pins the first message's checksum): start over with it.
                    *self = Self::default();
                    self.total = n;
                    self.seen.insert(i);
                    self.decoder.receive(&lower).map_err(err)?;
                }
                match self.decoder.message().map_err(err)? {
                    Some(m) if self.decoder.complete() => m,
                    _ => return Ok(None),
                }
            }
        };
        Psbt::deserialize(cbor_unbytes(&cbor)?).map(Some).map_err(err)
    }

    pub fn progress(&self) -> ScanProgress {
        let got = self.decoder.resolved_fragment_count().unwrap_or(0);
        let percent = if self.total == 0 { 0 } else { (100 * got / self.total).min(99) as u8 };
        ScanProgress { percent, done: false }
    }
}

pub fn psbt_base64(psbt: &Psbt) -> String {
    B64.encode(psbt.serialize())
}

pub fn psbt_from_base64(s: &str) -> Res<Psbt> {
    Psbt::deserialize(&B64.decode(s.trim()).map_err(err)?).map_err(err)
}

/// `psbt` for a signer that isn't Goldfish (a .psbt file, or copied as
/// text): with each input's whole previous transaction, which Trezor and
/// Ledger ask for. Goldfish's QR leaves them out to stay small.
pub fn psbt_for_export(wallet: &Wallet, psbt: &Psbt) -> Psbt {
    let mut out = psbt.clone();
    for (input, txin) in out.inputs.iter_mut().zip(&psbt.unsigned_tx.input) {
        if input.non_witness_utxo.is_none() {
            input.non_witness_utxo = wallet.get_tx(txin.previous_output.txid).map(|t| (*t.tx_node.tx).clone());
        }
    }
    out
}

/// A signed file from another wallet, for `unsigned`: a PSBT, binary
/// (Sparrow, Coldcard), base64 or hex, or the finished transaction itself
/// (Coldcard's .txn once it completes the signatures), binary or hex. A
/// finished transaction comes back as `unsigned` with each input finalized.
pub fn signed_from_file(bytes: &[u8], unsigned: &Psbt) -> Res<Psbt> {
    use bdk_wallet::bitcoin::hex::FromHex;
    let not = || "That file isn't a signed transaction.".to_string();
    let text = std::str::from_utf8(bytes).map(str::trim).unwrap_or("");
    let raw = match Vec::<u8>::from_hex(text) {
        Ok(raw) if !raw.is_empty() => raw,
        _ if text.starts_with("cHNidP") => return psbt_from_base64(text).map_err(|_| not()),
        _ => bytes.to_vec(),
    };
    if raw.starts_with(b"psbt\xff") {
        return Psbt::deserialize(&raw).map_err(|_| not());
    }
    let tx: Transaction = bdk_wallet::bitcoin::consensus::encode::deserialize(&raw).map_err(|_| not())?;
    if tx.compute_txid() != unsigned.unsigned_tx.compute_txid() {
        // another of this wallet's transactions, or just not one at all
        let ours = tx.input.iter().any(|i| unsigned.unsigned_tx.input.iter().any(|u| u.previous_output == i.previous_output));
        return Err(if ours { "That's a different transaction. Sign this one, then bring it back.".into() } else { not() });
    }
    let mut psbt = unsigned.clone();
    for (input, txin) in psbt.inputs.iter_mut().zip(&tx.input) {
        if !txin.witness.is_empty() {
            input.final_script_witness = Some(txin.witness.clone());
        }
    }
    Ok(psbt)
}

/// scrypt's cost for the password: 2^18 (256 MiB; well under a second on a
/// desktop, slow enough to make guessing expensive). Fixed,
/// not age's default of timing this computer: a file locked on a fast, idle
/// machine would otherwise be refused as too much work on a slow or busy one.
const SCRYPT_LOG_N: u8 = 18;

/// `plain` locked with `password`: the age format, scrypt passphrase.
pub fn seal(plain: &[u8], password: &str) -> Res<Vec<u8>> {
    let mut recipient = age::scrypt::Recipient::new(password.to_owned().into());
    recipient.set_work_factor(SCRYPT_LOG_N);
    age::encrypt(&recipient, plain).map_err(err)
}

pub fn unseal(sealed: &[u8], password: &str) -> Res<Vec<u8>> {
    let mut identity = age::scrypt::Identity::new(password.to_owned().into());
    identity.set_max_work_factor(SCRYPT_LOG_N);
    age::decrypt(&identity, sealed).map_err(|e| match e {
        age::DecryptError::DecryptionFailed | age::DecryptError::NoMatchingKeys => "wrong password".into(),
        e => err(e),
    })
}

/// Save the settings in `dir`: settings.json, or with a password, locked in
/// settings.age. `remove_other_settings` then deletes the other one.
pub fn write_settings(dir: &std::path::Path, plain: &[u8], password: Option<&str>) -> Res<()> {
    let (bytes, ext) = match password {
        Some(pw) => (seal(plain, pw)?, "age"),
        None => (plain.to_vec(), "json"),
    };
    std::fs::create_dir_all(dir).map_err(err)?;
    let path = dir.join("settings").with_extension(ext);
    // write a copy, then swap it in: a crash mid-write can't eat the saved wallet
    let tmp = path.with_extension(format!("{ext}.tmp"));
    std::fs::write(&tmp, bytes).map_err(err)?;
    std::fs::rename(&tmp, &path).map_err(err)
}

/// After `write_settings`, so only one of the two files is ever there.
pub fn remove_other_settings(dir: &std::path::Path, locked: bool) -> Res<()> {
    let other = dir.join("settings").with_extension(if locked { "json" } else { "age" });
    match std::fs::remove_file(&other) {
        Err(e) if e.kind() != std::io::ErrorKind::NotFound => {
            Err(format!("saved, but couldn't delete the old {}: {e}", other.display()))
        }
        _ => Ok(()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use bdk_wallet::bitcoin::bip32::{DerivationPath, Xpriv};
    use bdk_wallet::bitcoin::secp256k1::Secp256k1;
    use bdk_wallet::bitcoin::NetworkKind;

    /// The BIP-84 test vector's account key ("abandon … about"), spelled as a zpub,
    /// the way Goldfish's Connect wallet shows a mainnet key.
    fn abandon_export() -> String {
        let seed = bdk_wallet::bip39::Mnemonic::parse(
            "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about",
        )
        .unwrap()
        .to_seed("");
        let secp = Secp256k1::new();
        let root = Xpriv::new_master(NetworkKind::Main, &seed).unwrap();
        let path: DerivationPath = "m/84h/0h/0h".parse().unwrap();
        let mut raw = Xpub::from_priv(&secp, &root.derive_priv(&secp, &path).unwrap()).encode();
        raw[..4].copy_from_slice(&ZPUB);
        format!("[73c5da0a/84h/0h/0h]{}", base58::encode_check(&raw))
    }

    #[test]
    fn key_export_parses() {
        let a = parse_key_export(&abandon_export()).unwrap();
        assert_eq!(a.network, Network::Bitcoin);
        let mut w = a.wallet().unwrap();
        assert_eq!(
            w.reveal_next_address(KeychainKind::External).address.to_string(),
            "bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu"
        );
        assert!(parse_key_export("[73c5da0a/84h/1h/0h]xpub661MyMwAqRbcF").is_err());
    }

    #[test]
    fn psbt_round_trips_through_ur() {
        let a = parse_key_export(&abandon_export()).unwrap();
        let mut w = a.wallet().unwrap();
        fund_for_demo(&mut w, 100_000);
        let to = w.peek_address(KeychainKind::External, 5).address.to_string();
        let psbt = build_psbt(&mut w, &to, 30_000, 2.0).unwrap();
        let mut enc = psbt_encoder(&psbt).unwrap();
        let mut scan = PsbtScanner::default();
        let mut got = None;
        for _ in 0..200 {
            if let Some(p) = scan.receive(&enc.next_part().unwrap().to_uppercase()).unwrap() {
                got = Some(p);
                break;
            }
        }
        assert_eq!(got.unwrap(), psbt);
    }
    #[test]
    fn own_server_broadcasts_itself() {
        let own = |url: &str| ServerConfig { url: url.into(), proxy: String::new() }.own_server();
        for u in ["ssl://abc.onion:50002", "tcp://192.168.1.20:50001", "http://umbrel.local:3002/api", "tcp://[::1]:60401", "https://[fd00::1]/api"] {
            assert!(own(u), "{u}");
        }
        for u in ["", "ssl://electrum.blockstream.info:50002", "https://mempool.space/api", "ssl://8.8.8.8:50002"] {
            assert!(!own(u), "{u}");
        }
    }

    #[test]
    fn own_hosts_skip_domain_check() {
        for h in ["abc.onion", "umbrel.local", "localhost", "192.168.1.20", "10.0.0.5", "127.0.0.1", "[::1]", "[fd00::1]"] {
            assert!(own_host(h), "{h}");
        }
        for h in ["electrum.blockstream.info", "8.8.8.8", "[2001:db8::1]"] {
            assert!(!own_host(h), "{h}");
        }
    }

    #[test]
    fn server_urls_are_checked() {
        let bad = |url: &str, proxy: &str| ServerConfig { url: url.into(), proxy: proxy.into() }.connect(Network::Signet).err().unwrap();
        assert!(bad("electrum.example.com:50002", "").contains("ssl://"));
        assert!(bad("ssl://electrum.example.com", "").contains("port"));
        assert!(bad("https://mempool.space/api", "socks5://127.0.0.1:9050").contains("HTTPTunnelPort"));
        assert!(bad("ssl://127.0.0.1:1", "http://127.0.0.1:8080").contains("SOCKS5"));
        // building an Esplora client doesn't touch the network
        let esplora = ServerConfig { url: "https://mempool.space/signet/api".into(), proxy: String::new() };
        assert!(matches!(esplora.connect(Network::Signet), Ok(Backend::Esplora(_))));
    }

    #[test]
    fn server_on_another_chain_is_refused() {
        use bdk_wallet::bitcoin::constants::genesis_block;
        let g = |n: Network| genesis_block(n).block_hash();
        assert_eq!(
            network_mismatch(g(Network::Bitcoin), Network::Signet).unwrap(),
            "This server is on mainnet, but this wallet is on signet."
        );
        assert!(network_mismatch(g(Network::Signet), Network::Bitcoin).unwrap().contains("on signet"));
        assert!(network_mismatch("00".repeat(32).parse::<BlockHash>().unwrap(), Network::Bitcoin).is_some());
        for n in [Network::Signet, Network::Testnet4, Network::Regtest] {
            assert!(network_mismatch(g(n), Network::Signet).is_none(), "{n}");
        }
        assert!(network_mismatch(g(Network::Bitcoin), Network::Bitcoin).is_none());
        assert!(network_mismatch("00".repeat(32).parse::<BlockHash>().unwrap(), Network::Signet).is_none());
    }

    #[test]
    fn pending_send_can_be_bumped() {
        // A wallet that can sign for itself stands in for Goldfish here.
        let seed = bdk_wallet::bip39::Mnemonic::parse(
            "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about",
        )
        .unwrap()
        .to_seed("");
        let root = Xpriv::new_master(NetworkKind::Test, &seed).unwrap();
        let mut w = Wallet::create(format!("wpkh({root}/84h/1h/0h/0/*)"), format!("wpkh({root}/84h/1h/0h/1/*)"))
            .network(Network::Signet)
            .create_wallet_no_persist()
            .unwrap();
        fund_for_demo(&mut w, 100_000);
        let to = "tb1q6rz28mcfaxtmd6v789l9rrlrusdprr9pqcpvkl"; // someone else
        let mut psbt = build_psbt(&mut w, to, 30_000, 1.5).unwrap();
        #[allow(deprecated)]
        let signed = w.sign(&mut psbt, SignOptions { trust_witness_utxo: true, ..Default::default() }).unwrap();
        assert!(signed);
        let (old_fee, old_input) = (psbt.fee().unwrap(), psbt.unsigned_tx.input[0].previous_output);
        let tx = psbt.extract_tx().unwrap();
        let txid = tx.compute_txid();
        w.apply_unconfirmed_txs([(tx, 2)]);

        let row = overview(&abandon_account(), &w)
            .txs
            .into_iter()
            .find(|t| t.txid == txid.to_string())
            .unwrap();
        assert!(row.can_bump && !row.confirmed);
        assert!((row.fee_rate.unwrap() - 1.5).abs() < 0.2, "{:?}", row.fee_rate);

        let bump = build_bump(&mut w, &txid.to_string(), 5.0).unwrap();
        assert_eq!(bump.unsigned_tx.input[0].previous_output, old_input);
        assert!(bump.fee().unwrap() > old_fee);
        assert!(build_bump(&mut w, &txid.to_string(), 0.0).is_err());
    }

    #[test]
    fn receive_keeps_the_newest_address_until_it_is_paid() {
        let mut w = abandon_account().wallet().unwrap();
        assert_eq!(receive_address(&mut w).index, 0);
        assert_eq!(receive_address(&mut w).index, 0, "showing it again hands out nothing new");
        assert_eq!(new_receive_address(&mut w).unwrap().index, 1);
        assert_eq!(receive_address(&mut w).index, 1, "not back to the unused #0");
        assert_eq!(unused_in_a_row(&w), 2);

        // #0 gets paid: #1 is still waiting
        fund_for_demo(&mut w, 10_000);
        assert_eq!(unused_in_a_row(&w), 1);
        assert_eq!(receive_address(&mut w).index, 1);
    }

    #[test]
    fn new_addresses_stop_at_the_gap_limit() {
        let mut w = abandon_account().wallet().unwrap();
        receive_address(&mut w);
        for i in 1..GAP_LIMIT {
            assert_eq!(new_receive_address(&mut w).unwrap().index, i);
        }
        assert_eq!(unused_in_a_row(&w), GAP_LIMIT);
        assert!(new_receive_address(&mut w).is_err());
        assert_eq!(w.derivation_index(KeychainKind::External), Some(GAP_LIMIT - 1));

        // a payment to #0 makes room for one more
        fund_for_demo(&mut w, 10_000);
        assert_eq!(new_receive_address(&mut w).unwrap().index, GAP_LIMIT);
        assert!(new_receive_address(&mut w).is_err());
    }

    #[test]
    fn a_paid_newest_address_is_replaced() {
        let mut w = abandon_account().wallet().unwrap();
        fund_for_demo(&mut w, 10_000); // pays #0, as a sync would find it
        assert_eq!(unused_in_a_row(&w), 0);
        assert_eq!(receive_address(&mut w).index, 1);
    }

    fn abandon_account() -> Account {
        parse_key_export(&abandon_export()).unwrap()
    }

    /// A test cosigner: its root key and its BIP-48 multisig key export.
    fn cosigner(words: &str) -> (Xpriv, String) {
        let secp = Secp256k1::new();
        let seed = bdk_wallet::bip39::Mnemonic::parse(words).unwrap().to_seed("");
        let root = Xpriv::new_master(NetworkKind::Test, &seed).unwrap();
        let path: DerivationPath = "m/48h/1h/0h/2h".parse().unwrap();
        let xpub = Xpub::from_priv(&secp, &root.derive_priv(&secp, &path).unwrap());
        (root, format!("[{}/48h/1h/0h/2h]{xpub}", root.fingerprint(&secp)))
    }

    fn three_cosigners() -> Vec<(Xpriv, String)> {
        [
            "crew marble private differ race truly blush basket crater affair prepare unique",
            "zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo wrong",
            "legal winner thank year wave sausage worth useful legal winner thank yellow",
        ]
        .iter()
        .map(|w| cosigner(w))
        .collect()
    }

    #[test]
    fn multisig_from_keys_and_descriptors() {
        let c = three_cosigners();
        let keys: Vec<&str> = c.iter().map(|(_, k)| k.as_str()).collect();
        let a = multisig_from_keys(2, &keys).unwrap();
        assert_eq!((a.threshold, a.fingerprints.len(), a.network), (2, 3, Network::Signet));
        assert_eq!(a.kind(), "2 of 3 multisig");
        assert!(a.descriptor.starts_with("wsh(sortedmulti(2,[") && a.descriptor.contains("/<0;1>/*") && a.descriptor.contains('#'));

        // the same wallet in every spelling wallet apps use
        let body = a.descriptor.split('#').next().unwrap();
        for spelling in [
            a.descriptor.clone(),
            body.to_string(),
            body.replace("/<0;1>/*", "/0/*"),
            body.replace("/<0;1>/*", "/{0,1}/*"),
            body.replace("/<0;1>/*", ""),
            format!("  {}\n", a.descriptor),
        ] {
            assert_eq!(parse_wallet(&spelling).unwrap().descriptor, a.descriptor, "{spelling}");
        }

        // Goldfish's own multisig key comes as a Vpub (SLIP-132)
        let mut raw = base58::decode_check(c[0].1.split(']').nth(1).unwrap()).unwrap();
        raw[..4].copy_from_slice(&VPUB_MS);
        let vpub = format!("{}]{}", c[0].1.split(']').next().unwrap(), base58::encode_check(&raw));
        assert!(vpub.contains("]Vpub"));
        assert_eq!(multisig_from_keys(2, &[vpub.as_str(), keys[1], keys[2]]).unwrap().descriptor, a.descriptor);

        // what's refused
        let mut damaged = a.descriptor.clone();
        let last = damaged.pop().unwrap();
        damaged.push(if last == 'q' { 'p' } else { 'q' });
        assert!(parse_wallet(&damaged).unwrap_err().contains("checksum"));
        assert!(parse_wallet(&body.replace("sortedmulti", "multi")).unwrap_err().contains("sortedmulti"));
        assert!(multisig_from_keys(4, &keys).is_err());
        assert!(multisig_from_keys(2, &[keys[0], keys[0]]).unwrap_err().contains("twice"));
        let bare = keys[1].split(']').nth(1).unwrap();
        assert!(multisig_from_keys(2, &[keys[0], bare]).unwrap_err().contains("fingerprint"));
        assert!(parse_key_export(keys[0]).unwrap_err().contains("multisig"));
    }

    #[test]
    fn multisig_backup_lists_every_cosigner() {
        let c = three_cosigners();
        let keys: Vec<&str> = c.iter().map(|(_, k)| k.as_str()).collect();
        let a = multisig_from_keys(2, &keys).unwrap();
        let b = backup_sheet(&a, "2026-11-28").unwrap();
        assert_eq!((b.threshold, b.cosigners.len(), b.network.as_str()), (2, 3, "Signet"));
        assert_eq!(b.first_address, a.wallet().unwrap().peek_address(KeychainKind::External, 0).address.to_string());
        for (i, k) in keys.iter().enumerate() {
            let cs = &b.cosigners[i];
            assert!(cs.xpub.starts_with("tpub") && cs.path == "m/48h/1h/0h/2h", "{cs:?}");
            assert!(a.descriptor.contains(&format!("[{}/{}]{}/<0;1>/*", cs.fingerprint, &cs.path[2..], cs.xpub)));
            assert!(k.starts_with(&format!("[{}/", cs.fingerprint)));
        }

        // the file is the descriptor with its checksum, and reads back as the same wallet
        assert_eq!(b.text().trim(), a.descriptor);
        assert_eq!(parse_wallet(&b.text()).unwrap().descriptor, a.descriptor);

        // the sheet: every key, the first address, the descriptor (its <0;1> escaped), no secrets
        let html = b.html();
        // keys and address come in groups of four: strip the tags to find them whole
        let plain = html.replace("<i>", "").replace("</i>", "");
        for cs in &b.cosigners {
            assert!(plain.contains(&cs.fingerprint) && plain.contains(&cs.xpub));
        }
        assert!(plain.contains(&b.first_address) && html.contains("2026-11-28") && html.contains("Sign with any 2"));
        assert!(html.contains(&a.descriptor.replace('<', "&lt;").replace('>', "&gt;")) && !html.contains("<0;1>"));
        assert!(html.contains("No seed or private key") && html.contains("only 2 of the 3 seeds") && html.contains("<svg"));
        assert!(html.contains(&format!("<i>{}</i><i>{}</i>", &b.cosigners[0].fingerprint[..4], &b.cosigners[0].fingerprint[4..])));
        assert!(!html.contains("tprv") && !html.contains("src=") && !html.contains("<link"));

        // n of n: no "only" line; one signer: nothing to back up
        let all = backup_sheet(&multisig_from_keys(3, &keys).unwrap(), "2026-11-28").unwrap();
        assert!(!all.html().contains("only 3 of the 3"));
        assert!(backup_sheet(&abandon_account(), "2026-11-28").is_err());
    }

    #[test]
    fn two_of_three_collects_signatures() {
        let secp = Secp256k1::new();
        let c = three_cosigners();
        let keys: Vec<&str> = c.iter().map(|(_, k)| k.as_str()).collect();
        let a = multisig_from_keys(2, &keys).unwrap();
        let mut w = a.wallet().unwrap();
        fund_for_demo(&mut w, 100_000);
        let mut psbt = build_psbt(&mut w, "tb1q6rz28mcfaxtmd6v789l9rrlrusdprr9pqcpvkl", 30_000, 2.0).unwrap();

        let signed_by = |root: &Xpriv, base: &Psbt| {
            let mut p = base.clone();
            p.sign(root, &secp).unwrap();
            p
        };
        let first = signed_by(&c[0].0, &psbt);
        let (sigs, tx) = add_signatures(&w, &mut psbt, first.clone(), a.threshold).unwrap();
        assert!(tx.is_none());
        assert_eq!((sigs.have, sigs.needed), (1, 2));
        assert_eq!(sigs.signers, vec![a.fingerprints[0].clone()]);
        // the same signature again changes nothing
        assert_eq!(add_signatures(&w, &mut psbt, first, a.threshold).unwrap().0.have, 1);

        let third = signed_by(&c[2].0, &psbt);
        let (sigs, tx) = add_signatures(&w, &mut psbt, third, a.threshold).unwrap();
        assert_eq!(sigs.have, 2);
        assert_eq!(tx.unwrap().input[0].witness.len(), 4); // empty, two signatures, the script
    }


    #[test]
    fn a_send_to_yourself_is_not_change() {
        let mut w = abandon_account().wallet().unwrap();
        fund_for_demo(&mut w, 100_000);
        let own = w.peek_address(KeychainKind::External, 3).address.to_string();
        let psbt = build_psbt(&mut w, &own, 10_000, 1.0).unwrap();
        let r = review(&w, &psbt, 1);
        assert_eq!(r.recipients.len(), 1);
        assert_eq!((r.recipients[0].sat, r.recipients[0].yours), (10_000, true));
        assert!(r.change_sat > 0 && r.change_sat < 90_000);
    }


    #[test]
    fn too_small_and_too_much_say_so() {
        let mut w = abandon_account().wallet().unwrap();
        fund_for_demo(&mut w, 100_000);
        let to = w.peek_address(KeychainKind::External, 1).address.to_string(); // P2WPKH: dust below 294
        assert!(build_psbt(&mut w, &to, 100, 1.0).unwrap_err().contains("₿294"));
        assert!(build_psbt(&mut w, &to, 294, 1.0).is_ok());
        assert!(build_psbt(&mut w, &to, 200_000, 1.0).unwrap_err().starts_with("Not enough bitcoin"));
    }


    #[test]
    fn a_bad_signature_is_refused() {
        let secp = Secp256k1::new();
        let c = three_cosigners();
        let keys: Vec<&str> = c.iter().map(|(_, k)| k.as_str()).collect();
        let a = multisig_from_keys(2, &keys).unwrap();
        let mut w = a.wallet().unwrap();
        fund_for_demo(&mut w, 100_000);
        let mut psbt = build_psbt(&mut w, "tb1q6rz28mcfaxtmd6v789l9rrlrusdprr9pqcpvkl", 30_000, 2.0).unwrap();
        let mut signed = psbt.clone();
        signed.sign(&c[0].0, &secp).unwrap();
        // the same signature, claimed for another cosigner's key
        let (_, sig) = signed.inputs[0].partial_sigs.iter().next().map(|(k, v)| (*k, *v)).unwrap();
        let other = *signed.inputs[0].bip32_derivation.keys().find(|k| !signed.inputs[0].partial_sigs.keys().any(|p| p.inner == **k)).unwrap();
        signed.inputs[0].partial_sigs.clear();
        signed.inputs[0].partial_sigs.insert(bdk_wallet::bitcoin::PublicKey::new(other), sig);
        assert!(add_signatures(&w, &mut psbt, signed, 2).unwrap_err().contains("doesn't check out"));
        assert!(psbt.inputs[0].partial_sigs.is_empty(), "nothing merged");
    }

    #[test]
    fn a_cosigner_signs_from_a_file() {
        let secp = Secp256k1::new();
        let c = three_cosigners();
        let keys: Vec<&str> = c.iter().map(|(_, k)| k.as_str()).collect();
        let mut w = multisig_from_keys(2, &keys).unwrap().wallet().unwrap();
        fund_for_demo(&mut w, 100_000);
        let mut psbt = build_psbt(&mut w, "tb1q6rz28mcfaxtmd6v789l9rrlrusdprr9pqcpvkl", 30_000, 2.0).unwrap();
        assert!(psbt.inputs[0].non_witness_utxo.is_none());
        let export = psbt_for_export(&w, &psbt);
        assert!(export.inputs[0].non_witness_utxo.is_some(), "the previous transaction, for Trezor and Ledger");
        // read back as a binary file, base64 and hex
        assert_eq!(signed_from_file(&export.serialize(), &psbt).unwrap(), export);
        assert_eq!(signed_from_file(format!("{}\n", psbt_base64(&export)).as_bytes(), &psbt).unwrap(), export);
        let hex: String = export.serialize().iter().map(|b| format!("{b:02x}")).collect();
        assert_eq!(signed_from_file(hex.as_bytes(), &psbt).unwrap(), export);
        assert_eq!(signed_from_file(b"hello", &psbt).unwrap_err(), "That file isn't a signed transaction.");
        let mut signed = export;
        signed.sign(&c[1].0, &secp).unwrap();
        let (sigs, tx) = add_signatures(&w, &mut psbt, signed, 2).unwrap();
        assert_eq!((sigs.have, tx.is_none()), (1, true));
        assert!(psbt.inputs[0].non_witness_utxo.is_none(), "the next QR stays small");
    }

    /// Sparrow (or a Coldcard) adds the last signature and finalizes: the
    /// finished inputs count, after their signatures are checked.
    #[test]
    fn a_finalized_psbt_or_transaction_completes_the_signatures() {
        let secp = Secp256k1::new();
        let c = three_cosigners();
        let keys: Vec<&str> = c.iter().map(|(_, k)| k.as_str()).collect();
        let mut w = multisig_from_keys(2, &keys).unwrap().wallet().unwrap();
        fund_for_demo(&mut w, 100_000);
        let unsigned = build_psbt(&mut w, "tb1q6rz28mcfaxtmd6v789l9rrlrusdprr9pqcpvkl", 30_000, 2.0).unwrap();
        let mut first = unsigned.clone();
        first.sign(&c[0].0, &secp).unwrap();
        let mut ours = unsigned.clone();
        add_signatures(&w, &mut ours, first, 2).unwrap();
        // the other wallet signs and finalizes: no partial signatures left
        let mut finished = ours.clone();
        finished.sign(&c[1].0, &secp).unwrap();
        assert!(w.finalize_psbt(&mut finished, SignOptions { trust_witness_utxo: true, ..Default::default() }).unwrap());
        assert!(finished.inputs[0].partial_sigs.is_empty());
        let tx = finished.clone().extract_tx().unwrap();
        let (sigs, done) = add_signatures(&w, &mut ours.clone(), finished.clone(), 2).unwrap();
        assert_eq!(sigs.have, 2);
        assert_eq!(done.unwrap().compute_wtxid(), tx.compute_wtxid());
        // the finished transaction as a file (Coldcard's .txn), hex
        let hex: String = bdk_wallet::bitcoin::consensus::encode::serialize(&tx).iter().map(|b| format!("{b:02x}")).collect();
        let from_txn = signed_from_file(hex.as_bytes(), &unsigned).unwrap();
        let (sigs, done) = add_signatures(&w, &mut ours.clone(), from_txn, 2).unwrap();
        assert_eq!((sigs.have, done.unwrap().compute_wtxid()), (2, tx.compute_wtxid()));
        // the signatures in the wrong order: rebuilt from our script, it still completes
        let mut swapped = finished.clone();
        let mut items: Vec<Vec<u8>> = swapped.inputs[0].final_script_witness.as_ref().unwrap().to_vec();
        items.swap(1, 2);
        swapped.inputs[0].final_script_witness = Some(items.into());
        let (_, done) = add_signatures(&w, &mut ours.clone(), swapped, 2).unwrap();
        assert_eq!(done.unwrap().compute_wtxid(), tx.compute_wtxid());
        // a finalized witness with a signature that doesn't check out is refused
        let mut forged = finished;
        let mut items: Vec<Vec<u8>> = forged.inputs[0].final_script_witness.as_ref().unwrap().to_vec();
        let n = items[1].len();
        items[1][n - 3] ^= 1;
        forged.inputs[0].final_script_witness = Some(items.into());
        assert!(add_signatures(&w, &mut ours.clone(), forged, 2).unwrap_err().contains("doesn't check out"));
    }

    #[test]
    fn scanner_starts_over_on_another_message_of_the_same_length() {
        let a = parse_key_export(&abandon_export()).unwrap();
        let mut w = a.wallet().unwrap();
        fund_for_demo(&mut w, 100_000);
        let to = w.peek_address(KeychainKind::External, 5).address.to_string();
        let first = build_psbt(&mut w, &to, 30_000, 2.0).unwrap();
        let second = build_psbt(&mut w, &to, 31_000, 2.0).unwrap();
        let (mut e1, mut e2) = (psbt_encoder(&first).unwrap(), psbt_encoder(&second).unwrap());
        let mut scan = PsbtScanner::default();
        scan.receive(&e1.next_part().unwrap()).unwrap();
        let mut got = None;
        for _ in 0..200 {
            if let Some(p) = scan.receive(&e2.next_part().unwrap()).unwrap() {
                got = Some(p);
                break;
            }
        }
        assert_eq!(got.unwrap(), second);
    }

    #[test]
    fn socks5h_and_non_multisig_keys() {
        let c = three_cosigners();
        let single = c[0].1.replace("/48h/1h/0h/2h]", "/84h/1h/0h]");
        let keys = [single.as_str(), c[1].1.as_str(), c[2].1.as_str()];
        assert!(multisig_from_keys(2, &keys).unwrap_err().contains("isn't a multisig key"));
        let bad = ServerConfig { url: "ssl://127.0.0.1:1".into(), proxy: "socks5h://127.0.0.1:1".into() };
        assert!(!bad.connect(Network::Signet).err().unwrap_or_default().contains("SOCKS5 proxy, like"));
    }

    #[test]
    fn sealed_settings_open_only_with_the_password() {
        let plain = br#"{"key":"[73c5da0a/84h/0h/0h]zpub...","server":{"url":"","proxy":""}}"#;
        let sealed = seal(plain, "correct horse").unwrap();
        assert!(sealed.starts_with(b"age-encryption.org/v1"));
        assert!(!sealed.windows(4).any(|w| w == b"zpub"));
        assert_eq!(unseal(&sealed, "correct horse").unwrap(), plain);
        assert_eq!(unseal(&sealed, "wrong horse").unwrap_err(), "wrong password");
    }

    #[test]
    fn a_password_swaps_the_plain_file_for_a_locked_one() {
        let dir = std::env::temp_dir().join(format!("companion-settings-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let plain = br#"{"key":"k"}"#;
        let save = |pw: Option<&str>| {
            write_settings(&dir, plain, pw).unwrap();
            remove_other_settings(&dir, pw.is_some()).unwrap();
        };
        save(None);
        assert_eq!(std::fs::read(dir.join("settings.json")).unwrap(), plain);
        save(Some("pw"));
        assert!(!dir.join("settings.json").exists());
        assert_eq!(unseal(&std::fs::read(dir.join("settings.age")).unwrap(), "pw").unwrap(), plain);
        save(None);
        assert!(!dir.join("settings.age").exists());
        assert_eq!(std::fs::read(dir.join("settings.json")).unwrap(), plain);
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
