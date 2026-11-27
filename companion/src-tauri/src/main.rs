// Goldfish Companion: a watch-only wallet that builds transactions for
// Goldfish to sign over QR codes. Nothing here can spend on its own.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use std::sync::Mutex;

use std::path::PathBuf;

use bdk_wallet::bitcoin::{Psbt, Transaction};
use bdk_wallet::{KeychainKind, Wallet};
use companion::camera::Camera;
use companion::*;
use serde::{Deserialize, Serialize};
use tauri::{AppHandle, Emitter, Manager, State};

/// What survives a restart: the watch-only key and the server. Saved as
/// settings.json in the app's data folder. The seed is never here.
#[derive(Default, Clone, Serialize, Deserialize)]
struct Settings {
    key: Option<String>,
    server: ServerConfig,
}

fn settings_path(h: &AppHandle) -> Res<PathBuf> {
    Ok(h.path().app_data_dir().map_err(|e| e.to_string())?.join("settings.json"))
}

fn load_settings(h: &AppHandle) -> Settings {
    settings_path(h)
        .ok()
        .and_then(|p| std::fs::read(p).ok())
        .and_then(|b| serde_json::from_slice(&b).ok())
        .unwrap_or_default()
}

fn save_settings(h: &AppHandle, s: &Settings) -> Res<()> {
    let path = settings_path(h)?;
    std::fs::create_dir_all(path.parent().unwrap()).map_err(|e| e.to_string())?;
    // write a copy, then swap it in: a crash mid-write can't eat the saved wallet
    let tmp = path.with_extension("json.tmp");
    std::fs::write(&tmp, serde_json::to_vec_pretty(s).unwrap()).map_err(|e| e.to_string())?;
    std::fs::rename(&tmp, &path).map_err(|e| e.to_string())
}

#[derive(Default)]
struct App {
    settings: Settings,
    account: Option<Account>,
    wallet: Option<Wallet>,
    unsigned: Option<Psbt>,
    encoder: Option<ur::Encoder<'static>>,
    wallet_encoder: Option<ur::Encoder<'static>>,
    sigs: Signatures,
    scanner: PsbtScanner,
    signed: Option<Transaction>,
    camera: Option<Camera>,
}

type Shared = Mutex<App>;

fn loaded<'a>(app: &'a mut App) -> Res<(&'a Account, &'a mut Wallet)> {
    match (&app.account, &mut app.wallet) {
        (Some(a), Some(w)) => Ok((a, w)),
        _ => Err("no wallet yet: scan Goldfish's Connect wallet QR".into()),
    }
}

/// Load the wallet for `text` (a Connect wallet key) and remember it.
fn link(h: &AppHandle, app: &mut App, text: &str) -> Res<Overview> {
    link_account(h, app, parse_wallet(text)?)
}

fn link_account(h: &AppHandle, app: &mut App, account: Account) -> Res<Overview> {
    let wallet = account.wallet()?;
    let saved = account.saved.clone();
    let overview = overview(&account, &wallet);
    (app.account, app.wallet) = (Some(account), Some(wallet));
    (app.unsigned, app.encoder, app.signed) = (None, None, None);
    app.scanner = PsbtScanner::default();
    app.settings.key = Some(saved);
    save_settings(h, &app.settings)?;
    Ok(overview)
}

/// A multisig wallet from its cosigners' keys (one per line) and how many
/// must sign; or, when `keys` holds a whole descriptor, from that.
#[tauri::command]
fn link_multisig(threshold: usize, keys: String, handle: AppHandle, state: State<Shared>) -> Res<Overview> {
    let account = if keys.contains("multi(") {
        parse_descriptor(&keys)?
    } else {
        multisig_from_keys(threshold, &keys.lines().collect::<Vec<_>>())?
    };
    link_account(&handle, &mut state.lock().unwrap(), account)
}

#[derive(Serialize)]
struct ShowWallet {
    descriptor: String,
    kind: String,
    fingerprints: Vec<String>,
    first_address: String,
    /// a static QR when the descriptor fits one Goldfish reads easily,
    /// otherwise None and the page plays `next_wallet_frame`
    qr: Option<String>,
}

