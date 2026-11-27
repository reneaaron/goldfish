// Goldfish Companion page. All wallet logic is in Rust (src-tauri); this only
// switches views and calls commands.
const { invoke } = window.__TAURI__.core;
const { listen } = window.__TAURI__.event;

const $ = (id) => document.getElementById(id);
const VIEWS = ["unlock", "link", "wallet", "receive", "send", "bump", "settings", "update", "show", "sign", "ready", "sent"];
let waiting = null; // txid of a signed transaction not broadcast yet
let current = null; // the linked wallet's overview
let showTimer = null;
let server = { url: "", proxy: "" };
let hasPassword = false;
let frameTimer = null;
let scanning = false;
let fwTimer = null; // looks for Goldfish's RP2350 drive while Update is open
let fwRelease = null; // the checked release, once downloaded
let fwBusy = false; // copying: drive polls still in flight must not touch the button

// On a wide window the wallet stays put and every other screen opens as a
// dialog over it; on a narrow one each screen takes the whole window.
const wide = matchMedia("(min-width: 900px)");
let currentView = "link";
function layout() {
  const dialog = wide.matches && !!current && currentView !== "wallet" && currentView !== "link";
  for (const v of VIEWS) $("v-" + v).hidden = !(v === currentView || (dialog && v === "wallet"));
  document.body.classList.toggle("dialog-open", dialog);
  $("backdrop").hidden = !dialog;
  $("v-" + currentView).setAttribute("role", dialog ? "dialog" : "region");
}
wide.addEventListener("change", layout);
$("backdrop").onclick = () => show("wallet");
document.addEventListener("keydown", (e) => {
  if (e.key === "Escape" && !$("sign-menu").hidden) return menu(false);
  if (e.key === "Escape" && document.body.classList.contains("dialog-open")) show("wallet");
});

function show(view) {
  currentView = view;
  layout();
  if (view !== "sign") stopFrames();
  if (view !== "show") clearInterval(showTimer);
  if (view !== "update") clearInterval(fwTimer);
  if (view === "wallet") $("w-waiting").hidden = !waiting;
  if (scanning) stopScan();
  error("");
  window.scrollTo(0, 0);
}

// Errors in words a person can act on, with the technical detail under it,
// shown under the open screen's header.
function friendly(raw) {
  const s = String(raw);
  if (/timed out|timeout|refused|unreachable|could not connect|failed to lookup|dns error|resolve|os error (101|110|111|113)|IoError|Minreq|HttpResponse|ConnectionReset|broken pipe/i.test(s))
    return ["Can't reach the server. Check your internet connection, or the server in Settings.", s];
  return [s.charAt(0).toUpperCase() + s.slice(1), ""];
}
function error(e) {
  const el = $("error");
  el.hidden = !e;
  if (!e) return;
  const [text, detail] = friendly(e);
  $("error-text").textContent = text;
  $("error-detail").textContent = detail;
  // the screen in front: the dialog when one is open, not the wallet behind it
  const open = $("v-" + currentView);
  const bar = open?.querySelector(".bar");
  if (bar) bar.after(el);
  else if (open) open.prepend(el);
}
$("error-close").onclick = () => error("");

async function call(cmd, args) {
  error("");
  try {
    return await invoke(cmd, args);
  } catch (e) {
    error(e);
    throw e;
  }
}

// Amounts as Goldfish shows them: BIP177 ₿ (whole sats), BTC underneath.
const btc = (sat) => "₿" + Math.abs(sat).toLocaleString("en-US");
const inBtc = (sat) => (sat / 1e8).toFixed(8) + " BTC";
const fingerprint = (fp) => fp.slice(0, 4) + " " + fp.slice(4);
const groups = (a) => a.match(/.{1,4}/g).join(" ");
// "14:02" today, "26 Nov" this year, "26 Nov 2025" before
function when(t) {
  if (!t) return "";
  const d = new Date(t * 1000);
  const now = new Date();
  if (d.toDateString() === now.toDateString()) return d.toLocaleTimeString("en-GB", { hour: "2-digit", minute: "2-digit" });
  return d.toLocaleDateString("en-GB", { day: "numeric", month: "short", year: d.getFullYear() === now.getFullYear() ? undefined : "numeric" });
}
// "4/11" padded to " 4/11" with a figure space, so the caption keeps its width
const partOf = (f) => String(f.part).padStart(String(f.total).length, "\u2007") + "/" + f.total;
const shortId = (id) => id.slice(0, 8) + "…" + id.slice(-4);

function button(el, label, busy) {
  el.disabled = busy;
  el.querySelector("span").textContent = label;
}

// ---- link
async function startScan(mode) {
  await stopScan();
  await call("scan_start", { mode });
  scanning = true;
  camera(true);
}
async function stopScan() {
  camera(false);
  if (!scanning) return;
  scanning = false;
  await invoke("scan_stop");
}

