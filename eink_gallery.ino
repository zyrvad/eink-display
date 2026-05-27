/*
 * E-Ink Gallery Frame
 * Web UI is embedded directly in this file (no LittleFS plugin needed)
 *
 * Required libraries (Library Manager):
 *   - ESP Async WebServer  by mathieucarbou
 *   - AsyncTCP             by mathieucarbou
 *   - ArduinoJson          by Benoit Blanchon
 *
 * Partition Scheme: Tools → Default 4MB with spiffs
 *
 * Waveshare files in sketch folder:
 *   DEV_Config.h/.cpp, EPD.h, EPD_4in2_V2.cpp, GUI_Paint.h/.cpp
 *
 * ── Changes in v1.1 ──────────────────────────────────────────────────────────
 *   1. Frame resumes cycling immediately on boot / power reconnect
 *      (previously it sat blank until an upload).
 *   2. Cycle time is now adjustable live from the web UI (/config endpoint),
 *      persisted in playlist.json — no re-flashing needed.
 *   3. Delete fixed (correct `current` tracking, mutex-guarded screen clear)
 *      and a new /reorder endpoint lets the playlist be reordered.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "DEV_Config.h"
#include "EPD.h"
#include "GUI_Paint.h"

// ── User config ──────────────────────────────────────────────────────────────
const char* WIFI_SSID     = "";
const char* WIFI_PASSWORD = "";
const char* HOSTNAME      = "eink-gallery";
const int   DEFAULT_MINS  = 20;   // cycle time used the very first boot only;
                                  // afterwards it is set from the web UI
// ─────────────────────────────────────────────────────────────────────────────

#define IMG_BYTES     15000
#define PLAYLIST_PATH "/playlist.json"
#define IMAGES_DIR    "/images"
#define MIN_MINS      1
#define MAX_MINS      1440

AsyncWebServer server(80);

// Set whenever the display task should wake early instead of finishing its
// wait: a first upload to an empty queue, or a delete of the current image.
volatile bool newImagePending = false;

// ── Embedded HTML ─────────────────────────────────────────────────────────────
const char INDEX_HTML[] PROGMEM = R"END(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>ink frame</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link href="https://fonts.googleapis.com/css2?family=DM+Mono:ital,wght@0,300;0,400;1,300&family=Fraunces:ital,opsz,wght@0,9..144,200;0,9..144,300;1,9..144,200&display=swap" rel="stylesheet">
<style>
  :root {
    --ink: #0e0e0e;
    --paper: #f2efe8;
    --paper2: #e8e4dc;
    --accent: #1a1a1a;
    --dim: #888880;
    --border: #d0ccc4;
    --red: #c0392b;
  }

  [data-theme="dark"] {
    --ink: #e8e4dc;
    --paper: #18170f;
    --paper2: #201f17;
    --dim: #7a7a70;
    --border: #302f25;
    --red: #e05444;
  }

  * { box-sizing: border-box; margin: 0; padding: 0; }

  body {
    background: var(--paper);
    color: var(--ink);
    font-family: 'DM Mono', monospace;
    min-height: 100vh;
    padding: 0;
  }

  /* ── Header ── */
  header {
    border-bottom: 1px solid var(--border);
    padding: 28px 40px 24px;
    display: flex;
    align-items: baseline;
    gap: 20px;
  }

  .logo {
    font-family: 'Fraunces', serif;
    font-size: 2rem;
    font-weight: 200;
    font-style: italic;
    letter-spacing: -0.02em;
    color: var(--ink);
  }

  .logo span {
    font-style: normal;
    font-weight: 300;
    color: var(--dim);
    font-size: 0.85rem;
    font-family: 'DM Mono', monospace;
    margin-left: 4px;
  }

  .status-dot {
    width: 7px; height: 7px;
    border-radius: 50%;
    background: #aaa;
    transition: background 0.4s;
  }
  .status-dot.online { background: #2ecc71; box-shadow: 0 0 6px #2ecc7188; }
  .status-dot.error  { background: var(--red); }

  /* ── Layout ── */
  main {
    max-width: 860px;
    margin: 0 auto;
    padding: 48px 40px;
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 48px;
  }

  @media (max-width: 640px) {
    main { grid-template-columns: 1fr; padding: 32px 20px; }
    header { padding: 20px; }
  }

  /* ── Section label ── */
  .section-label {
    font-size: 0.65rem;
    letter-spacing: 0.15em;
    text-transform: uppercase;
    color: var(--dim);
    margin-bottom: 16px;
  }

  /* ── Upload area ── */
  .upload-section { grid-column: 1; }

  .drop-zone {
    border: 1.5px dashed var(--border);
    border-radius: 4px;
    aspect-ratio: 4/3;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    transition: border-color 0.2s, background 0.2s;
    position: relative;
    overflow: hidden;
    background: var(--paper2);
  }

  .drop-zone:hover { border-color: var(--ink); }
  .drop-zone.dragover { border-color: var(--ink); background: var(--border); }

  .drop-icon {
    font-size: 2rem;
    margin-bottom: 12px;
    opacity: 0.3;
    line-height: 1;
  }

  .drop-text {
    font-size: 0.7rem;
    color: var(--dim);
    text-align: center;
    line-height: 1.8;
  }

  .drop-zone canvas {
    position: absolute;
    inset: 0;
    width: 100%;
    height: 100%;
    object-fit: cover;
    image-rendering: pixelated;
  }

  /* Preview label overlay */
  .preview-label {
    position: absolute;
    bottom: 8px;
    right: 10px;
    font-size: 0.6rem;
    color: rgba(255,255,255,0.7);
    background: rgba(0,0,0,0.5);
    padding: 2px 6px;
    border-radius: 2px;
    display: none;
  }

  #file-input { display: none; }

  /* ── Controls below drop zone ── */
  .controls {
    margin-top: 16px;
    display: flex;
    flex-direction: column;
    gap: 12px;
  }

  .slider-row {
    display: flex;
    align-items: center;
    gap: 12px;
    font-size: 0.65rem;
    color: var(--dim);
  }

  .slider-row label { width: 72px; flex-shrink: 0; }

  input[type=range] {
    flex: 1;
    -webkit-appearance: none;
    height: 2px;
    background: var(--border);
    border-radius: 1px;
    outline: none;
  }
  input[type=range]::-webkit-slider-thumb {
    -webkit-appearance: none;
    width: 12px; height: 12px;
    border-radius: 50%;
    background: var(--ink);
    cursor: pointer;
  }

  .slider-val { width: 28px; text-align: right; }

  /* ── Upload button ── */
  .btn {
    font-family: 'DM Mono', monospace;
    font-size: 0.7rem;
    letter-spacing: 0.08em;
    padding: 12px 20px;
    border: 1.5px solid var(--ink);
    background: var(--ink);
    color: var(--paper);
    cursor: pointer;
    border-radius: 3px;
    transition: background 0.15s, color 0.15s;
    width: 100%;
    margin-top: 4px;
  }
  .btn:hover { background: transparent; color: var(--ink); }
  .btn:disabled { opacity: 0.35; cursor: not-allowed; }
  .btn.secondary {
    background: transparent;
    color: var(--ink);
  }
  .btn.secondary:hover { background: var(--ink); color: var(--paper); }
  .btn.danger { border-color: var(--red); background: var(--red); color: white; }
  .btn.danger:hover { background: transparent; color: var(--red); }

  /* ── Progress bar ── */
  .progress-wrap {
    height: 2px;
    background: var(--border);
    border-radius: 1px;
    overflow: hidden;
    display: none;
  }
  .progress-bar {
    height: 100%;
    background: var(--ink);
    width: 0%;
    transition: width 0.2s;
  }

  /* ── Status message ── */
  .msg {
    font-size: 0.65rem;
    color: var(--dim);
    min-height: 1.2em;
    text-align: center;
  }
  .msg.ok  { color: #27ae60; }
  .msg.err { color: var(--red); }

  /* ── Playlist ── */
  .playlist-section { grid-column: 2; }

  .playlist-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    margin-bottom: 16px;
  }

  .playlist-count {
    font-size: 0.65rem;
    color: var(--dim);
  }

  .playlist {
    display: flex;
    flex-direction: column;
    gap: 6px;
    max-height: 420px;
    overflow-y: auto;
  }

  .playlist::-webkit-scrollbar { width: 3px; }
  .playlist::-webkit-scrollbar-track { background: transparent; }
  .playlist::-webkit-scrollbar-thumb { background: var(--border); }

  .playlist-item {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 10px 12px;
    border: 1px solid var(--border);
    border-radius: 3px;
    background: var(--paper2);
    transition: border-color 0.15s;
    animation: slideIn 0.25s ease;
  }

  @keyframes slideIn {
    from { opacity: 0; transform: translateY(-6px); }
    to   { opacity: 1; transform: translateY(0); }
  }

  .playlist-item.active {
    border-color: var(--ink);
    background: var(--paper);
  }

  .item-num {
    font-size: 0.6rem;
    color: var(--dim);
    width: 18px;
    flex-shrink: 0;
  }

  .item-thumb {
    width: 40px;
    height: 30px;
    background: var(--border);
    border-radius: 2px;
    flex-shrink: 0;
    overflow: hidden;
  }

  .item-thumb canvas {
    width: 100%;
    height: 100%;
    image-rendering: pixelated;
  }

  .item-name {
    flex: 1;
    font-size: 0.65rem;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .item-badge {
    font-size: 0.55rem;
    padding: 2px 6px;
    border-radius: 2px;
    background: var(--ink);
    color: var(--paper);
    flex-shrink: 0;
  }

  /* ── Reorder controls ── */
  .item-controls {
    display: flex;
    flex-direction: column;
    gap: 1px;
    flex-shrink: 0;
  }
  .item-move {
    font-size: 0.55rem;
    line-height: 1;
    color: var(--dim);
    background: none;
    border: none;
    cursor: pointer;
    padding: 1px 3px;
    transition: color 0.15s;
  }
  .item-move:hover:not(:disabled) { color: var(--ink); }
  .item-move:disabled { opacity: 0.2; cursor: not-allowed; }

  .item-del {
    font-size: 0.7rem;
    color: var(--dim);
    cursor: pointer;
    padding: 2px 4px;
    border-radius: 2px;
    transition: color 0.15s;
    background: none;
    border: none;
    flex-shrink: 0;
  }
  .item-del:hover { color: var(--red); }

  .empty-playlist {
    font-size: 0.7rem;
    color: var(--dim);
    text-align: center;
    padding: 48px 0;
    border: 1.5px dashed var(--border);
    border-radius: 4px;
  }

  /* ── Cycle-time settings panel ── */
  .settings-panel {
    grid-column: 1 / -1;
    border: 1px solid var(--border);
    border-radius: 4px;
    padding: 18px 24px;
    display: flex;
    align-items: center;
    gap: 18px;
    background: var(--paper2);
  }
  .settings-panel .section-label { flex-shrink: 0; }
  .settings-panel input[type=range] { flex: 1; }
  .interval-val {
    font-family: 'Fraunces', serif;
    font-size: 1.15rem;
    font-weight: 200;
    color: var(--ink);
    min-width: 64px;
    text-align: right;
  }

  /* ── Frame status panel ── */
  .status-panel {
    grid-column: 1 / -1;
    border: 1px solid var(--border);
    border-radius: 4px;
    padding: 20px 24px;
    display: grid;
    grid-template-columns: repeat(3, 1fr);
    gap: 24px;
    background: var(--paper2);
  }

  .stat { display: flex; flex-direction: column; gap: 4px; }
  .stat-val {
    font-family: 'Fraunces', serif;
    font-size: 1.5rem;
    font-weight: 200;
    color: var(--ink);
  }
  .stat-label { font-size: 0.6rem; color: var(--dim); letter-spacing: 0.08em; }

  /* ── Theme toggle ── */
  .theme-btn {
    background: none;
    border: 1px solid var(--border);
    color: var(--dim);
    font-family: 'DM Mono', monospace;
    font-size: 0.6rem;
    letter-spacing: 0.1em;
    padding: 5px 12px;
    border-radius: 2px;
    cursor: pointer;
    margin-left: auto;
    margin-right: 14px;
    transition: color 0.2s, border-color 0.2s;
    white-space: nowrap;
  }
  .theme-btn:hover { color: var(--ink); border-color: var(--ink); }

</style>
</head>
<body>

<header>
  <div class="logo">ink frame <span>v1.2</span></div>
  <button class="theme-btn" id="theme-btn" onclick="toggleTheme()">◐ dark</button>
  <div id="status-dot" class="status-dot" title="Frame status"></div>
</header>

<main>

  <!-- ── Upload ── -->
  <section class="upload-section">
    <div class="section-label">Add image</div>

    <div class="drop-zone" id="drop-zone">
      <div class="drop-icon">⬡</div>
      <div class="drop-text">drop a photo here<br>or tap to browse</div>
      <canvas id="preview-canvas"></canvas>
      <div class="preview-label" id="preview-label">preview · 400×300</div>
    </div>
    <input type="file" id="file-input" accept="image/*">

    <div class="controls">
      <div class="slider-row">
        <label>contrast</label>
        <input type="range" id="contrast" min="80" max="220" value="160" step="5">
        <span class="slider-val" id="contrast-val">1.6</span>
      </div>
      <div class="slider-row">
        <label>sharpness</label>
        <input type="range" id="sharpness" min="0" max="10" value="4" step="1">
        <span class="slider-val" id="sharpness-val">mid</span>
      </div>

      <div class="progress-wrap" id="progress-wrap">
        <div class="progress-bar" id="progress-bar"></div>
      </div>

      <button class="btn" id="upload-btn" disabled>send to frame</button>
      <div class="msg" id="msg"></div>
    </div>
  </section>

  <!-- ── Playlist ── -->
  <section class="playlist-section">
    <div class="playlist-header">
      <div class="section-label" style="margin-bottom:0">on frame</div>
      <span class="playlist-count" id="playlist-count">— images</span>
    </div>
    <div class="playlist" id="playlist">
      <div class="empty-playlist">no images yet</div>
    </div>
  </section>

  <!-- ── Cycle time ── -->
  <div class="settings-panel">
    <div class="section-label" style="margin-bottom:0">cycle time</div>
    <input type="range" id="interval" min="1" max="120" value="20" step="1">
    <span class="interval-val" id="interval-val">20 min</span>
  </div>

  <!-- ── Status ── -->
  <div class="status-panel">
    <div class="stat">
      <div class="stat-val" id="stat-images">—</div>
      <div class="stat-label">images stored</div>
    </div>
    <div class="stat">
      <div class="stat-val" id="stat-current">—</div>
      <div class="stat-label">currently showing</div>
    </div>
    <div class="stat">
      <div class="stat-val" id="stat-free">—</div>
      <div class="stat-label">KB free</div>
    </div>
  </div>

</main>

<script>
// ── Constants ──────────────────────────────────────────────────────────────
const W = 400, H = 300;
const dropZone   = document.getElementById('drop-zone');
const fileInput  = document.getElementById('file-input');
const canvas     = document.getElementById('preview-canvas');
const ctx        = canvas.getContext('2d');
const uploadBtn  = document.getElementById('upload-btn');
const msgEl      = document.getElementById('msg');
const progressWrap = document.getElementById('progress-wrap');
const progressBar  = document.getElementById('progress-bar');
const previewLabel = document.getElementById('preview-label');

canvas.width  = W;
canvas.height = H;

let processedBin = null;
let currentFile  = null;
let lastKnownCurrent = -1;   // -1 = not yet set; used to detect photo advances
const thumbCache = {};        // name → ImageData; avoids re-fetching on playlist refresh

// ── Sliders ────────────────────────────────────────────────────────────────
const sharpLabels = ['off','low','low','mid','mid','mid','mid','high','high','high','max'];
document.getElementById('contrast').addEventListener('input', e => {
  document.getElementById('contrast-val').textContent = (e.target.value/100).toFixed(1);
  if (currentFile) processImage(currentFile);
});
document.getElementById('sharpness').addEventListener('input', e => {
  document.getElementById('sharpness-val').textContent = sharpLabels[e.target.value];
  if (currentFile) processImage(currentFile);
});

// ── Drag & drop ────────────────────────────────────────────────────────────
dropZone.addEventListener('click', () => fileInput.click());
dropZone.addEventListener('dragover', e => { e.preventDefault(); dropZone.classList.add('dragover'); });
dropZone.addEventListener('dragleave', () => dropZone.classList.remove('dragover'));
dropZone.addEventListener('drop', e => {
  e.preventDefault();
  dropZone.classList.remove('dragover');
  const f = e.dataTransfer.files[0];
  if (f && f.type.startsWith('image/')) handleFile(f);
});
fileInput.addEventListener('change', e => {
  if (e.target.files[0]) handleFile(e.target.files[0]);
});

// ── Image processing ───────────────────────────────────────────────────────
function handleFile(file) {
  currentFile = file;
  processImage(file);
}

function processImage(file) {
  const reader = new FileReader();
  reader.onload = e => {
    const img = new Image();
    img.onload = () => renderDithered(img);
    img.src = e.target.result;
  };
  reader.readAsDataURL(file);
}

function renderDithered(img) {
  // Smart crop to 4:3
  const targetRatio = W / H;
  let sx=0, sy=0, sw=img.width, sh=img.height;
  const srcRatio = sw / sh;
  if (srcRatio > targetRatio) {
    sw = Math.round(sh * targetRatio);
    sx = Math.round((img.width - sw) / 2);
  } else {
    sh = Math.round(sw / targetRatio);
    sy = Math.round((img.height - sh) * 0.35);
  }

  // Draw to canvas at full res first
  const tmp = document.createElement('canvas');
  tmp.width = W; tmp.height = H;
  const tctx = tmp.getContext('2d');
  tctx.drawImage(img, sx, sy, sw, sh, 0, 0, W, H);

  // Get pixel data
  let data = tctx.getImageData(0, 0, W, H);

  // Contrast
  const contrast = document.getElementById('contrast').value / 100;
  applyContrast(data.data, contrast);

  // Sharpness (unsharp mask approximation)
  const sharpLevel = parseInt(document.getElementById('sharpness').value);
  if (sharpLevel > 0) applySharpness(tctx, data, sharpLevel);
  else tctx.putImageData(data, 0, 0);

  data = tctx.getImageData(0, 0, W, H);

  // To grayscale + Floyd-Steinberg dither
  const gray = toGrayscale(data.data);
  const dithered = floydSteinberg(gray, W, H);

  // Render result
  const out = ctx.createImageData(W, H);
  for (let i = 0; i < dithered.length; i++) {
    const v = dithered[i] ? 255 : 0;
    out.data[i*4]   = v;
    out.data[i*4+1] = v;
    out.data[i*4+2] = v;
    out.data[i*4+3] = 255;
  }
  ctx.putImageData(out, 0, 0);

  // Pack to binary (Waveshare: 0=black, 1=white, MSB first)
  processedBin = packBits(dithered, W, H);

  canvas.style.display = 'block';
  previewLabel.style.display = 'block';
  dropZone.querySelector('.drop-icon').style.display = 'none';
  dropZone.querySelector('.drop-text').style.display = 'none';
  uploadBtn.disabled = false;
  setMsg('');
}

function applyContrast(data, factor) {
  const intercept = 128 * (1 - factor);
  for (let i = 0; i < data.length; i += 4) {
    data[i]   = Math.min(255, Math.max(0, data[i]   * factor + intercept));
    data[i+1] = Math.min(255, Math.max(0, data[i+1] * factor + intercept));
    data[i+2] = Math.min(255, Math.max(0, data[i+2] * factor + intercept));
  }
}

function applySharpness(tctx, imgData, level) {
  tctx.putImageData(imgData, 0, 0);
  // Simple unsharp mask via shadow blur trick
  const amount = level * 0.12;
  const tmp2 = document.createElement('canvas');
  tmp2.width = W; tmp2.height = H;
  const c2 = tmp2.getContext('2d');
  c2.filter = `blur(${1 + level * 0.3}px)`;
  c2.drawImage(tctx.canvas, 0, 0);
  const blurred = c2.getImageData(0, 0, W, H);
  const orig = imgData.data;
  const blur = blurred.data;
  const out = new ImageData(W, H);
  for (let i = 0; i < orig.length; i += 4) {
    out.data[i]   = Math.min(255,Math.max(0, orig[i]   + amount*(orig[i]   - blur[i])));
    out.data[i+1] = Math.min(255,Math.max(0, orig[i+1] + amount*(orig[i+1] - blur[i+1])));
    out.data[i+2] = Math.min(255,Math.max(0, orig[i+2] + amount*(orig[i+2] - blur[i+2])));
    out.data[i+3] = 255;
  }
  tctx.putImageData(out, 0, 0);
}

function toGrayscale(data) {
  const gray = new Float32Array(W * H);
  for (let i = 0; i < W * H; i++) {
    gray[i] = 0.299*data[i*4] + 0.587*data[i*4+1] + 0.114*data[i*4+2];
  }
  return gray;
}

function floydSteinberg(gray, w, h) {
  const buf = new Float32Array(gray);
  const out = new Uint8Array(w * h);
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const i = y * w + x;
      const old = buf[i];
      const nw  = old < 128 ? 0 : 255;
      out[i] = nw === 255 ? 1 : 0;
      const err = old - nw;
      if (x+1 < w)           buf[i+1]     += err * 7/16;
      if (y+1 < h && x > 0)  buf[i+w-1]   += err * 3/16;
      if (y+1 < h)            buf[i+w]     += err * 5/16;
      if (y+1 < h && x+1 < w) buf[i+w+1]  += err * 1/16;
    }
  }
  return out;
}