/// Longer descriptors go as an animated `bytes` UR, which Goldfish also reads.
const STATIC_DESCRIPTOR_MAX: usize = 500;

/// The multisig descriptor for Goldfish's Scan QR, so it can check
/// transactions and change against the wallet.
#[tauri::command]
fn wallet_descriptor(state: State<Shared>) -> Res<ShowWallet> {
    let mut app = state.lock().unwrap();
    let (a, w) = loaded(&mut app)?;
    let a = a.clone();
    let first_address = w.peek_address(KeychainKind::External, 0).address.to_string();
    let qr = if a.descriptor.len() <= STATIC_DESCRIPTOR_MAX { Some(qr_svg(&a.descriptor)?) } else { None };
    app.wallet_encoder = if qr.is_none() { Some(text_encoder(&a.descriptor)?) } else { None };
    Ok(ShowWallet { descriptor: a.descriptor.clone(), kind: a.kind(), fingerprints: a.fingerprints.clone(), first_address, qr })
}

#[tauri::command]
fn next_wallet_frame(state: State<Shared>) -> Res<Frame> {
    let mut app = state.lock().unwrap();
    let enc = app.wallet_encoder.as_mut().ok_or("no animated descriptor")?;
    let part = enc.next_part().map_err(|e| e.to_string())?;
    let (i, n) = part_indices(&part).unwrap_or((1, 1));
    Ok(Frame { svg: qr_svg(&part)?, part: (i - 1) % n + 1, total: n })
}

#[tauri::command]
fn import_key(text: String, handle: AppHandle, state: State<Shared>) -> Res<Overview> {
    link(&handle, &mut state.lock().unwrap(), &text)
}

#[derive(Serialize)]
struct Startup {
    wallet: Option<Overview>,
    server: ServerConfig,
}

/// What the page shows first: the saved wallet, if there is one.
#[tauri::command]
fn startup(state: State<Shared>) -> Startup {
    let app = state.lock().unwrap();
    let wallet = match (&app.account, &app.wallet) {
        (Some(a), Some(w)) => Some(overview(a, w)),
        _ => None,
    };
    Startup { wallet, server: app.settings.server.clone() }
}

/// Unlink: drop the saved key (and the wallet in memory). The server stays.
#[tauri::command]
fn forget_wallet(handle: AppHandle, state: State<Shared>) -> Res<()> {
    let mut app = state.lock().unwrap();
    (app.account, app.wallet, app.unsigned, app.encoder, app.signed) = (None, None, None, None, None);
    app.settings.key = None;
    save_settings(&handle, &app.settings)
}

/// Check the server answers, then save it.
#[tauri::command(async)]
fn set_server(url: String, proxy: String, handle: AppHandle, state: State<Shared>) -> Res<FeeRates> {
    let server = ServerConfig { url: url.trim().into(), proxy: proxy.trim().into() };
    let network = state.lock().unwrap().account.as_ref().map(|a| a.network).unwrap_or(bdk_wallet::bitcoin::Network::Signet);
    let backend = server.connect(network)?;
    backend.check_network(network)?;
    let rates = backend.fee_rates()?;
    let mut app = state.lock().unwrap();
    app.settings.server = server;
    save_settings(&handle, &app.settings)?;
    Ok(rates)
}

fn backend(state: &State<Shared>) -> Res<Backend> {
    let (server, network) = {
        let mut app = state.lock().unwrap();
        let network = loaded(&mut app)?.0.network;
        (app.settings.server.clone(), network)
    };
    let backend = server.connect(network)?;
    backend.check_network(network)?;
    Ok(backend)
}

#[tauri::command(async)]
fn fee_rates(state: State<Shared>) -> Res<FeeRates> {
    backend(&state)?.fee_rates()
}

#[tauri::command(async)]
fn sync(state: State<Shared>) -> Res<Overview> {
    let backend = backend(&state)?;
    let request = {
        let mut app = state.lock().unwrap();
        loaded(&mut app)?.1.start_full_scan().build()
    };
    let update = backend.full_scan(request)?;   // no lock held over the network
    let mut app = state.lock().unwrap();
    let (a, w) = loaded(&mut app)?;
    w.apply_update(update).map_err(|e| e.to_string())?;
    Ok(overview(a, w))
}