// ---- camera preview
function camera(on) {
  // only the camera box of the screen in front: Link and Review each have one
  for (const c of document.querySelectorAll(".cam, .cam-cancel, .cam-msg")) c.hidden = true;
  if (on) for (const c of $("v-" + currentView).querySelectorAll(".cam, .cam-cancel")) c.hidden = false;
  // on Link, the camera takes the steps' place (the multisig panel keeps its own)
  if ($("link-ms").hidden && $("link-paste").hidden) $("link-intro").hidden = on;
  $("sign-show").hidden = on;
  if (!on) {
    button($("link-scan"), "Scan QR", false);
    button($("ms-scan"), "Scan a key or descriptor", false);
    button($("sign-scan"), $("sign-scan").dataset.label || "Scan signed QR", false);
  }
}
for (const b of document.querySelectorAll(".cam-cancel")) b.onclick = () => stopScan();
listen("preview", ({ payload: p }) => {
  const canvas = $("v-" + currentView).querySelector(".cam:not([hidden]) canvas");
  if (!canvas) return;
  canvas.width = p.width;
  canvas.height = p.height;
  const ctx = canvas.getContext("2d");
  const grey = atob(p.grey);
  const img = ctx.createImageData(p.width, p.height);
  for (let i = 0; i < grey.length; i++) {
    const v = grey.charCodeAt(i);
    img.data[4 * i] = img.data[4 * i + 1] = img.data[4 * i + 2] = v;
    img.data[4 * i + 3] = 255;
  }
  ctx.putImageData(img, 0, 0);
  ctx.lineWidth = 3;
  ctx.lineJoin = "round";
  for (const code of p.codes) {
    ctx.strokeStyle = code.read ? "#15803D" : "#0A7ED0";
    ctx.beginPath();
    code.corners.forEach(([x, y], i) => (i ? ctx.lineTo(x, y) : ctx.moveTo(x, y)));
    ctx.closePath();
    ctx.stroke();
  }
});

function renderOverview(o) {
  current = o;
  // "2 of 3" (+ " multisig" where there's room), or the single key's fingerprint
  $("fp").textContent = o.multisig ? o.kind.replace(" multisig", "") : fingerprint(o.fingerprints[0]);
  $("fp-more").textContent = o.multisig ? " multisig" : "";
  $("fp-pill").classList.toggle("mono", !o.multisig);
  $("fp-pill").title = o.multisig ? o.kind : "Your signer's fingerprint";
  $("fp-pill").querySelector("use").setAttribute("href", o.multisig ? "#i-users" : "#i-fingerprint");
  $("w-show").hidden = !o.multisig;
  $("net").hidden = o.network === "bitcoin";
  $("net").textContent = o.network.charAt(0).toUpperCase() + o.network.slice(1); // "Signet"
  $("bal").textContent = btc(o.confirmed_sat);
  $("bal-btc").textContent = inBtc(o.confirmed_sat);
  $("bal-pending").hidden = !o.pending_sat;
  // unconfirmed, either way: incoming, or the change of our own send
  $("bal-pending").textContent = `${btc(o.pending_sat)} unconfirmed`;
  $("w-waiting").hidden = !waiting;
  $("txs").replaceChildren(
    ...o.txs.map((t) => {
      const incoming = t.net_sat >= 0;
      const li = document.createElement("li");
      li.innerHTML = `<span class="ic"><svg><use href="#i-out"/></svg></span>
        <span class="what"><span></span><small></small></span><span class="right"><span class="amt"></span></span>`;
      if (t.can_bump) {
        const b = document.createElement("button");
        b.className = "bump";
        b.innerHTML = `<svg><use href="#i-zap"/></svg>Speed up`;
        b.onclick = () => openBump(t);
        li.querySelector(".right").append(b);
      }
      if (incoming) {
        li.querySelector(".ic").classList.add("in");
        li.querySelector("use").setAttribute("href", "#i-in");
        li.querySelector(".amt").classList.add("in");
      }
      li.querySelector(".what span").textContent = (incoming ? "Received" : "Sent") + (t.confirmed ? "" : " · pending");
      li.querySelector(".what small").textContent = [
        shortId(t.txid),
        when(t.time),
        !incoming && t.fee_rate ? `${t.fee_rate.toFixed(1)} sat/vB` : "",
      ].filter(Boolean).join(" · ");
      li.querySelector(".amt").textContent = (incoming ? "+" : "−") + btc(t.net_sat);
      return li;
    }),
  );
  renderEmpty();
}

// What the Activity list says while it has nothing to list: still looking,
// nothing there, or the server out of reach.
let syncState = "idle"; // "syncing" | "ok" | "failed" | "idle"
const EMPTY = {
  syncing: {
    icon: "i-sync", spin: true,
    title: "Looking for transactions",
    text: "This can take a minute the first time.",
  },
  ok: {
    icon: "i-in",
    title: "No transactions yet",
    text: "Bitcoin you receive to this wallet shows up here.",
    actions: [["receive", "secondary", "i-in", "Receive"]],
  },
  failed: {
    icon: "i-cloud-off", tone: "warn",
    title: "Can't reach the server",
    text: "Your wallet is fine. Check your connection, or pick another server in Settings.",
    actions: [["retry", "secondary", "i-sync", "Try again"], ["settings", "link", null, "Settings"]],
  },
};
function renderEmpty() {
  const el = $("txs-empty");
  el.hidden = (current?.txs.length ?? 0) > 0;
  if (el.hidden) return;
  const st = EMPTY[syncState] || EMPTY.ok;
  el.innerHTML = `<span class="empty-badge${st.spin ? " spin" : ""}${st.tone ? " " + st.tone : ""}"><svg><use href="#${st.icon}"/></svg></span>
    <b>${st.title}</b><span>${st.text}</span>
    ${st.actions ? `<span class="empty-actions">${st.actions
      .map(([act, cls, icon, label]) => `<button class="${cls}" data-act="${act}">${icon ? `<svg><use href="#${icon}"/></svg>` : ""}${label}</button>`)
      .join("")}</span>` : ""}`;
  for (const b of el.querySelectorAll("button"))
    b.onclick = () => ({ receive: () => $("w-receive").click(), retry: sync, settings: () => $("w-settings").click() })[b.dataset.act]();
}