function packBits(pixels, w, h) {
  // Waveshare: 1=white, 0=black, MSB first
  const bytes = new Uint8Array((w * h) / 8);
  for (let i = 0; i < pixels.length; i += 8) {
    let byte = 0;
    for (let b = 0; b < 8; b++) {
      if (pixels[i + b] === 1) byte |= (1 << (7 - b));
    }
    bytes[i / 8] = byte;
  }
  return bytes;
}

// ── Upload ─────────────────────────────────────────────────────────────────
uploadBtn.addEventListener('click', async () => {
  if (!processedBin) return;
  uploadBtn.disabled = true;
  progressWrap.style.display = 'block';
  progressBar.style.width = '0%';
  setMsg('uploading…');

  const name = (currentFile.name.replace(/\.[^.]+$/, '') + '.bin')
               .replace(/[^a-zA-Z0-9._-]/g, '_');

  try {
    await uploadBin(processedBin, name);
    setMsg('done — image queued', 'ok');
    progressBar.style.width = '100%';
    setTimeout(() => { progressWrap.style.display = 'none'; }, 1000);
    loadPlaylist();
    loadStatus();
  } catch(err) {
    setMsg('upload failed: ' + err.message, 'err');
    progressWrap.style.display = 'none';
  }
  uploadBtn.disabled = false;
});

