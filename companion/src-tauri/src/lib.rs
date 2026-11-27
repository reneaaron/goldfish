//! The companion's wallet logic, without any UI: the watch-only wallet built
//! from Goldfish's Connect wallet QR, PSBTs as animated UR QR codes out, the
//! signed PSBT back in, finalize and broadcast. The seed never comes here.

pub mod camera;

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
/// each `[fingerprint/path]key` (Goldfish's multisig Connect wallet key is one).
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

// ------------------------------------------------------------ chain

/// Where the wallet syncs and broadcasts: an Esplora API (`https://…`) or an
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

    pub fn broadcast(&self, tx: &Transaction) -> Res<Txid> {
        match self {
            Backend::Esplora(c) => c.broadcast(tx).map_err(err)?,
            Backend::Electrum(c) => {
                c.transaction_broadcast(tx).map_err(err)?;
            }
        }
        Ok(tx.compute_txid())
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

/// Merge a signed copy of `psbt` (from Goldfish or another cosigner) into
/// it. Once `needed` keys have signed every input, also the finished
/// transaction, ready to broadcast.
pub fn add_signatures(wallet: &Wallet, psbt: &mut Psbt, signed: Psbt, needed: usize) -> Res<(Signatures, Option<Transaction>)> {
    if signed.unsigned_tx.compute_txid() != psbt.unsigned_tx.compute_txid() {
        return Err("That's a different transaction. Sign this one on Goldfish, then scan again.".into());
    }
    let mut merged = psbt.clone();
    merged.combine(signed).map_err(err)?;
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
pub fn qr_svg(text: &str) -> Res<String> {
    let text = if text.to_lowercase().starts_with("ur:") { text.to_uppercase() } else { text.to_string() };
    let code = qrcode::QrCode::with_error_correction_level(text, qrcode::EcLevel::L).map_err(err)?;
    Ok(code
        .render::<qrcode::render::svg::Color>()
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
