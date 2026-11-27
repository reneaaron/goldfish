// Goldfish Companion: a watch-only wallet that builds transactions for
// Goldfish to sign over QR codes. Nothing here can spend on its own.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use std::sync::Mutex;

use std::path::PathBuf;

use bdk_wallet::bitcoin::{Psbt, Transaction};
use bdk_wallet::{AddressInfo, KeychainKind, Wallet};
use companion::camera::Camera;
use companion::*;
use age::secrecy::{ExposeSecret, SecretString};
use serde::{Deserialize, Serialize};
use tauri::{AppHandle, Emitter, Manager, State};

/// What survives a restart: the watch-only key and the server. Saved as
/// settings.json in the app's data folder, or, with a password, locked in
/// settings.age (age format, scrypt). The seed is never here.
#[derive(Default, Clone, Serialize, Deserialize)]
struct Settings {
    key: Option<String>,
    server: ServerConfig,
    /// The newest receive address handed out, so a restart doesn't show an
    /// older one again before any coins have reached it.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    receive_index: Option<u32>,
}

fn settings_path(h: &AppHandle, ext: &str) -> Res<PathBuf> {
    Ok(h.path().app_data_dir().map_err(|e| e.to_string())?.join("settings").with_extension(ext))
}

fn load_settings(h: &AppHandle) -> Settings {
    settings_path(h, "json")
        .ok()
        .and_then(|p| std::fs::read(p).ok())
        .and_then(|b| serde_json::from_slice(&b).ok())
        .unwrap_or_default()
}

fn is_locked_on_disk(h: &AppHandle) -> bool {
    settings_path(h, "age").map(|p| p.exists()).unwrap_or(false)
}

fn unlocked(app: &App) -> Res<()> {
    if app.locked {
        return Err("Dove is locked: enter your password first".into());
    }
    Ok(())
}

/// One save at a time, so an older snapshot can't overwrite a newer one.
static SAVING: Mutex<()> = Mutex::new(());

/// Save the settings as they are now. Call with SAVING held and the app
/// unlocked (state lock not held): the snapshot is taken under the app lock,
/// then sealed and written without it, since scrypt takes a while and the
/// camera thread needs the lock meanwhile. Err((written, e)): `written` when
/// the new file is in place and only the other one couldn't be deleted.
fn save_holding(h: &AppHandle) -> Result<(), (bool, String)> {
    let (plain, password) = {
        let shared = h.state::<Shared>();
        let app = shared.lock().unwrap();
        unlocked(&app).map_err(|e| (false, e))?;
        (serde_json::to_vec_pretty(&app.settings).unwrap(), app.password.clone())
    };
    let dir = h.path().app_data_dir().map_err(|e| (false, e.to_string()))?;
    let password = password.as_ref().map(|p| p.expose_secret());
    write_settings(&dir, &plain, password).map_err(|e| (false, e))?;
    remove_other_settings(&dir, password.is_some()).map_err(|e| (true, e))
}

fn save_settings(h: &AppHandle) -> Res<()> {
    let _saving = SAVING.lock().unwrap();
    save_holding(h).map_err(|(_, e)| e)
}

/// Open the saved wallet in `settings`, if any.
fn open_settings(app: &mut App, settings: Settings) {
    if let Some(key) = settings.key.clone() {
        if let Ok(account) = parse_wallet(&key) {
            app.wallet = account.wallet().ok();
            if let (Some(w), Some(i)) = (app.wallet.as_mut(), settings.receive_index) {
                let _ = w.reveal_addresses_to(KeychainKind::External, i);
            }
            app.account = app.wallet.as_ref().map(|_| account);
        }
    }
    app.settings = settings;
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
    /// settings.age is there but not opened yet
    locked: bool,
    /// kept while the app runs, to lock the settings again on each save
    password: Option<SecretString>,
    /// the checked firmware.uf2, waiting for the board's drive
    firmware: Option<Vec<u8>>,
}

type Shared = Mutex<App>;

fn loaded<'a>(app: &'a mut App) -> Res<(&'a Account, &'a mut Wallet)> {
    match (&app.account, &mut app.wallet) {
        (Some(a), Some(w)) => Ok((a, w)),
        _ => Err("no wallet yet: scan Goldfish's Connect wallet QR".into()),
    }
}

/// Load the wallet for `text` (a Connect wallet key). The caller saves it,
/// after letting go of the app lock.
fn link(app: &mut App, text: &str) -> Res<Overview> {
    link_account(app, parse_wallet(text)?)
}