function uploadBin(data, filename) {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/upload');
    xhr.setRequestHeader('X-Filename', filename);
    xhr.upload.onprogress = e => {
      if (e.lengthComputable)
        progressBar.style.width = (e.loaded / e.total * 90) + '%';
    };
    xhr.onload = () => xhr.status === 200 ? resolve() : reject(new Error('HTTP ' + xhr.status));
    xhr.onerror = () => reject(new Error('network error'));
    xhr.send(data.buffer);
  });
}

// ── Playlist ───────────────────────────────────────────────────────────────
async function loadPlaylist() {
  try {
    const r = await fetch('/playlist');
    const data = await r.json();
    renderPlaylist(data);
    document.getElementById('playlist-count').textContent =
      data.images.length + ' image' + (data.images.length !== 1 ? 's' : '');
  } catch { /* offline */ }
}

function renderPlaylist(data) {
  const el = document.getElementById('playlist');
  if (!data.images || data.images.length === 0) {
    el.innerHTML = '<div class="empty-playlist">no images yet</div>';
    return;
  }
  const last = data.images.length - 1;
  el.innerHTML = data.images.map((name, i) => `
    <div class="playlist-item ${i === data.current ? 'active' : ''}">
      <span class="item-num">${String(i+1).padStart(2,'0')}</span>
      <div class="item-thumb"><canvas id="thumb-${i}" width="40" height="30"></canvas></div>
      <span class="item-name">${name}</span>
      ${i === data.current ? '<span class="item-badge">now</span>' : ''}
      <div class="item-controls">
        <button class="item-move" ${i === 0 ? 'disabled' : ''}
                onclick="moveImage(${i}, ${i-1})" title="move up">▲</button>
        <button class="item-move" ${i === last ? 'disabled' : ''}
                onclick="moveImage(${i}, ${i+1})" title="move down">▼</button>
      </div>
      <button class="item-del" onclick="deleteImage('${name}')" title="remove">✕</button>
    </div>
  `).join('');
  // Paint thumbnails (served as raw 1-bpp binary from the ESP32).
  data.images.forEach((name, i) => loadThumb(name, i));
}