function linked(o) {
  syncState = "syncing";
  renderOverview(o);
  show("wallet");
  sync();
}

// ---- first use: welcome, how many signers, link, backup (multisig), server, password, check on Goldfish
const STEPS = ["welcome", "type", "link", "backup", "server", "password", "done"];
// one signer has nothing to back up here: its seed is the backup
const flow = () => STEPS.filter((s) => s !== "backup" || wzMultisig);
let wzStep = "welcome";
let wzMultisig = false;
let wzServer = "public";
function step(name) {
  wzStep = name;
  clearInterval(showTimer);
  if (scanning) stopScan();
  error("");
  for (const s of STEPS) $("wz-" + s).hidden = s !== name;
  const steps = flow();
  const i = steps.indexOf(name);
  // a dot for each step after the welcome
  $("wz-dots").hidden = i === 0;
  $("wz-dots").replaceChildren(...steps.slice(1).map((_, j) => Object.assign(document.createElement("i"), { className: j < i ? "on" : "" })));
  $("wz-back").hidden = i === 0;
  $("wz-next").hidden = !["welcome", "backup", "server", "password", "done"].includes(name);
  button($("wz-next"), { welcome: "Get started", backup: "I saved it", server: "Continue", password: "Set password", done: "Open wallet" }[name] || "", false);
  $("wz-skip").hidden = name !== "password";
  if (name === "password") {
    $("wz-pw").value = $("wz-pw-again").value = "";
    $("wz-pw").focus();
  }
  $("link-scan").hidden = $("link-alt").hidden = true;
  if (name === "link") linkPanel(wzMultisig ? "ms" : null);
  // a server saved before (it stays when a wallet is forgotten) is kept, not swapped for the public one
  if (name === "server") {
    $("wz-url").value = server.url;
    $("wz-proxy").value = server.proxy;
    pickServer(server.url || server.proxy ? "own" : "public");
  }
  if (name === "backup") showBackup();
  if (name === "done") showDone();
  window.scrollTo(0, 0);
}
$("wz-back").onclick = () => {
  if (wzStep === "link" && !$("link-paste").hidden) return linkPanel(null);
  // the password step is passed over once there is one
  const steps = flow();
  step(wzStep === "done" && hasPassword ? "server" : steps[steps.indexOf(wzStep) - 1]);
};
$("wz-next").onclick = async () => {
  if (wzStep === "welcome") step("type");
  else if (wzStep === "backup") step("server");
  else if (wzStep === "server") saveServer();
  else if (wzStep === "password") savePassword();
  else if (wzStep === "done") linked(current);
};
$("wz-skip").onclick = () => step("done");
$("wz-password").onsubmit = (ev) => {
  ev.preventDefault();
  savePassword();
};
async function savePassword() {
  if (!$("wz-pw").value) return error("Enter a password, or skip this step.");
  if ($("wz-pw").value !== $("wz-pw-again").value) return error("The two passwords don't match.");
  button($("wz-next"), "Locking…", true);
  try {
    await call("set_password", { password: $("wz-pw").value });
  } catch (_) {
    return button($("wz-next"), "Set password", false); // shown in the error line
  }
  hasPassword = true;
  step("done");
}
for (const b of document.querySelectorAll("[data-type]"))
  b.onclick = () => {
    wzMultisig = b.dataset.type === "ms";
    step("link");
  };
function pickServer(kind) {
  wzServer = kind;
  for (const o of document.querySelectorAll("[data-server]")) o.setAttribute("aria-pressed", String(o.dataset.server === kind));
  $("wz-own").hidden = kind !== "own";
}
for (const b of document.querySelectorAll("[data-server]"))
  b.onclick = () => {
    pickServer(b.dataset.server);
    if (wzServer === "own") $("wz-url").focus();
  };
