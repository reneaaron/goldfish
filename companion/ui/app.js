// Goldfish Companion page. All wallet logic is in Rust (src-tauri); this only
// switches views and calls commands.
const { invoke } = window.__TAURI__.core;
const { listen } = window.__TAURI__.event;

const $ = (id) => document.getElementById(id);
const VIEWS = ["link", "wallet", "receive", "send", "bump", "settings", "show", "sign", "ready", "sent"];
let waiting = null; // txid of a signed transaction not broadcast yet
let current = null; // the linked wallet's overview
let showTimer = null;
let server = { url: "", proxy: "" };
let frameTimer = null;
let scanning = false;

function show(view) {
  for (const v of VIEWS) $("v-" + v).hidden = v !== view;
  if (view !== "sign") stopFrames();
  if (view !== "show") clearInterval(showTimer);
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
  const open = [...document.querySelectorAll("main > section")].find((s) => !s.hidden);
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
  for (const c of document.querySelectorAll(".cam, .cam-cancel")) c.hidden = !on;
  $("sign-show").hidden = on;
  if (!on) {
    button($("link-scan"), "Scan QR", false);
    button($("ms-scan"), "Scan a key or descriptor", false);
    button($("sign-scan"), $("sign-scan").dataset.label || "Scan signed QR", false);
  }
}
for (const b of document.querySelectorAll(".cam-cancel")) b.onclick = () => stopScan();
listen("preview", ({ payload: p }) => {
  const canvas = [...document.querySelectorAll(".cam")].find((c) => !c.hidden)?.querySelector("canvas");
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
    ctx.strokeStyle = code.read ? "#15803D" : "#C2501A";
    ctx.beginPath();
    code.corners.forEach(([x, y], i) => (i ? ctx.lineTo(x, y) : ctx.moveTo(x, y)));
    ctx.closePath();
    ctx.stroke();
  }
});

function renderOverview(o) {
  current = o;
  $("fp").textContent = o.multisig ? o.kind.replace(" multisig", "") : fingerprint(o.fingerprints[0]);
  $("fp").previousElementSibling.querySelector("use").setAttribute("href", o.multisig ? "#i-users" : "#i-fingerprint");
  $("w-show").hidden = !o.multisig;
  $("net").hidden = o.network === "bitcoin";
  $("net").textContent = o.network.toUpperCase();
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
      li.querySelector(".what small").textContent =
        shortId(t.txid) + (!incoming && t.fee_rate ? ` · ${t.fee_rate.toFixed(1)} sat/vB` : "");
      li.querySelector(".amt").textContent = (incoming ? "+" : "−") + btc(t.net_sat);
      return li;
    }),
  );
  $("txs-empty").hidden = o.txs.length > 0;
}

function linked(o) {
  renderOverview(o);
  show("wallet");
  sync();
}

$("link-scan").onclick = async () => {
  await startScan("key");
  button($("link-scan"), "Looking for the QR…", true);
};
$("link-paste-toggle").onclick = () => ($("link-paste").hidden = !$("link-paste").hidden);
$("link-import").onclick = async () => linked(await call("import_key", { text: $("link-text").value }));
listen("key", (e) => {
  scanning = false;
  camera(false);
  invoke("scan_stop");
  button($("link-scan"), "Scan QR", false);
  linked(e.payload);
});

// ---- multisig: link from a descriptor or the cosigners' keys
$("link-ms-toggle").onclick = () => ($("link-ms").hidden = !$("link-ms").hidden);
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
  linked(await call("link_multisig", { threshold: Number($("ms-threshold").value), keys: $("ms-keys").value }));

// ---- show the multisig wallet to Goldfish
$("w-show").onclick = async () => {
  const w = await call("wallet_descriptor");
  show("show");
  $("sh-kind").textContent = w.kind;
  const rows = [
    ["Wallet", w.kind],
    ...w.fingerprints.map((fp, i) => [i ? "" : "Fingerprints", fingerprint(fp)]),
    ["First address", groups(w.first_address)],
  ];
  $("sh-kv").replaceChildren(
    ...rows.flatMap(([k, v]) => {
      const dt = document.createElement("dt");
      const dd = document.createElement("dd");
      dt.textContent = k;
      dd.textContent = v;
      dd.className = "mono";
      if (!k) dt.className = "cont"; // another fingerprint: same row group
      return [dt, dd];
    }),
  );
  $("sh-part").hidden = !!w.qr;
  if (w.qr) {
    $("sh-qr").innerHTML = w.qr; // SVG made by the qrcode crate from our own descriptor
    return;
  }
  const tick = async () => {
    const f = await invoke("next_wallet_frame");
    $("sh-qr").innerHTML = f.svg;
    $("sh-part").textContent = `Animated · ${f.part}/${f.total}`;
  };
  tick();
  showTimer = setInterval(tick, 250);
};