// ── Thumbnails ─────────────────────────────────────────────────────────────
// Fetch /image?name=… (raw Waveshare 1-bpp binary, 400×300, 15 000 bytes),
// decode it to a 40×30 ImageData (sampling every 10th pixel), and paint the
// matching playlist canvas.  Results are cached so a playlist refresh caused
// by a photo advance doesn't re-download anything.
async function loadThumb(name, idx) {
  if (thumbCache[name]) { renderThumb(thumbCache[name], idx); return; }
  try {
    const r = await fetch('/image?name=' + encodeURIComponent(name));
    if (!r.ok) return;
    const bytes = new Uint8Array(await r.arrayBuffer());
    thumbCache[name] = decodeThumb(bytes);
    renderThumb(thumbCache[name], idx);
  } catch {}
}

function decodeThumb(bytes) {
  // Waveshare packing: MSB-first, 1 = white, 0 = black.
  // Downsample 400×300 → 40×30 by picking every 10th pixel.
  const td = new ImageData(40, 30);
  for (let ty = 0; ty < 30; ty++) {
    for (let tx = 0; tx < 40; tx++) {
      const li  = (ty * 10) * 400 + (tx * 10);
      const isW = (bytes[li >> 3] >> (7 - (li & 7))) & 1;
      const v   = isW ? 255 : 0;
      const oi  = (ty * 40 + tx) * 4;
      td.data[oi] = td.data[oi+1] = td.data[oi+2] = v;
      td.data[oi+3] = 255;
    }
  }
  return td;
}