fn link_account(app: &mut App, account: Account) -> Res<Overview> {
    unlocked(app)?;
    let wallet = account.wallet()?;
    let saved = account.saved.clone();
    let overview = overview(&account, &wallet);
    (app.account, app.wallet) = (Some(account), Some(wallet));
    (app.unsigned, app.encoder, app.signed) = (None, None, None);
    app.scanner = PsbtScanner::default();
    if app.settings.key.as_ref() != Some(&saved) {
        app.settings.receive_index = None;
    }
    app.settings.key = Some(saved);
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
    let overview = link_account(&mut state.lock().unwrap(), account)?;
    save_settings(&handle)?;
    Ok(overview)
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

/// Multisig backup, saved at `path`: the descriptor as a text file, or the
/// sheet to print (`date` is today's, from the page), which then opens in
/// the browser.
fn backup(state: &State<Shared>, date: &str) -> Res<BackupSheet> {
    let mut app = state.lock().unwrap();
    backup_sheet(loaded(&mut app)?.0, date)
}

/// The save dialog doesn't always add the filter's extension; the sheet
/// needs `.html` to open in the browser.
fn with_extension(path: String, ext: &str) -> PathBuf {
    let p = PathBuf::from(path);
    if p.extension().is_some() { p } else { p.with_extension(ext) }
}

#[tauri::command]
fn save_descriptor(path: String, state: State<Shared>) -> Res<()> {
    let path = with_extension(path, "txt");
    std::fs::write(&path, backup(&state, "")?.text()).map_err(|e| format!("Couldn't save the file: {e}"))
}

#[tauri::command]
fn save_backup_sheet(path: String, date: String, state: State<Shared>) -> Res<()> {
    let path = with_extension(path, "html");
    std::fs::write(&path, backup(&state, &date)?.html()).map_err(|e| format!("Couldn't save the file: {e}"))?;
    tauri_plugin_opener::open_path(&path, None::<&str>).map_err(|e| format!("Saved, but couldn't open it: {e}"))
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
    let overview = link(&mut state.lock().unwrap(), &text)?;
    save_settings(&handle)?;
    Ok(overview)
}

#[derive(Serialize)]
struct Startup {
    wallet: Option<Overview>,
    server: ServerConfig,
    /// the settings are locked: ask for the password first
    locked: bool,
    has_password: bool,
}

fn startup_of(app: &App) -> Startup {
    let wallet = match (&app.account, &app.wallet) {
        (Some(a), Some(w)) => Some(overview(a, w)),
        _ => None,
    };
    Startup { wallet, server: app.settings.server.clone(), locked: app.locked, has_password: app.password.is_some() }
}

/// What the page shows first: the saved wallet, if there is one.
#[tauri::command]
fn startup(state: State<Shared>) -> Startup {
    startup_of(&state.lock().unwrap())
}

/// Open settings.age with `password`, then the wallet in it.
#[tauri::command(async)]
fn unlock(password: String, handle: AppHandle, state: State<Shared>) -> Res<Startup> {
    let sealed = std::fs::read(settings_path(&handle, "age")?).map_err(|e| e.to_string())?;
    let plain = unseal(&sealed, &password)?;   // scrypt takes a while: no lock held
    let settings: Settings = serde_json::from_slice(&plain).map_err(|e| e.to_string())?;
    let mut app = state.lock().unwrap();
    open_settings(&mut app, settings);
    (app.locked, app.password) = (false, Some(password.into()));
    Ok(startup_of(&app))
}

/// Forgot the password: delete settings.age. Only the watch-only key and the
/// server were in it; the wallet links again from the signer.
#[tauri::command]
fn start_over(handle: AppHandle, state: State<Shared>) -> Res<()> {
    let mut app = state.lock().unwrap();
    match std::fs::remove_file(settings_path(&handle, "age")?) {
        Err(e) if e.kind() != std::io::ErrorKind::NotFound => return Err(e.to_string()),
        _ => {}
    }
    app.locked = false;
    Ok(())
}

/// Lock the settings with `password`, or, when it's empty, save them plain.
#[tauri::command(async)]
fn set_password(password: String, handle: AppHandle, state: State<Shared>) -> Res<()> {
    let password = (!password.is_empty()).then(|| SecretString::from(password));
    let _saving = SAVING.lock().unwrap();
    let before = {
        let mut app = state.lock().unwrap();
        unlocked(&app)?;
        std::mem::replace(&mut app.password, password)
    };
    match save_holding(&handle) {
        // nothing written: the old file and password still go together
        Err((false, e)) => {
            state.lock().unwrap().password = before;
            Err(e)
        }
        r => r.map_err(|(_, e)| e),
    }
}

/// Unlink: drop the saved key (and the wallet in memory). The server stays.
#[tauri::command]
fn forget_wallet(handle: AppHandle, state: State<Shared>) -> Res<()> {
    {
        let mut app = state.lock().unwrap();
        unlocked(&app)?;
        (app.account, app.wallet, app.unsigned, app.encoder, app.signed) = (None, None, None, None, None);
        (app.settings.key, app.settings.receive_index) = (None, None);
    }
    save_settings(&handle)
}

/// Check the server answers, then save it.
#[tauri::command(async)]
fn set_server(url: String, proxy: String, handle: AppHandle, state: State<Shared>) -> Res<FeeRates> {
    let server = ServerConfig { url: url.trim().into(), proxy: proxy.trim().into() };
    let network = state.lock().unwrap().account.as_ref().map(|a| a.network).unwrap_or(bdk_wallet::bitcoin::Network::Signet);
    let backend = server.connect(network)?;
    backend.check_network(network)?;
    let rates = backend.fee_rates()?;
    {
        let mut app = state.lock().unwrap();
        unlocked(&app)?;
        app.settings.server = server;
    }
    save_settings(&handle)?;
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
    /// false once GAP_LIMIT addresses are waiting unused
    more: bool,
}

/// Show the address `pick` chooses, saving how far addresses have been
/// handed out when that grew.
fn show_receive(handle: AppHandle, state: State<Shared>, pick: fn(&mut Wallet) -> Res<AddressInfo>) -> Res<Receive> {
    let (receive, grew) = {
        let mut app = state.lock().unwrap();
        let (_, w) = loaded(&mut app)?;
        let info = pick(w)?;
        let more = unused_in_a_row(w) < GAP_LIMIT;
        let address = info.address.to_string();
        // The QR is what Goldfish's Scan QR reads to verify the address is its own.
        let receive = Receive { qr: qr_svg(&address)?, address, index: info.index, more };
        let grew = app.settings.receive_index < Some(info.index);
        if grew {
            app.settings.receive_index = Some(info.index);
        }
        (receive, grew)
    };
    if grew {
        save_settings(&handle)?;
    }
    Ok(receive)
}

#[tauri::command(async)]
fn receive_address(handle: AppHandle, state: State<Shared>) -> Res<Receive> {
    show_receive(handle, state, |w| Ok(companion::receive_address(w)))
}

#[tauri::command(async)]
fn new_receive_address(handle: AppHandle, state: State<Shared>) -> Res<Receive> {
    show_receive(handle, state, companion::new_receive_address)
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
    // start Tor while the PSBT goes to the signer and back, so Broadcast
    // doesn't wait for it; only once a payment is on the way, not for
    // someone just looking at the wallet
    static WARMING: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
    if over_tor(app) && !WARMING.swap(true, std::sync::atomic::Ordering::Relaxed) {
        std::thread::spawn(|| {
            if let Err(e) = tor::proxy() {
                eprintln!("tor: {e}");
            }
            WARMING.store(false, std::sync::atomic::Ordering::Relaxed);
        });
    }
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
    take_signed(app, signed)
}

/// A signed PSBT, whole: from the camera, pasted text or a file.
fn take_signed(app: &mut App, signed: Psbt) -> ScanEvent {
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

/// The PSBT to sign, with the signatures so far, for a signer that isn't
/// Goldfish: as base64 text to copy, or saved as a binary .psbt at `path`.
fn export(app: &App) -> Res<Psbt> {
    let psbt = app.unsigned.as_ref().ok_or("nothing to sign")?;
    let w = app.wallet.as_ref().ok_or("no wallet")?;
    Ok(psbt_for_export(w, psbt))
}

#[tauri::command]
fn psbt_text(state: State<Shared>) -> Res<String> {
    Ok(psbt_base64(&export(&state.lock().unwrap())?))
}

#[tauri::command]
fn save_psbt(path: String, state: State<Shared>) -> Res<()> {
    let psbt = export(&state.lock().unwrap())?;
    std::fs::write(&path, psbt.serialize()).map_err(|e| format!("Couldn't save the file: {e}"))
}

/// A signed PSBT file from another signer (Sparrow, a Coldcard's SD card).
#[tauri::command]
fn open_psbt(path: String, state: State<Shared>) -> ScanEvent {
    let read = std::fs::read(&path).map_err(|e| format!("Couldn't open the file: {e}"));
    let mut app = state.lock().unwrap();
    let Some(unsigned) = &app.unsigned else {
        return ScanEvent { error: Some("nothing to sign".into()), ..Default::default() };
    };
    match read.and_then(|b| signed_from_file(&b, unsigned)) {
        Ok(signed) => take_signed(&mut app, signed),
        Err(e) => ScanEvent { error: Some(e), ..Default::default() },
    }
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
            let linked = link(&mut shared.lock().unwrap(), &text);
            let linked = linked.and_then(|ov| save_settings(&h).map(|_| ov));
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

/// Whether Broadcast goes to peers over the built-in Tor (see p2p.rs): not
/// for your own server, which gets it itself, nor on regtest, which has no
/// public peers.
fn over_tor(app: &App) -> bool {
    !app.settings.server.own_server() && app.wallet.as_ref().is_some_and(|w| w.network() != bdk_wallet::bitcoin::Network::Regtest)
}

/// Wait until Tor is up (at once if it already is), for the Signed screen to
/// say what it's waiting on. False when Broadcast won't use Tor.
#[tauri::command(async)]
fn tor_ready(state: State<Shared>) -> Res<bool> {
    if !over_tor(&state.lock().unwrap()) {
        return Ok(false);
    }
    tor::proxy().map(|_| true)
}

/// `via_server`: through the wallet's server after all, when Tor or the
/// peers couldn't be reached and the user chose to.
#[tauri::command(async)]
fn broadcast_signed(via_server: bool, state: State<Shared>) -> Res<String> {
    let (tx, network, start, tor) = {
        let mut app = state.lock().unwrap();
        let tor = !via_server && over_tor(&app);
        let tx = app.signed.clone().ok_or("nothing signed")?;
        let w = loaded(&mut app)?.1;
        (tx, w.network(), p2p::start(w), tor)
    };
    let txid = if tor { p2p::broadcast(network, start, Some(tor::proxy()?), &tx)? } else { backend(&state)?.broadcast(&tx)? };
    let mut app = state.lock().unwrap();
    if let Some(w) = app.wallet.as_mut() {
        w.apply_unconfirmed_txs([(tx, std::time::UNIX_EPOCH.elapsed().unwrap().as_secs())]);
    }
    app.signed = None;
    app.unsigned = None;
    Ok(txid.to_string())
}

/// Download the latest release's firmware and check it; kept until installed.
#[tauri::command(async)]
fn firmware_check(state: State<Shared>) -> Res<firmware::Release> {
    let proxy = state.lock().unwrap().settings.server.proxy.clone();
    let (release, uf2) = firmware::download_latest(proxy.trim())?;
    state.lock().unwrap().firmware = Some(uf2);
    Ok(release)
}

/// The board's RP2350 drive, once Goldfish restarted into it. Off the main
/// thread: a mount going away as the board restarts can block for a moment.
#[tauri::command(async)]
fn firmware_drive() -> Option<String> {
    firmware::find_drive(&firmware::mount_roots()).map(|p| p.display().to_string())
}

#[tauri::command(async)]
fn firmware_install(state: State<Shared>) -> Res<()> {
    let drive = firmware::find_drive(&firmware::mount_roots()).ok_or("Goldfish's drive is gone. Restart it into the update screen again.")?;
    let uf2 = state.lock().unwrap().firmware.clone().ok_or("Check for the update first")?;
    firmware::write(&drive, &uf2)
}

fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_dialog::init())
        .manage(Shared::default())
        .setup(|app| {
            if let Ok(dir) = app.path().app_data_dir() {
                tor::set_dir(dir.join("tor"));
            }
            // Reopen the saved watch-only wallet, if any; a locked one waits
            // for the password.
            let shared = app.state::<Shared>();
            let mut state = shared.lock().unwrap();
            if is_locked_on_disk(app.handle()) {
                state.locked = true;
            } else {
                open_settings(&mut state, load_settings(app.handle()));
            }
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            startup,
            unlock,
            start_over,
            set_password,
            forget_wallet,
            link_multisig,
            wallet_descriptor,
            next_wallet_frame,
            save_descriptor,
            save_backup_sheet,
            set_server,
            fee_rates,
            create_bump,
            import_key,
            sync,
            receive_address,
            new_receive_address,
            create_send,
            next_frame,
            scan_text,
            psbt_text,
            save_psbt,
            open_psbt,
            scan_start,
            scan_stop,
            tor_ready,
            broadcast_signed,
            firmware_check,
            firmware_drive,
            firmware_install,
        ])
        .run(tauri::generate_context!())
        .expect("error while running Goldfish Companion");
}