async function saveServer() {
  const url = wzServer === "own" ? $("wz-url").value.trim() : "";
  const proxy = wzServer === "own" ? $("wz-proxy").value.trim() : "";
  if (wzServer === "own" && !url) return error("Enter your server's address, or pick the public server.");
  // the public server is the default: nothing to check or save unless one was set before
  if (url !== server.url || proxy !== server.proxy) {
    button($("wz-next"), "Checking the server…", true);
    try {
      await call("set_server", { url, proxy });
      server = { url, proxy };
    } catch (_) {
      return button($("wz-next"), "Continue", false); // shown in the error line
    }
  }
  step(hasPassword ? "done" : "password");
}
// Multisig: the descriptor as text, to keep with the seed backups
async function showBackup() {
  $("backup-text").textContent = "";
  $("backup-copy").dataset.text = "";
  $("backup-copy").disabled = true; // until the descriptor is here: never a "Copied" with nothing copied
  const w = await call("wallet_descriptor");
  if (wzStep !== "backup") return;
  $("backup-text").textContent = w.descriptor;
  $("backup-copy").dataset.text = w.descriptor;
  $("backup-copy").disabled = false;
}
// The last step: for one signer, the first address to check on Goldfish;
// for multisig, the wallet for each Goldfish to scan.
async function showDone() {
  const ms = !!current?.multisig;
  $("done-title").textContent = ms ? "Connect your signers" : "Make sure it's yours";
  $("done-hint").innerHTML = ms
    ? "Scan this with each Goldfish, then tap <b>Use wallet</b> if the details match."
    : "On Goldfish, tap <b>Scan QR</b> and scan this. It should say <b>Address verified</b>.";
  $("done-addr").hidden = ms;
  $("done-kv").hidden = !ms;
  $("done-qr").innerHTML = "";
  $("done-part").hidden = true;
  if (ms) {
    const w = await call("wallet_descriptor");
    if (wzStep === "done" && currentView === "link") showDescriptor(w, "done-qr", "done-part", "done-kv");
    return;
  }
  const r = await call("receive_address");
  if (wzStep === "done") renderAddress(r, "done-qr", "done-addr");
}

$("link-scan").onclick = async () => {
  await startScan("key");
  button($("link-scan"), "Looking for the QR…", true);
};
// One of: the intro (steps), the paste panel, the multisig panel.
function linkPanel(name) {
  $("link-intro").hidden = !!name;
  $("link-paste").hidden = name !== "paste";
  $("link-ms").hidden = name !== "ms";
  $("link-scan").hidden = !!name;
  $("link-alt").hidden = !!name;
}
$("link-paste-toggle").onclick = () => linkPanel("paste");
// linked: render it, then on to the backup (multisig) or server step
function linkedFirst(o) {
  renderOverview(o);
  wzMultisig = !!o.multisig; // a descriptor pasted as one signer's key is multisig too
  step(wzMultisig ? "backup" : "server");
}
$("link-import").onclick = async () => linkedFirst(await call("import_key", { text: $("link-text").value }));
listen("key", (e) => {
  scanning = false;
  camera(false);
  invoke("scan_stop");
  button($("link-scan"), "Scan QR", false);
  linkedFirst(e.payload);
});

// ---- multisig: link from a descriptor or the cosigners' keys
$("ms-keys").oninput = () => ($("ms-threshold-field").hidden = $("ms-keys").value.includes("multi("));
$("ms-scan").onclick = async () => {
  await startScan("text");
  button($("ms-scan"), "Looking for the QR…", true);
};
listen("text", (e) => {
  scanning = false;
  camera(false);
  invoke("scan_stop");
  button($("ms-scan"), "Scan a key or descriptor", false);
  const text = e.payload.trim();
  $("ms-keys").value = text.includes("multi(") ? text : ($("ms-keys").value.trim() + "\n" + text).trim();
  $("ms-keys").oninput();
});
$("ms-link").onclick = async () =>
  linkedFirst(await call("link_multisig", { threshold: Number($("ms-threshold").value), keys: $("ms-keys").value }));

// ---- show the multisig wallet to Goldfish
$("w-show").onclick = async () => {
  const w = await call("wallet_descriptor");
  show("show");
  showDescriptor(w, "sh-qr", "sh-part", "sh-kv");
};
function showDescriptor(w, qr, part, kv) {
  const rows = [
    ["Wallet", w.kind],
    ...w.fingerprints.map((fp, i) => [i ? "" : "Cosigners", fingerprint(fp)]),
    ["First address", groups(w.first_address)],
  ];
  $(kv).replaceChildren(
    ...rows.flatMap(([k, v]) => {
      const dt = document.createElement("dt");
      const dd = document.createElement("dd");
      dt.textContent = k;
      dd.textContent = v;
      dd.className = "mono";
      if (!k) dt.className = "cont"; // another cosigner: same row group
      if (k === "Cosigners" || (!k && v)) {
        // each cosigner's key fingerprint, with the fingerprint icon Goldfish shows beside its own
        dd.classList.add("cosigner");
        dd.insertAdjacentHTML("afterbegin", '<svg><use href="#i-fingerprint"/></svg>');
      }
      return [dt, dd];
    }),
  );
  $(part).hidden = !!w.qr;
  if (w.qr) {
    $(qr).innerHTML = w.qr; // SVG made by the qrcode crate from our own descriptor
    return;
  }
  const tick = async () => {
    const f = await invoke("next_wallet_frame");
    $(qr).innerHTML = f.svg;
    $(part).textContent = `Animated · ${partOf(f)}`;
  };
  tick();
  clearInterval(showTimer);
  showTimer = setInterval(tick, 250);
}