function renderThumb(imgData, idx) {
  const c = document.getElementById('thumb-' + idx);
  if (c) c.getContext('2d').putImageData(imgData, 0, 0);
}

// Move an image to a new slot in the playlist (reorder).
async function moveImage(from, to) {
  try {
    const r = await fetch(`/reorder?from=${from}&to=${to}`, { method: 'POST' });
    if (!r.ok) throw new Error('HTTP ' + r.status);
    loadPlaylist();
    loadStatus();
  } catch { setMsg('reorder failed', 'err'); }
}

async function deleteImage(name) {
  if (!confirm(`Remove "${name}" from the frame?`)) return;
  try {
    const r = await fetch('/delete?name=' + encodeURIComponent(name), { method: 'DELETE' });
    if (!r.ok) throw new Error('HTTP ' + r.status);
    loadPlaylist(); loadStatus();
  } catch { setMsg('delete failed', 'err'); }
}

// ── Cycle time ─────────────────────────────────────────────────────────────
const intervalSlider = document.getElementById('interval');
const intervalVal    = document.getElementById('interval-val');
let   intervalTimer  = null;

intervalSlider.addEventListener('input', () => {
  intervalVal.textContent = intervalSlider.value + ' min';
  // Debounce: only POST 0.5s after the user stops sliding.
  clearTimeout(intervalTimer);
  intervalTimer = setTimeout(saveInterval, 500);
});

