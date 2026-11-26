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
  if (e.key === "Escape" && document.body.classList.contains("dialog-open")) show("wallet");
});

function show(view) {
  currentView = view;
  layout();
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
  for (const c of document.querySelectorAll(".cam, .cam-cancel")) c.hidden = true;
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
  $("fp-pill").title = o.multisig ? o.kind : "Goldfish's fingerprint";
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
  $("link-back").hidden = !name;
}
$("link-paste-toggle").onclick = () => linkPanel("paste");
$("link-back").onclick = () => linkPanel(null);
$("link-import").onclick = async () => linked(await call("import_key", { text: $("link-text").value }));
listen("key", (e) => {
  scanning = false;
  camera(false);
  invoke("scan_stop");
  button($("link-scan"), "Scan QR", false);
  linked(e.payload);
});

// ---- multisig: link from a descriptor or the cosigners' keys
$("link-ms-toggle").onclick = () => linkPanel("ms");
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
  const rows = [
    ["Wallet", w.kind],
    ...w.fingerprints.map((fp, i) => [i ? "" : "Cosigners", fingerprint(fp)]),
    ["First address", groups(w.first_address)],
  ];
  $("sh-kv").replaceChildren(
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
  $("r-qr").innerHTML = r.qr; // SVG made by the qrcode crate from our own address
  $("r-label").textContent = `Address #${r.index}`;
  // groups of four, every other one lighter, to compare against Goldfish's screen
  $("r-addr").replaceChildren(
    ...r.address.match(/.{1,4}/g).map((g, i) => {
      const span = document.createElement("span");
      span.textContent = g;
      if (i % 2) span.className = "alt";
      return span;
    }),
  );
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
    $("set-forget").textContent = "Tap again to forget";
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
  button($("sign-scan"), "Scan signed QR", false);
  renderSigs({ have: 0, needed: current?.threshold ?? 1, signers: [] });
  $("sign-who").textContent = "Scan with Goldfish";
  $("sign-fish").hidden = false;
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
    $("sign-fish").hidden = true; // the next signer may not be a Goldfish
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