// signatures so far: how many are still needed, one row per cosigner,
// and the scan button counts along ("Scan signature 2 of 2")
function renderSigs(sigs) {
  const el = $("sign-sigs");
  el.hidden = !current?.multisig;
  if (el.hidden) return;
  const left = sigs.needed - sigs.have;
  const head = document.createElement("span");
  head.className = "sig-head";
  head.innerHTML = `<b></b><span></span>`;
  head.querySelector("b").textContent = `${left} more signature${left === 1 ? "" : "s"} needed`;
  head.querySelector("span").textContent = `${sigs.have} of ${sigs.needed}`;
  const bar = document.createElement("span");
  bar.className = "sig-bar";
  for (let i = 0; i < sigs.needed; i++) bar.append(Object.assign(document.createElement("i"), { className: i < sigs.have ? "on" : "" }));
  el.replaceChildren(
    head,
    bar,
    ...current.fingerprints.map((fp) => {
      const on = sigs.signers.includes(fp);
      const row = document.createElement("span");
      row.className = "signer" + (on ? " on" : "");
      row.innerHTML = `<span class="fp"><svg><use href="#i-fingerprint"/></svg><span></span></span><span class="state"></span>`;
      row.querySelector(".fp span").textContent = fingerprint(fp);
      row.querySelector(".state").innerHTML = on ? `<svg><use href="#i-check"/></svg>Signed` : "Waiting";
      return row;
    }),
  );
  const label = `Scan signature ${sigs.have + 1} of ${sigs.needed}`;
  $("sign-scan").dataset.label = label;
  button($("sign-scan"), label, false);
}

// ---- wallet
async function sync() {
  button($("w-sync"), "Refreshing…", true);
  syncState = "syncing";
  renderEmpty();
  try {
    const o = await call("sync");
    syncState = "ok";
    renderOverview(o);
  } catch (_) {
    syncState = "failed";
    // with nothing to list, the Activity card says it; otherwise the error line does
    if (!current?.txs.length) error("");
    renderEmpty();
  } finally {
    button($("w-sync"), "Refresh", false);
  }
}
$("w-sync").onclick = sync;
$("w-receive").onclick = async () => {
  const r = await call("receive_address");
  show("receive");
  $("r-label").textContent = `Address #${r.index}`;
  renderAddress(r, "r-qr", "r-addr");
  $("r-copy").dataset.text = r.address;
};
function renderAddress(r, qr, addr) {
  $(qr).innerHTML = r.qr; // SVG made by the qrcode crate from our own address
  // groups of four, every other one lighter, to compare against Goldfish's screen
  $(addr).replaceChildren(
    ...r.address.match(/.{1,4}/g).map((g, i) => {
      const span = document.createElement("span");
      span.textContent = g;
      if (i % 2) span.className = "alt";
      return span;
    }),
  );
}
$("w-send").onclick = () => {
  show("send");
  loadRates("s-rates", "s-fee");
};
$("w-settings").onclick = () => {
  show("settings");
  $("set-url").value = server.url;
  $("set-proxy").value = server.proxy;
  $("set-url").placeholder = "Public server (default)";
  $("set-ok").hidden = true;
  passwordSection();
};
window.__TAURI__.app.getVersion().then((v) => { $("set-version").textContent = `Dove ${v}`; }, () => {});

// ---- fee suggestions: chips that fill the fee field
async function loadRates(chips, input) {
  $(chips).hidden = true;
  let rates;
  try {
    rates = await invoke("fee_rates");
  } catch (_) {
    return; // no estimates: the field keeps what it has
  }
  for (const b of $(chips).querySelectorAll("button")) {
    const r = rates[b.dataset.rate];
    b.querySelector("span").textContent = `${r} sat/vB`;
    b.onclick = () => pick(chips, input, b, r);
    b.setAttribute("aria-pressed", "false");
  }
  $(chips).hidden = false;
  const normal = $(chips).querySelector('[data-rate="normal"]');
  if (input === "s-fee") pick(chips, input, normal, rates.normal);
  return rates;
}
function pick(chips, input, button, rate) {
  $(input).value = rate;
  for (const b of $(chips).querySelectorAll("button")) b.setAttribute("aria-pressed", String(b === button));
}
for (const id of ["s-fee", "b-fee"]) {
  $(id).oninput = () => {
    for (const b of $(id === "s-fee" ? "s-rates" : "b-rates").querySelectorAll("button")) b.setAttribute("aria-pressed", "false");
  };
}

// ---- speed up (RBF)
let bumping = null;
async function openBump(t) {
  bumping = t;
  show("bump");
  $("b-txid").textContent = shortId(t.txid);
  $("b-now").textContent = t.fee_rate ? `${t.fee_rate.toFixed(1)} sat/vB` : "unknown";
  $("b-fee").value = t.fee_rate ? Math.ceil(t.fee_rate + 1) : "";
  const rates = await loadRates("b-rates", "b-fee");
  if (rates && t.fee_rate && rates.fast > t.fee_rate) pick("b-rates", "b-fee", $("b-rates").querySelector('[data-rate="fast"]'), rates.fast);
}
$("b-form").onsubmit = async (ev) => {
  ev.preventDefault();
  showReview(await call("create_bump", { txid: bumping.txid, satPerVb: Number($("b-fee").value) }));
};