async function loadConfig() {
  try {
    const r = await fetch('/config');
    const d = await r.json();
    if (d.intervalMins) {
      intervalSlider.value = d.intervalMins;
      intervalVal.textContent = d.intervalMins + ' min';
    }
  } catch { /* offline */ }
}

async function saveInterval() {
  try {
    const r = await fetch('/config?mins=' + intervalSlider.value, { method: 'POST' });
    if (!r.ok) throw new Error('HTTP ' + r.status);
    setMsg('cycle time set to ' + intervalSlider.value + ' min', 'ok');
  } catch { setMsg('could not update cycle time', 'err'); }
}

// ── Status ─────────────────────────────────────────────────────────────────
async function loadStatus() {
  try {
    const r = await fetch('/status');
    const d = await r.json();
    document.getElementById('stat-images').textContent  = d.count ?? '—';
    document.getElementById('stat-current').textContent = d.current !== undefined ? '#' + (d.current+1) : '—';
    document.getElementById('stat-free').textContent    = d.freeKB ? d.freeKB + ' KB' : '—';
    document.getElementById('status-dot').className = 'status-dot online';
    // If the frame has advanced to a new image, refresh the playlist so the
    // "now" badge and active highlight move to the correct row automatically.
    if (d.current !== undefined) {
      if (lastKnownCurrent !== -1 && d.current !== lastKnownCurrent) loadPlaylist();
      lastKnownCurrent = d.current;
    }
  } catch {
    document.getElementById('status-dot').className = 'status-dot error';
  }
}

// ── Helpers ────────────────────────────────────────────────────────────────
function setMsg(text, type='') {
  msgEl.textContent = text;
  msgEl.className = 'msg' + (type ? ' '+type : '');
}

// ── Theme ───────────────────────────────────────────────────────────────────
function applyTheme(theme) {
  document.documentElement.setAttribute('data-theme', theme);
  const btn = document.getElementById('theme-btn');
  if (btn) btn.textContent = theme === 'dark' ? '◐ light' : '◐ dark';
}

function toggleTheme() {
  const next = (document.documentElement.getAttribute('data-theme') || 'light') === 'dark'
               ? 'light' : 'dark';
  applyTheme(next);
  try { localStorage.setItem('eink-theme', next); } catch {}
}

// ── Init ───────────────────────────────────────────────────────────────────
(function loadTheme() {
  try { const s = localStorage.getItem('eink-theme'); if (s) applyTheme(s); } catch {}
})();
loadPlaylist();
loadStatus();
loadConfig();
setInterval(loadStatus, 15000);
</script>
</body>
</html>
)END";

// ── Playlist ──────────────────────────────────────────────────────────────────

struct Playlist {
    std::vector<String> images;
    int current = 0;
    int intervalMins = DEFAULT_MINS;   // cycle time, adjustable from the web UI
};

Playlist playlist;

void loadPlaylist() {
    playlist.images.clear();
    playlist.current = 0;
    playlist.intervalMins = DEFAULT_MINS;
    if (!LittleFS.exists(PLAYLIST_PATH)) return;
    File f = LittleFS.open(PLAYLIST_PATH, "r");
    if (!f) return;
    JsonDocument doc;
    if (deserializeJson(doc, f) != DeserializationError::Ok) { f.close(); return; }
    f.close();
    playlist.current      = doc["current"]  | 0;
    playlist.intervalMins = doc["interval"] | DEFAULT_MINS;
    if (playlist.intervalMins < MIN_MINS) playlist.intervalMins = MIN_MINS;
    if (playlist.intervalMins > MAX_MINS) playlist.intervalMins = MAX_MINS;
    JsonArray arr = doc["images"].as<JsonArray>();
    for (JsonVariant v : arr) playlist.images.push_back(v.as<String>());
    Serial.printf("[playlist] loaded %d images, current=%d, interval=%dmin\n",
                  playlist.images.size(), playlist.current, playlist.intervalMins);
}