#[derive(Serialize)]
struct Receive {
    address: String,
    index: u32,
    qr: String,
}

#[tauri::command]
fn receive_address(state: State<Shared>) -> Res<Receive> {
    let mut app = state.lock().unwrap();
    let (_, w) = loaded(&mut app)?;
    let info = w.next_unused_address(KeychainKind::External);
    let address = info.address.to_string();
    // The QR is what Goldfish's Scan QR reads to verify the address is its own.
    Ok(Receive { qr: qr_svg(&address)?, address, index: info.index })
}

#[tauri::command]
fn create_send(to: String, amount_sat: u64, sat_per_vb: f64, state: State<Shared>) -> Res<Review> {
    let mut app = state.lock().unwrap();
    let (_, w) = loaded(&mut app)?;
    let psbt = build_psbt(w, &to, amount_sat, sat_per_vb)?;
    to_sign(&mut app, psbt)
}

/// Replace pending `txid` with the same payment at `sat_per_vb` (RBF).
#[tauri::command]
fn create_bump(txid: String, sat_per_vb: f64, state: State<Shared>) -> Res<Review> {
    let mut app = state.lock().unwrap();
    let (_, w) = loaded(&mut app)?;
    let psbt = build_bump(w, &txid, sat_per_vb)?;
    to_sign(&mut app, psbt)
}

/// Make `psbt` the transaction on screen for Goldfish to sign.
fn to_sign(app: &mut App, psbt: Psbt) -> Res<Review> {
    let w = app.wallet.as_ref().ok_or("no wallet")?;
    let mut encoder = psbt_encoder(&psbt)?;
    let frames = fragment_count(&encoder.next_part().map_err(|e| e.to_string())?);
    let review = review(w, &psbt, frames);
    app.unsigned = Some(psbt);
    app.encoder = Some(encoder);
    app.scanner = PsbtScanner::default();
    app.signed = None;
    app.sigs = Signatures { needed: app.account.as_ref().map(|a| a.threshold).unwrap_or(1), ..Default::default() };
    Ok(review)
}

#[derive(Serialize)]
struct Frame {
    svg: String,
    part: usize,
    total: usize,
}

/// The next frame of the animated PSBT, as SVG, and which of the pure
/// fragments it stands for ("2/8", as Goldfish shows under its own).
/// The page asks every ~250 ms.
#[tauri::command]
fn next_frame(state: State<Shared>) -> Res<Frame> {
    let mut app = state.lock().unwrap();
    let enc = app.encoder.as_mut().ok_or("nothing to sign")?;
    let part = enc.next_part().map_err(|e| e.to_string())?;
    let (i, n) = part_indices(&part).unwrap_or((1, 1));
    Ok(Frame { svg: qr_svg(&part)?, part: (i - 1) % n + 1, total: n })
}

#[derive(Serialize, Clone, Default)]
struct ScanEvent {
    percent: u8,
    error: Option<String>,
    signed_txid: Option<String>,
    /// set when a signed PSBT came in: who has signed so far
    sigs: Option<Signatures>,
}

fn feed(app: &mut App, text: &str) -> ScanEvent {
    let got = match app.scanner.receive(text) {
        Ok(got) => got,
        Err(e) => return ScanEvent { error: Some(e), ..app_progress(app) },
    };
    let Some(signed) = got else { return app_progress(app) };
    app.scanner = PsbtScanner::default();
    let needed = app.account.as_ref().map(|a| a.threshold).unwrap_or(1);
    let result = match (&app.wallet, &mut app.unsigned) {
        (Some(w), Some(psbt)) => add_signatures(w, psbt, signed, needed),
        _ => Err("scanned a signed PSBT, but nothing was sent to sign".into()),
    };
    match result {
        Ok((sigs, Some(tx))) => {
            let txid = tx.compute_txid().to_string();
            app.signed = Some(tx);
            app.encoder = None;
            app.sigs = sigs.clone();
            ScanEvent { percent: 100, signed_txid: Some(txid), sigs: Some(sigs), ..Default::default() }
        }
        Ok((sigs, None)) => {
            // Not enough yet: from now on show the PSBT with the signatures
            // so far, for the next cosigner.
            if let Some(psbt) = &app.unsigned {
                match psbt_encoder(psbt) {
                    Ok(enc) => app.encoder = Some(enc),
                    Err(e) => return ScanEvent { error: Some(e), ..Default::default() },
                }
            }
            app.sigs = sigs.clone();
            ScanEvent { percent: 100, sigs: Some(sigs), ..Default::default() }
        }
        Err(e) => ScanEvent { error: Some(e), ..Default::default() },
    }
}