// ---- settings
$("set-form").onsubmit = async (ev) => {
  ev.preventDefault();
  button($("set-save"), "Checking the server…", true);
  try {
    const r = await call("set_server", { url: $("set-url").value, proxy: $("set-proxy").value });
    server = { url: $("set-url").value.trim(), proxy: $("set-proxy").value.trim() };
    $("set-ok").textContent = `Connected and saved. Fees now ${r.fast} / ${r.normal} / ${r.slow} sat/vB.`;
    $("set-ok").hidden = false;
    sync();
  } catch (_) {
    // shown in the error line
  } finally {
    button($("set-save"), "Save", false);
  }
};
$("set-forget").onclick = async () => {
  if ($("set-forget").dataset.sure !== "1") {
    $("set-forget").dataset.sure = "1";
    $("set-forget").textContent = "Tap again to forget";
    return;
  }
  await call("forget_wallet");
  $("set-forget").dataset.sure = "";
  $("set-forget").textContent = "Forget this wallet";
  current = null;
  for (const id of ["link-text", "ms-keys"]) $(id).value = "";
  wzMultisig = false;
  show("link");
  step("welcome");
};
for (const b of document.querySelectorAll(".back")) b.onclick = () => show("wallet");

// ---- update Goldfish: check, update mode, install. Nothing goes to GitHub
// until Check for update.
const UP_STEPS = ["check", "found", "connect", "done"];
let upStep = "check";
function upGo(name) {
  upStep = name;
  clearInterval(fwTimer);
  for (const s of UP_STEPS) $("up-" + s).hidden = s !== name;
  $("up-btn").hidden = false;
  if (name === "check") button($("up-btn"), "Check for update", false);
  if (name === "found") button($("up-btn"), "Continue", false);
  if (name === "done") button($("up-btn"), "Done", false);
  if (name === "connect") {
    const poll = async () => {
      const drive = await invoke("firmware_drive");
      if (fwBusy || upStep !== "connect") return;
      $("up-status").textContent = drive ? "Found Goldfish in update mode." : "Looking for Goldfish…";
      $("up-status").classList.toggle("found", !!drive);
      button($("up-btn"), `Install ${fwRelease.tag}`, !drive);
    };
    poll();
    fwTimer = setInterval(poll, 1000);
  }
}
$("set-update").onclick = () => {
  show("update");
  upGo("check");
};
$("up-tag").onclick = () => window.__TAURI__.opener.openUrl(fwRelease.url).catch(error);
$("up-btn").onclick = async () => {
  if (upStep === "check") {
    button($("up-btn"), "Downloading…", true);
    try {
      fwRelease = await invoke("firmware_check");
    } catch (e) {
      // the network errors friendly() knows would point at the wallet's server
      const offline = friendly(e)[0].startsWith("Can't reach");
      error(offline ? "Can't reach GitHub. Check your internet connection, or the proxy in Settings." : e);
      button($("up-btn"), "Check for update", false);
      return;
    }
    $("up-title").textContent = `Goldfish ${fwRelease.tag}`;
    $("up-tag").textContent = fwRelease.tag;
    $("up-sha").textContent = fwRelease.sha256;
    upGo("found");
  } else if (upStep === "found") {
    upGo("connect");
  } else if (upStep === "connect") {
    clearInterval(fwTimer);
    fwBusy = true;
    button($("up-btn"), "Installing…", true);
    $("up-status").textContent = "Keep Goldfish plugged in.";
    try {
      await call("firmware_install");
    } catch (_) {
      upGo("connect");
      return;
    } finally {
      fwBusy = false;
    }
    $("up-done-text").textContent = `Goldfish restarts by itself. Its home screen should now show ${fwRelease.tag} in the corner.`;
    upGo("done");
  } else {
    show("wallet");
  }
};

// ---- send
$("s-form").onsubmit = async (ev) => {
  ev.preventDefault();
  showReview(
    await call("create_send", {
      to: $("s-to").value.trim(),
      amountSat: Number($("s-amount").value),
      satPerVb: Number($("s-fee").value),
    }),
  );
};

function showReview(r) {
  const paying = r.recipients.reduce((s, x) => s + x.sat, 0);
  show("sign");
  $("rv-amount").textContent = btc(paying);
  $("rv-btc").textContent = inBtc(paying);
  const rows = [
    ...r.recipients.map((x) => [r.recipients.length > 1 ? btc(x.sat) : "To", groups(x.address), x.yours]),
    ["Fee", btc(r.fee_sat)],
    ["Change", r.change_sat ? btc(r.change_sat) : "None"],
  ];
  $("review").replaceChildren(
    ...rows.flatMap(([k, v, yours]) => {
      const dt = document.createElement("dt");
      const dd = document.createElement("dd");
      dt.textContent = k;
      dd.textContent = v;
      if (v.length > 20) dd.className = "mono";
      if (yours) {
        // a send to one of this wallet's own addresses
        const tag = document.createElement("span");
        tag.className = "yours";
        tag.textContent = "This wallet";
        dd.append(tag);
      }
      return [dt, dd];
    }),
  );
  $("sign-progress").hidden = true;
  $("sign-paste").hidden = true;
  menu(false);
  signTxid = r.txid;
  button($("sign-scan"), "Scan signed QR", false);
  $("sign-who").textContent = "Scan with Goldfish";
  $("sign-fish").hidden = false;
  $("sign-cap").classList.add("gf-cap");
  waiting = null; // a new transaction replaces one signed but not broadcast
  $("sign-scan").dataset.label = "Scan signed QR";
  renderSigs({ have: 0, needed: current?.threshold ?? 1, signers: [] });
  startFrames();
}