void savePlaylist() {
    File f = LittleFS.open(PLAYLIST_PATH, "w");
    if (!f) return;
    JsonDocument doc;
    doc["current"]  = playlist.current;
    doc["interval"] = playlist.intervalMins;
    JsonArray arr = doc["images"].to<JsonArray>();
    for (auto& name : playlist.images) arr.add(name);
    serializeJson(doc, f);
    f.close();
}

void addToPlaylist(const String& name) {
    for (auto& n : playlist.images) if (n == name) return;
    playlist.images.push_back(name);
    savePlaylist();
}

// Remove an image and keep `current` pointing at the same picture it did
// before (so a delete never silently changes what is on screen).
void removeFromPlaylist(const String& name) {
    auto& v = playlist.images;
    int idx = -1;
    for (int i = 0; i < (int)v.size(); i++) {
        if (v[i] == name) { idx = i; break; }
    }
    if (idx < 0) return;                 // not in the list

    v.erase(v.begin() + idx);

    if (idx < playlist.current) {
        // an item *before* the current one was removed — shift the pointer
        playlist.current--;
    }
    // if the current item itself (or beyond) was removed, clamp into range
    if (playlist.current >= (int)v.size()) playlist.current = 0;
    if (v.empty()) playlist.current = 0;

    savePlaylist();
}

// ── Display ───────────────────────────────────────────────────────────────────

SemaphoreHandle_t displayMutex;