fn app_progress(app: &App) -> ScanEvent {
    ScanEvent { percent: app.scanner.progress().percent, ..Default::default() }
}

/// Pasted text, for when there is no camera (or for tests).
#[tauri::command]
fn scan_text(text: String, state: State<Shared>) -> ScanEvent {
    feed(&mut state.lock().unwrap(), &text)
}

/// `mode` "key": Goldfish's Connect wallet QR (emits "key" with the wallet).
/// `mode` "psbt": the signed transaction (emits "scan" with progress).
#[tauri::command(async)]
fn scan_start(mode: String, handle: AppHandle, state: State<Shared>) -> Res<()> {
    let device = std::env::var("GOLDFISH_CAMERA").unwrap_or_else(|_| "/dev/video0".into());
    let h = handle.clone();
    let preview_handle = handle.clone();
    let camera = Camera::start(&device, move |text| {
        let shared = h.state::<Shared>();
        if mode == "text" {
            // a cosigner's key or a descriptor, for the page to place
            let _ = h.emit("text", text);
            return true;
        }
        if mode == "key" {
            let linked = link(&h, &mut shared.lock().unwrap(), &text);
            return match linked {
                Ok(ov) => {
                    let _ = h.emit("key", ov);
                    true
                }
                Err(e) => {
                    let _ = h.emit("scan", ScanEvent { error: Some(e), ..Default::default() });
                    false
                }
            };
        }
        let event = feed(&mut shared.lock().unwrap(), &text);
        // stop once a whole signed PSBT is in, enough signatures or not:
        // the next cosigner's QR is another scan
        let done = event.sigs.is_some();
        let _ = h.emit("scan", event);
        done
    }, move |preview| {
        let _ = preview_handle.emit("preview", preview);
    })?;
    // Take any old camera out under the lock, but drop it after: its thread
    // may be waiting on this same lock in its callback.
    let old = std::mem::replace(&mut state.lock().unwrap().camera, Some(camera));
    drop(old);
    Ok(())
}

#[tauri::command(async)]
fn scan_stop(state: State<Shared>) {
    // Take it out first: the camera thread locks the state, so it can't be
    // joined while we hold the lock.
    let camera = state.lock().unwrap().camera.take();
    drop(camera);
}

#[tauri::command(async)]
fn broadcast_signed(state: State<Shared>) -> Res<String> {
    let tx = state.lock().unwrap().signed.clone().ok_or("nothing signed")?;
    let txid = backend(&state)?.broadcast(&tx)?;
    let mut app = state.lock().unwrap();
    if let Some(w) = app.wallet.as_mut() {
        w.apply_unconfirmed_txs([(tx, std::time::UNIX_EPOCH.elapsed().unwrap().as_secs())]);
    }
    app.signed = None;
    app.unsigned = None;
    Ok(txid.to_string())
}

fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .manage(Shared::default())
        .setup(|app| {
            // Reopen the saved watch-only wallet, if any.
            let settings = load_settings(app.handle());
            let shared = app.state::<Shared>();
            let mut state = shared.lock().unwrap();
            if let Some(key) = settings.key.clone() {
                if let Ok(account) = parse_wallet(&key) {
                    state.wallet = account.wallet().ok();
                    state.account = state.wallet.as_ref().map(|_| account);
                }
            }
            state.settings = settings;
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            startup,
            forget_wallet,
            link_multisig,
            wallet_descriptor,
            next_wallet_frame,
            set_server,
            fee_rates,
            create_bump,
            import_key,
            sync,
            receive_address,
            create_send,
            next_frame,
            scan_text,
            scan_start,
            scan_stop,
            broadcast_signed,
        ])
        .run(tauri::generate_context!())
        .expect("error while running Goldfish Companion");
}