function startFrames() {
  stopFrames();
  const tick = async () => {
    try {
      const f = await invoke("next_frame");
      $("sign-qr").innerHTML = f.svg;
      $("sign-part").textContent = partOf(f);
    } catch (_) {
      stopFrames();
    }
  };
  tick();
  frameTimer = setInterval(tick, 250);
}
function stopFrames() {
  clearInterval(frameTimer);
  frameTimer = null;
}

// A problem with what the camera sees goes right under the camera, and goes
// away once a good QR is being read; anything else uses the error line.
function camMsg(text) {
  for (const m of document.querySelectorAll(".cam-msg")) m.hidden = true;
  const m = $("v-" + currentView).querySelector(".cam-msg");
  if (!m || !text) return;
  m.textContent = friendly(text)[0];
  m.hidden = false;
}
function onScan(ev) {
  if (ev.error) scanning ? camMsg(ev.error) : error(ev.error);
  else if (ev.percent) camMsg("");
  if (ev.percent) {
    $("sign-progress").hidden = false;
    $("sign-progress").value = ev.percent;
  }
  if (ev.sigs && !ev.signed_txid) {
    // a cosigner signed, more are needed: the QR now carries their signature
    scanning = false;
    camera(false);
    invoke("scan_stop");
    renderSigs(ev.sigs);
    $("sign-progress").hidden = true;
    $("sign-who").textContent = "Show to the next signer";
    $("sign-fish").hidden = true; // the next signer may not be a Goldfish
    $("sign-cap").classList.remove("gf-cap");
    return;
  }
  if (ev.signed_txid) {
    waiting = ev.signed_txid;
    openReady();
  }
}
$("sign-scan").onclick = async () => {
  await startScan("psbt");
  button($("sign-scan"), "Reading the QR…", true);
};
$("sign-use").onclick = async () => {
  for (const line of $("sign-text").value.split(/\s+/).filter(Boolean)) onScan(await call("scan_text", { text: line }));
};
listen("scan", (e) => onScan(e.payload));

// Another signer (Sparrow with a Trezor or Ledger, a Coldcard's SD card), from
// the ⋮ menu: the PSBT with the signatures so far as text or a .psbt file, and
// the signed one back as a file or pasted
let signTxid = "";
function menu(open) {
  $("sign-menu").hidden = !open;
  $("sign-more").setAttribute("aria-expanded", open);
}
$("sign-more").onclick = (e) => {
  e.stopPropagation();
  menu($("sign-menu").hidden);
};
document.addEventListener("click", (e) => {
  if (!$("sign-menu").hidden && !$("sign-menu").contains(e.target)) menu(false);
});
const psbtFilter = [{ name: "PSBT", extensions: ["psbt", "txn", "txt"] }];
$("sign-copy").onclick = async () => {
  const label = $("sign-copy").querySelector("span");
  try {
    label.textContent = (await copyText(await call("psbt_text"))) ? "Copied" : "Couldn't copy";
    await new Promise((r) => setTimeout(r, 700));
  } catch (_) {
    // shown in the error line
  } finally {
    menu(false);
    label.textContent = "Copy transaction";
  }
};
$("sign-save").onclick = async () => {
  menu(false);
  try {
    const path = await window.__TAURI__.dialog.save({ defaultPath: `dove-${signTxid.slice(0, 8)}.psbt`, filters: psbtFilter });
    if (path) await call("save_psbt", { path });
  } catch (e) {
    error(e);
  }
};
$("sign-open").onclick = async () => {
  menu(false);
  try {
    const path = await window.__TAURI__.dialog.open({ filters: psbtFilter });
    if (path) onScan(await call("open_psbt", { path }));
  } catch (e) {
    error(e);
  }
};
$("sign-paste-item").onclick = () => {
  menu(false);
  $("sign-paste").hidden = !$("sign-paste").hidden;
  if (!$("sign-paste").hidden) $("sign-text").focus();
};