void showImage(const String& filename) {
    String path = String(IMAGES_DIR) + "/" + filename;
    Serial.println("[display] showing " + path);
    File f = LittleFS.open(path, "r");
    if (!f) { Serial.println("[display] file not found"); return; }
    UBYTE* buf = (UBYTE*)malloc(IMG_BYTES);
    if (!buf) { Serial.println("[display] malloc failed"); f.close(); return; }
    size_t read = f.read(buf, IMG_BYTES);
    f.close();
    if (read != IMG_BYTES) {
        Serial.printf("[display] bad size: %d\n", read);
        free(buf); return;
    }
    if (xSemaphoreTake(displayMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
        EPD_4IN2_V2_Init();
        Paint_NewImage(buf, EPD_4IN2_V2_WIDTH, EPD_4IN2_V2_HEIGHT, 0, WHITE);
        Paint_SelectImage(buf);
        EPD_4IN2_V2_Display(buf);
        EPD_4IN2_V2_Sleep();
        xSemaphoreGive(displayMutex);
        Serial.println("[display] done");
    }
    free(buf);
}

// Blank the panel (mutex-guarded so it can't collide with showImage()).
void clearScreen() {
    if (xSemaphoreTake(displayMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
        EPD_4IN2_V2_Init();
        EPD_4IN2_V2_Clear();
        EPD_4IN2_V2_Sleep();
        xSemaphoreGive(displayMutex);
    }
}

// ── Display task (core 0) ─────────────────────────────────────────────────────

void displayTask(void* param) {
    vTaskDelay(pdMS_TO_TICKS(3000));

    for (;;) {
        // ── Show the current image right away ──
        // This runs on every boot / power reconnect, so the frame resumes
        // cycling immediately instead of waiting for a fresh upload.
        if (!playlist.images.empty()) {
            playlist.current = playlist.current % playlist.images.size();
            String name = playlist.images[playlist.current];   // copy before showing
            showImage(name);
            savePlaylist();
        } else {
            Serial.println("[display] playlist empty, waiting for upload...");
        }

        // ── Wait out the cycle interval, or wake early on a signal ──
        unsigned long waited = 0;
        for (;;) {
            // intervalMins is read live, so a change from the web UI takes
            // effect even during the current wait.
            unsigned long intervalMs =
                (unsigned long)playlist.intervalMins * 60UL * 1000UL;

            if (newImagePending) break;   // first upload, or current image deleted
            if (!playlist.images.empty() && waited >= intervalMs) break;

            vTaskDelay(pdMS_TO_TICKS(1000));
            waited += 1000;
        }

        // ── Decide what to show next ──
        if (newImagePending) {
            // A first upload (current already 0) or a delete (which already
            // repointed `current`) — just loop and re-show `current`.
            newImagePending = false;
        } else if (!playlist.images.empty()) {
            // Normal timed advance to the next image in the rotation.
            playlist.current = (playlist.current + 1) % playlist.images.size();
        }
    }
}

// ── Web server ────────────────────────────────────────────────────────────────

void setupServer() {

    server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send_P(200, "text/html", INDEX_HTML);
    });

    server.on("/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        doc["count"]   = playlist.images.size();
        doc["current"] = playlist.current;
        doc["freeKB"]  = (LittleFS.totalBytes() - LittleFS.usedBytes()) / 1024;
        String out; serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    server.on("/playlist", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        doc["current"] = playlist.current;
        JsonArray arr = doc["images"].to<JsonArray>();
        for (auto& n : playlist.images) arr.add(n);
        String out; serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    // ── Serve raw image binary so the browser can render thumbnails ──
    server.on("/image", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!req->hasParam("name")) { req->send(400); return; }
        String name = req->getParam("name")->value();
        name.replace("..", ""); name.replace("/", "");   // sanitise
        String path = String(IMAGES_DIR) + "/" + name;
        if (!LittleFS.exists(path)) { req->send(404); return; }
        req->send(LittleFS, path, "application/octet-stream");
    });

    // ── Cycle-time config ──
    server.on("/config", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        doc["intervalMins"] = playlist.intervalMins;
        String out; serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    server.on("/config", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!req->hasParam("mins")) { req->send(400); return; }
        int m = req->getParam("mins")->value().toInt();
        if (m < MIN_MINS) m = MIN_MINS;
        if (m > MAX_MINS) m = MAX_MINS;
        playlist.intervalMins = m;
        savePlaylist();
        Serial.printf("[config] cycle time set to %d min\n", m);
        req->send(200, "application/json", "{\"ok\":true}");
    });

    // ── Reorder the playlist (move one image from index -> index) ──
    server.on("/reorder", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!req->hasParam("from") || !req->hasParam("to")) { req->send(400); return; }
        int from = req->getParam("from")->value().toInt();
        int to   = req->getParam("to")->value().toInt();
        auto& v = playlist.images;
        if (from < 0 || from >= (int)v.size() ||
            to   < 0 || to   >= (int)v.size()) {
            req->send(400, "application/json", "{\"ok\":false}");
            return;
        }
        if (from != to) {
            // Remember which image is on screen so `current` follows it.
            String showing = (playlist.current >= 0 &&
                              playlist.current < (int)v.size())
                              ? v[playlist.current] : String();
            String moved = v[from];
            v.erase(v.begin() + from);
            v.insert(v.begin() + to, moved);
            for (int i = 0; i < (int)v.size(); i++) {
                if (v[i] == showing) { playlist.current = i; break; }
            }
            savePlaylist();
        }
        req->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/delete", HTTP_DELETE, [](AsyncWebServerRequest* req) {
        if (!req->hasParam("name")) { req->send(400); return; }
        String name = req->getParam("name")->value();

        // Is the image being deleted the one currently on screen?
        bool deletingCurrent = (!playlist.images.empty() &&
                                playlist.current < (int)playlist.images.size() &&
                                playlist.images[playlist.current] == name);

        LittleFS.remove(String(IMAGES_DIR) + "/" + name);
        removeFromPlaylist(name);                 // also fixes up `current`
        req->send(200, "application/json", "{\"ok\":true}");

        if (deletingCurrent) {
            if (!playlist.images.empty()) {
                // Wake the display task to show whatever moved into place.
                newImagePending = true;
            } else {
                // Queue is now empty — blank the panel.
                clearScreen();
            }
        }
    });

    server.on("/upload", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            req->send(200, "application/json", "{\"ok\":true}");
        },
        NULL,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len,
           size_t index, size_t total) {
            if (index == 0) {
                String name = req->header("X-Filename");
                if (name.isEmpty()) name = "image.bin";
                name.replace("..", ""); name.replace("/", "");
                Serial.printf("[upload] receiving %s (%d bytes)\n", name.c_str(), total);
                req->_tempFile = LittleFS.open(String(IMAGES_DIR) + "/" + name, "w");
            }
            if (req->_tempFile) req->_tempFile.write(data, len);
            if (index + len == total) {
                String name = req->header("X-Filename");
                name.replace("..", ""); name.replace("/", "");
                if (req->_tempFile) req->_tempFile.close();

                bool wasEmpty = playlist.images.empty();
                addToPlaylist(name);
                // Only wake the display task if the queue was empty before:
                // an upload onto a non-empty queue just joins the rotation
                // without disrupting whatever is currently showing.
                if (wasEmpty) newImagePending = true;
                Serial.println("[upload] done: " + name);
            }
        }
    );

    server.begin();
    Serial.println("[server] started");
}

// ── Setup ──────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    Serial.println("\n=== E-Ink Gallery ===");

    if (!LittleFS.begin(true)) { Serial.println("LittleFS failed!"); while(1); }
    Serial.printf("[fs] %dKB used / %dKB total\n",
                  LittleFS.usedBytes()/1024, LittleFS.totalBytes()/1024);
    if (!LittleFS.exists(IMAGES_DIR)) LittleFS.mkdir(IMAGES_DIR);

    loadPlaylist();

    DEV_Module_Init();
    EPD_4IN2_V2_Init();
    EPD_4IN2_V2_Clear();
    EPD_4IN2_V2_Sleep();

    Serial.print("[wifi] connecting");
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 30) {
        delay(500); Serial.print("."); tries++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n[wifi] " + WiFi.localIP().toString());
    } else {
        Serial.println("\n[wifi] FAILED");
    }

    if (MDNS.begin(HOSTNAME)) {
        MDNS.addService("http", "tcp", 80);
        Serial.println("[mdns] http://" + String(HOSTNAME) + ".local");
    }

    displayMutex = xSemaphoreCreateMutex();
    setupServer();

    xTaskCreatePinnedToCore(displayTask, "display", 8192, NULL, 1, NULL, 0);
}

void loop() { delay(1000); }