// signatures so far, one chip per cosigner
function renderSigs(sigs) {
  const el = $("sign-sigs");
  el.hidden = !current?.multisig;
  if (el.hidden) return;
  const label = document.createElement("span");
  label.textContent = `${sigs.have} of ${sigs.needed} signatures`;
  el.replaceChildren(
    label,
    ...current.fingerprints.map((fp) => {
      const c = document.createElement("span");
      c.className = "chip" + (sigs.signers.includes(fp) ? " on" : "");
      c.textContent = fingerprint(fp);
      return c;
    }),
  );
}

// ---- wallet
async function sync() {
  button($("w-sync"), "Syncing…", true);
  try {
    renderOverview(await call("sync"));
  } catch (_) {
    // shown in the error line
  } finally {
    button($("w-sync"), "Sync", false);
  }
}
$("w-sync").onclick = sync;
$("w-receive").onclick = async () => {
  const r = await call("receive_address");
  show("receive");
  $("r-qr").innerHTML = r.qr; // SVG made by the qrcode crate from our own address
  $("r-label").textContent = `Receive address #${r.index}`;
  $("r-addr").textContent = groups(r.address);
  $("r-copy").dataset.text = r.address;
};
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
};

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
    $("set-forget").textContent = "Forget it: tap again";
    return;
  }
  await call("forget_wallet");
  $("set-forget").dataset.sure = "";
  $("set-forget").textContent = "Forget this wallet";
  show("link");
};
for (const b of document.querySelectorAll(".back")) b.onclick = () => show("wallet");

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
  const paying = r.recipients.reduce((s, [, v]) => s + v, 0);
  show("sign");
  $("rv-amount").textContent = btc(paying);
  $("rv-btc").textContent = inBtc(paying);
  const rows = [
    ...r.recipients.map(([a, v]) => [r.recipients.length > 1 ? btc(v) : "To", groups(a)]),
    ["Fee", btc(r.fee_sat)],
    ["Change", r.change_sat ? btc(r.change_sat) : "None"],
  ];
  $("review").replaceChildren(
    ...rows.flatMap(([k, v]) => {
      const dt = document.createElement("dt");
      const dd = document.createElement("dd");
      dt.textContent = k;
      dd.textContent = v;
      if (v.length > 20) dd.className = "mono";
      return [dt, dd];
    }),
  );
  $("sign-progress").hidden = true;
  $("sign-paste").hidden = true;
  button($("sign-scan"), "Scan signed QR", false);
  renderSigs({ have: 0, needed: current?.threshold ?? 1, signers: [] });
  $("sign-who").textContent = "Scan with Goldfish";
  waiting = null; // a new transaction replaces one signed but not broadcast
  $("sign-scan").dataset.label = "Scan signed QR";
  startFrames();
}

function startFrames() {
  stopFrames();
  const tick = async () => {
    try {
      const f = await invoke("next_frame");
      $("sign-qr").innerHTML = f.svg;
      $("sign-part").textContent = `${f.part}/${f.total}`;
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

function onScan(ev) {
  if (ev.error) error(ev.error);
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
    $("sign-scan").dataset.label = "Scan next signature";
    button($("sign-scan"), "Scan next signature", false);
    return;
  }
  if (ev.signed_txid) {
    waiting = ev.signed_txid;
    openReady();
  }
}
$("sign-scan").onclick = async () => {
  await startScan("psbt");
  button($("sign-scan"), "Reading Goldfish's QR…", true);
};
$("sign-paste-toggle").onclick = () => ($("sign-paste").hidden = !$("sign-paste").hidden);
$("sign-use").onclick = async () => {
  for (const line of $("sign-text").value.split(/\s+/).filter(Boolean)) onScan(await call("scan_text", { text: line }));
};
listen("scan", (e) => onScan(e.payload));

// ---- broadcast
function openReady() {
  show("ready");
  $("ready-txid").textContent = shortId(waiting);
  $("ready-txid").title = waiting;
}
$("w-waiting").onclick = openReady;
$("ready-send").onclick = async () => {
  $("ready-send").disabled = true;
  try {
    const txid = await call("broadcast_signed");
    waiting = null;
    show("sent");
    $("sent-txid").textContent = groups(txid);
    $("sent-copy").dataset.text = txid;
    const base = current?.network === "bitcoin" ? "https://mempool.space/tx/" : "https://mempool.space/signet/tx/";
    $("sent-explorer").onclick = () => window.__TAURI__.opener.openUrl(base + txid).catch(error);
    sync();
  } catch (_) {
    // shown in the error line
  } finally {
    $("ready-send").disabled = false;
  }
};

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

// ---- start: reopen the saved wallet, if any
invoke("startup")
  .then((s) => {
    server = s.server;
    if (s.wallet) linked(s.wallet);
    else show("link");
  })
  .catch((e) => {
    show("link");
    error(e);
  });