// ---- broadcast
function openReady() {
  show("ready");
  $("ready-txid").textContent = shortId(waiting);
  $("ready-txid").title = waiting;
  $("ready-server").hidden = true;
}
$("w-waiting").onclick = openReady;
// viaServer: after Tor or its peers failed, through the wallet's server after all
async function broadcast(viaServer) {
  const buttons = [$("ready-send"), $("ready-server")];
  buttons.forEach((b) => (b.disabled = true));
  const status = $("ready-sending");
  status.hidden = false;
  let tor = false;
  try {
    if (!viaServer) {
      status.textContent = "Connecting to Tor…";
      tor = await call("tor_ready");
    }
    status.textContent = tor ? "Sending over Tor. This can take a minute or two." : "Sending…";
    const txid = await call("broadcast_signed", { viaServer });
    waiting = null;
    show("sent");
    $("sent-txid").textContent = groups(txid);
    $("sent-copy").dataset.text = txid;
    const base = current?.network === "bitcoin" ? "https://mempool.space/tx/" : "https://mempool.space/signet/tx/";
    $("sent-explorer").onclick = () => window.__TAURI__.opener.openUrl(base + txid).catch(error);
    sync();
  } catch (_) {
    // shown in the error line; Tor may be blocked here, so offer the server
    if (!viaServer) $("ready-server").hidden = false;
  } finally {
    buttons.forEach((b) => (b.disabled = false));
    status.hidden = true;
  }
}
$("ready-send").onclick = () => broadcast(false);
$("ready-server").onclick = () => broadcast(true);

async function copyText(text) {
  try {
    await navigator.clipboard.writeText(text);
    return true;
  } catch (_) {
    // WebKitGTK may refuse the async clipboard; the old way still works there
    const t = document.createElement("textarea");
    t.value = text;
    t.style.position = "fixed";
    t.style.opacity = "0";
    document.body.append(t);
    t.select();
    const ok = document.execCommand("copy");
    t.remove();
    return ok;
  }
}

// Copy buttons: the text is in data-text; the label says it worked.
for (const b of document.querySelectorAll("button.copy")) {
  b.onclick = async () => {
    const label = b.querySelector("span");
    const was = label.dataset.was || (label.dataset.was = label.textContent);
    label.textContent = (await copyText(b.dataset.text)) ? "Copied" : "Couldn't copy: select the text instead";
    setTimeout(() => (label.textContent = was), 1600);
  };
}

// ---- password: locks the saved wallet and server on this computer
function passwordSection() {
  $("pw-hint").textContent = hasPassword
    ? "Dove asks for it when it opens. Forgot it? Link the wallet again from your signer."
    : "Lock this wallet and the server on this computer. Dove asks for it when it opens.";
  $("pw-new-label").textContent = hasPassword ? "New password" : "Password";
  button($("pw-save"), hasPassword ? "Change password" : "Set password", false);
  $("pw-remove").hidden = !hasPassword;
  $("pw-remove").dataset.sure = "";
  $("pw-remove").textContent = "Remove password";
  $("pw-new").value = $("pw-again").value = "";
  $("pw-ok").hidden = true;
}
$("pw-form").onsubmit = async (ev) => {
  ev.preventDefault();
  $("pw-ok").hidden = true;
  if (!$("pw-new").value) return error("Enter a password first.");
  if ($("pw-new").value !== $("pw-again").value) return error("The two passwords don't match.");
  button($("pw-save"), "Locking…", true);
  try {
    await call("set_password", { password: $("pw-new").value });
    hasPassword = true;
    passwordSection();
    $("pw-ok").textContent = "Password set. Dove asks for it next time.";
    $("pw-ok").hidden = false;
  } catch (_) {
    button($("pw-save"), hasPassword ? "Change password" : "Set password", false);
  }
};
$("pw-remove").onclick = async () => {
  if ($("pw-remove").dataset.sure !== "1") {
    $("pw-remove").dataset.sure = "1";
    $("pw-remove").textContent = "Tap again to remove";
    return;
  }
  try {
    await call("set_password", { password: "" });
  } catch (_) {
    passwordSection(); // back to asking twice; the error says what failed
    return;
  }
  hasPassword = false;
  passwordSection();
  $("pw-ok").textContent = "Password removed.";
  $("pw-ok").hidden = false;
};

function started(s) {
  server = s.server;
  hasPassword = s.has_password;
  if (s.locked) {
    show("unlock");
    $("unlock-pw").focus();
  } else if (s.wallet) linked(s.wallet);
  else {
    show("link");
    step("welcome");
  }
}
$("unlock-form").onsubmit = async (ev) => {
  ev.preventDefault();
  button($("unlock-go"), "Unlocking…", true);
  try {
    started(await call("unlock", { password: $("unlock-pw").value }));
    $("unlock-pw").value = "";
  } catch (_) {
    $("unlock-pw").select();
  } finally {
    button($("unlock-go"), "Unlock", false);
  }
};
$("unlock-reset").onclick = async () => {
  if ($("unlock-reset").dataset.sure !== "1") {
    $("unlock-reset").dataset.sure = "1";
    $("unlock-reset").textContent = "Start over";
    $("unlock-reset").classList.add("danger");
    $("unlock-reset-hint").hidden = false;
    return;
  }
  try {
    await call("start_over");
  } catch (_) {
    $("unlock-reset").dataset.sure = "";
    $("unlock-reset").textContent = "Forgot password?";
    $("unlock-reset").classList.remove("danger");
    $("unlock-reset-hint").hidden = true;
    return;
  }
  hasPassword = false;
  show("link");
  step("welcome");
};

// ---- start: reopen the saved wallet, if any
invoke("startup")
  .then(started)
  .catch((e) => {
    show("link");
    step("welcome");
    error(e);
  });
