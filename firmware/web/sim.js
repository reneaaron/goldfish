// The firmware in the browser, on a canvas: the landing page's hero ("Plug it in").
//
//   goldfishSim(canvas, { base, onCameraStop, onReboot }) -> Promise<{ show(url), unplug() }>
//
// base is where signer.js, signer.wasm and samples/ live ('sim/' from the landing
// page); signer.js is loaded on the first call, not before. show(url) puts a
// picture in front of the camera; onCameraStop says the device took it away again.
// With no picture picked, the camera sees img/camera_idle.jpg: a desk, out of focus,
// so the view is live rather than a black box.
// unplug() stops the firmware and zeroes all of its memory, as pulling the cable
// does on the board; call goldfishSim again for a fresh start.
window.goldfishSim = (canvas, { base = '', onCameraStop = () => {}, onReboot = () => {} } = {}) => {
  const ctx = canvas.getContext('2d');
  const img = ctx.createImageData(240, 320);
  let M;

  // ---- screen: RGB565 framebuffer -> canvas
  function onFlush() {
    const fb = new Uint16Array(M.HEAPU16.buffer, M._web_framebuffer(), 240 * 320);
    const px = img.data;
    for (let i = 0, j = 0; i < fb.length; i++, j += 4) {
      const c = fb[i];
      px[j] = ((c >> 11) << 3) | (c >> 13);
      px[j + 1] = (((c >> 5) & 0x3f) << 2) | ((c >> 9) & 3);
      px[j + 2] = ((c & 0x1f) << 3) | ((c >> 2) & 7);
      px[j + 3] = 255;
    }
    ctx.putImageData(img, 0, 0);
  }

  // ---- touch: pointer on the canvas, in screen pixels
  function touch(e, down) {
    if (!M || !M._web_touch) return;
    const r = canvas.getBoundingClientRect();
    const x = Math.max(0, Math.min(239, Math.round((e.clientX - r.left) * 240 / r.width)));
    const y = Math.max(0, Math.min(319, Math.round((e.clientY - r.top) * 320 / r.height)));
    M._web_touch(x, y, down ? 1 : 0);
  }
  let pressed = false;
  const off = new AbortController();   // unplug() removes these, so a replug doesn't add a second set
  const on = { signal: off.signal };
  canvas.addEventListener('pointerdown', e => { pressed = true; canvas.setPointerCapture(e.pointerId); touch(e, true); }, on);
  canvas.addEventListener('pointermove', e => { if (pressed) touch(e, true); }, on);
  canvas.addEventListener('pointerup', e => { pressed = false; touch(e, false); }, on);
  canvas.addEventListener('pointercancel', e => { pressed = false; touch(e, false); }, on);

  // ---- camera: one grayscale picture at a time, at most 2000 px on its longer side
  const grab = document.createElement('canvas');
  const gctx = grab.getContext('2d', { willReadFrequently: true });
  function feed(source, w, h) {
    const s = Math.min(1, 2000 / Math.max(w, h));
    w = Math.round(w * s); h = Math.round(h * s);
    grab.width = w; grab.height = h;
    gctx.fillStyle = '#fff';
    gctx.fillRect(0, 0, w, h);
    gctx.drawImage(source, 0, 0, w, h);
    const rgba = gctx.getImageData(0, 0, w, h).data;
    const p = M._web_camera_buffer(w, h);
    if (!p) return;
    const gray = new Uint8Array(M.HEAPU8.buffer, p, w * h);
    for (let i = 0, j = 0; i < gray.length; i++, j += 4)
      gray[i] = (rgba[j] * 77 + rgba[j + 1] * 150 + rgba[j + 2] * 29) >> 8;
    M._web_camera_set(w, h);
  }
  let picked = false;
  async function load(url, idle) {
    const im = new Image();
    im.src = url;
    try { await im.decode(); } catch { return; }
    if (idle && picked) return;   // a pick came first
    if (M) feed(im, im.naturalWidth, im.naturalHeight);
  }
  const show = (url) => { picked = true; return load(url, false); };
  const idle = () => load(base + 'img/camera_idle.jpg', true);
  function unplug() {
    off.abort();
    if (!M) return;
    M._web_unplug();
    M.HEAPU8.fill(0);
    M = null;
    ctx.clearRect(0, 0, 240, 320);
  }

  // ---- start: signer.js on demand, then the firmware
  const script = window.Signer ? Promise.resolve() : new Promise((ok, fail) => {
    const s = document.createElement('script');
    s.src = base + 'signer.js';
    s.onload = ok; s.onerror = fail;
    document.head.append(s);
  });
  return script.then(() => {
    // The options object is the module itself: set M now, main() draws before the promise resolves.
    M = {
      onFlush,
      locateFile: (p) => base + p,
      // The device is done looking: take the picture away, as you would a card.
      onCameraStop: () => { M._web_camera_clear(); picked = false; idle(); onCameraStop(); },
      onReboot,
    };
    return window.Signer(M);
  }).then(() => { idle(); return { show, unplug }; });
};
