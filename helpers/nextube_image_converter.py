#!/usr/bin/env python3
"""
Nextube Image Converter v2
Converts images to 80x160 RGB565 raw binary (or JPEG/PNG) for Nextube displays.
Features interactive crop-box with live preview.
Run with: python nextube_image_converter.py  →  open http://localhost:5000
"""

import os, io, sys, zipfile, threading, webbrowser, json, base64, mimetypes
from pathlib import Path
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse

try:
    from PIL import Image
except ImportError:
    print("Pillow not found. Installing...")
    os.system(f"{sys.executable} -m pip install Pillow")
    from PIL import Image

DEFAULT_W, DEFAULT_H = 80, 160
OUTPUT_DIR = Path("nextube_output")
OUTPUT_DIR.mkdir(exist_ok=True)

# ── Image processing ──────────────────────────────────────────────────────────


def process_image(image_data, filename, width, height, output_fmt, crop_box=None):
    """
    crop_box: (x, y, w, h) in original image pixels, or None for auto-center crop.
    """
    img = Image.open(io.BytesIO(image_data))
    orig_w, orig_h = img.size

    if crop_box:
        cx, cy, cw, ch = crop_box
        cx = max(0, min(cx, orig_w - 1))
        cy = max(0, min(cy, orig_h - 1))
        cw = max(1, min(cw, orig_w - cx))
        ch = max(1, min(ch, orig_h - cy))
        img = img.crop((cx, cy, cx + cw, cy + ch))
    else:
        # auto center crop to aspect ratio
        ratio = width / height
        src_ratio = orig_w / orig_h
        if src_ratio > ratio:
            nw = int(orig_h * ratio)
            left = (orig_w - nw) // 2
            img = img.crop((left, 0, left + nw, orig_h))
        else:
            nh = int(orig_w / ratio)
            top = (orig_h - nh) // 2
            img = img.crop((0, top, orig_w, top + nh))

    img = img.resize((width, height), Image.LANCZOS)
    stem = Path(filename).stem

    if output_fmt == "jpeg":
        buf = io.BytesIO()
        img.convert("RGB").save(buf, format="JPEG", quality=80, optimize=True)
        out_bytes = buf.getvalue()
        out_name = f"{stem}_{width}x{height}.jpg"
    else:
        buf = io.BytesIO()
        img.save(buf, format="PNG", optimize=True)
        out_bytes = buf.getvalue()
        out_name = f"{stem}_{width}x{height}.png"

    prev_buf = io.BytesIO()
    img.convert("RGB").save(prev_buf, format="JPEG", quality=75)
    preview_b64 = base64.b64encode(prev_buf.getvalue()).decode()

    return {
        "out_name": out_name,
        "out_bytes": out_bytes,
        "preview_b64": preview_b64,
        "original_name": filename,
        "size": len(out_bytes),
        "dims": f"{width}x{height}",
        "format": output_fmt,
    }


# ── HTML + JS UI ──────────────────────────────────────────────────────────────
HTML = r"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Nextube Image Converter</title>
<style>
@import url('https://fonts.googleapis.com/css2?family=Share+Tech+Mono&family=Exo+2:wght@300;500;700&display=swap');

:root {
  --bg:#0a0e14; --panel:#111820; --panel2:#0d1520;
  --border:#1e3a4a; --border2:#2a4a5e;
  --accent:#00d4ff; --accent2:#ff6b35;
  --text:#c8d8e8; --dim:#4a6a7a;
  --success:#00ff88; --warn:#ffcc00;
  --mono:'Share Tech Mono',monospace;
  --sans:'Exo 2',sans-serif;
}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--text);font-family:var(--sans);min-height:100vh;overflow-x:hidden}
body::before{content:'';position:fixed;inset:0;
  background-image:linear-gradient(rgba(0,212,255,.03) 1px,transparent 1px),
    linear-gradient(90deg,rgba(0,212,255,.03) 1px,transparent 1px);
  background-size:40px 40px;pointer-events:none;z-index:0}
.container{max-width:1300px;margin:0 auto;padding:1.5rem;position:relative;z-index:1}

/* Header */
header{display:flex;align-items:center;gap:1rem;margin-bottom:2rem;padding-bottom:1.25rem;border-bottom:1px solid var(--border)}
.logo{width:40px;height:40px;background:var(--accent);clip-path:polygon(50% 0%,100% 25%,100% 75%,50% 100%,0% 75%,0% 25%);display:flex;align-items:center;justify-content:center;font-family:var(--mono);font-size:.6rem;color:#000;font-weight:bold;flex-shrink:0}
h1{font-family:var(--mono);font-size:1.3rem;color:var(--accent);letter-spacing:2px;text-transform:uppercase}
h1 span{color:var(--accent2)}
.subtitle{font-size:.7rem;color:var(--dim);letter-spacing:1px;margin-top:.2rem}

/* Layout */
.layout{display:grid;grid-template-columns:280px 1fr;gap:1.25rem;align-items:start}
.right-col{display:flex;flex-direction:column;gap:1.25rem}

/* Panel */
.panel{background:var(--panel);border:1px solid var(--border);border-radius:4px;padding:1.25rem;position:relative}
.panel::before{content:'';position:absolute;top:0;left:0;right:0;height:2px;background:linear-gradient(90deg,var(--accent),transparent)}
.panel-title{font-family:var(--mono);font-size:.68rem;color:var(--accent);letter-spacing:2px;text-transform:uppercase;margin-bottom:1rem;display:flex;align-items:center;gap:.4rem}
.panel-title::before{content:'//';color:var(--accent2)}

/* Form elements */
lbl{display:block;font-size:.65rem;color:var(--dim);letter-spacing:1px;text-transform:uppercase;margin-bottom:.35rem;margin-top:.9rem}
lbl:first-of-type{margin-top:0}
.dim-row{display:grid;grid-template-columns:1fr auto 1fr;gap:.4rem;align-items:center}
.dim-sep{font-family:var(--mono);color:var(--accent);text-align:center}
input[type=number],select{width:100%;background:var(--panel2);border:1px solid var(--border);color:var(--text);padding:.45rem .65rem;font-family:var(--mono);font-size:.82rem;border-radius:3px;outline:none;transition:border-color .2s}
input[type=number]:focus,select:focus{border-color:var(--accent)}
.radio-group{display:flex;gap:.4rem;flex-wrap:wrap}
.radio-btn{flex:1;min-width:60px}
.radio-btn input{display:none}
.radio-btn label{display:block;text-align:center;padding:.4rem .4rem;border:1px solid var(--border);border-radius:3px;cursor:pointer;font-size:.65rem;font-family:var(--mono);transition:all .15s;background:var(--panel2);line-height:1.4;margin:0}
.radio-btn input:checked+label{border-color:var(--accent);color:var(--accent);background:rgba(0,212,255,.08)}

/* Drop zone */
#dropzone{border:2px dashed var(--border);border-radius:4px;padding:1.75rem 1rem;text-align:center;cursor:pointer;transition:all .2s;position:relative;background:var(--panel2);margin-top:.9rem}
#dropzone.drag-over{border-color:var(--accent);background:rgba(0,212,255,.05)}
#dropzone input[type=file]{position:absolute;inset:0;opacity:0;cursor:pointer;width:100%;height:100%}
.drop-icon{font-size:2rem;margin-bottom:.5rem;display:block}
.drop-text{font-family:var(--mono);font-size:.72rem;color:var(--dim);line-height:1.6}
.drop-text strong{color:var(--accent)}

/* Buttons */
.btn{display:inline-flex;align-items:center;gap:.4rem;padding:.5rem 1rem;border:none;border-radius:3px;font-family:var(--mono);font-size:.75rem;letter-spacing:1px;cursor:pointer;transition:all .15s;text-transform:uppercase}
.btn-primary{background:var(--accent);color:#000;width:100%;justify-content:center;margin-top:1rem;font-weight:bold}
.btn-primary:hover{background:#33ddff}
.btn-primary:disabled{opacity:.4;cursor:not-allowed}
.btn-sm{background:transparent;border:1px solid var(--accent2);color:var(--accent2);font-size:.65rem;padding:.3rem .6rem}
.btn-sm:hover{background:rgba(255,107,53,.1)}
.btn-zip{background:var(--accent2);color:#000;font-size:.72rem;padding:.45rem .9rem;font-weight:bold}
.btn-zip:hover{background:#ff8555}
.btn-reset{background:transparent;border:1px solid var(--dim);color:var(--dim);font-size:.65rem;padding:.3rem .6rem}
.btn-reset:hover{border-color:var(--accent2);color:var(--accent2)}

/* Progress */
#progress-wrap{margin-top:1rem;display:none}
.prog-label{font-family:var(--mono);font-size:.65rem;color:var(--dim);margin-bottom:.35rem;display:flex;justify-content:space-between}
.prog-bar-bg{height:3px;background:var(--border);border-radius:2px;overflow:hidden}
.prog-bar{height:100%;background:linear-gradient(90deg,var(--accent),var(--success));border-radius:2px;width:0%;transition:width .3s}

/* File queue */
.file-queue{margin-top:.75rem;max-height:100px;overflow-y:auto;scrollbar-width:thin;scrollbar-color:var(--border) transparent}
.file-item{font-family:var(--mono);font-size:.65rem;color:var(--dim);padding:.2rem 0;border-bottom:1px solid rgba(30,58,74,.4);display:flex;justify-content:space-between;gap:.5rem}
.file-item .fname{overflow:hidden;text-overflow:ellipsis;white-space:nowrap;color:var(--text)}
.file-item .fsize{color:var(--dim);flex-shrink:0}

/* ── Crop Editor ─────────────────────────────────────────────────────────── */
.crop-editor{display:none;gap:1.25rem}
.crop-editor.active{display:grid;grid-template-columns:1fr 120px}

.crop-stage-wrap{position:relative;background:#000;border:1px solid var(--border);border-radius:3px;overflow:hidden;line-height:0;cursor:crosshair;user-select:none}
#crop-img{display:block;max-width:100%;max-height:480px;width:100%}

/* Crop overlay = darkened areas outside box */
.crop-overlay{position:absolute;inset:0;pointer-events:none}

/* The actual draggable box */
#crop-box{
  position:absolute;
  border:2px solid var(--accent);
  box-shadow:0 0 0 9999px rgba(0,0,0,.55);
  cursor:move;
  pointer-events:all;
}
#crop-box::before,#crop-box::after,
.cb-bl,  .cb-br{
  content:'';position:absolute;
  width:12px;height:12px;
  border-color:var(--accent);border-style:solid;
}
/* corners */
#crop-box::before{top:-1px;left:-1px;border-width:2px 0 0 2px}
#crop-box::after {top:-1px;right:-1px;border-width:2px 2px 0 0}
.cb-bl{bottom:-1px;left:-1px;border-width:0 0 2px 2px}
.cb-br{bottom:-1px;right:-1px;border-width:0 2px 2px 0}

/* resize handles */
.rh{position:absolute;width:10px;height:10px;background:var(--accent);border-radius:1px;pointer-events:all}
.rh-n {top:-5px;left:50%;transform:translateX(-50%);cursor:n-resize}
.rh-s {bottom:-5px;left:50%;transform:translateX(-50%);cursor:s-resize}
.rh-e {right:-5px;top:50%;transform:translateY(-50%);cursor:e-resize}
.rh-w {left:-5px;top:50%;transform:translateY(-50%);cursor:w-resize}

/* Preview pane */
.preview-pane{display:flex;flex-direction:column;gap:.75rem}
.preview-label{font-family:var(--mono);font-size:.62rem;color:var(--dim);letter-spacing:1px;text-transform:uppercase}
#preview-canvas{width:80px;height:160px;border:1px solid var(--border);display:block;image-rendering:pixelated;background:#000}
.crop-coords{font-family:var(--mono);font-size:.6rem;color:var(--dim);line-height:1.8}
.crop-coords span{color:var(--text)}

/* Results */
#results-panel{display:none}
.results-header{display:flex;justify-content:space-between;align-items:center;margin-bottom:1rem}
.stat-badge{font-family:var(--mono);font-size:.65rem;color:var(--success);background:rgba(0,255,136,.1);border:1px solid rgba(0,255,136,.3);padding:.2rem .5rem;border-radius:2px}
.result-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(120px,1fr));gap:.75rem}
.result-card{background:var(--panel2);border:1px solid var(--border);border-radius:4px;overflow:hidden;transition:border-color .2s;animation:fadeUp .3s ease both}
.result-card:hover{border-color:var(--accent)}
@keyframes fadeUp{from{opacity:0;transform:translateY(8px)}to{opacity:1;transform:translateY(0)}}
.result-card img{width:100%;aspect-ratio:1/2;object-fit:cover;display:block}
.card-info{padding:.5rem}
.card-name{font-family:var(--mono);font-size:.6rem;color:var(--text);white-space:nowrap;overflow:hidden;text-overflow:ellipsis;margin-bottom:.25rem}
.card-meta{font-family:var(--mono);font-size:.55rem;color:var(--dim);margin-bottom:.4rem}

/* Error */
.err-msg{font-family:var(--mono);font-size:.68rem;color:#ff4444;padding:.45rem;background:rgba(255,68,68,.08);border:1px solid rgba(255,68,68,.2);border-radius:3px;margin-top:.75rem;display:none}

::-webkit-scrollbar{width:4px}
::-webkit-scrollbar-track{background:transparent}
::-webkit-scrollbar-thumb{background:var(--border);border-radius:2px}

@media(max-width:760px){.layout{grid-template-columns:1fr}.crop-editor.active{grid-template-columns:1fr}}
</style>
</head>
<body>
<div class="container">
  <header>
    <div class="logo">ESP<br>32</div>
    <div>
      <h1>IMAGE <span>CONVERTER</span></h1>
      <div class="subtitle">// JPEG · PNG · INTERACTIVE CROP · BULK</div>
    </div>
  </header>

  <div class="layout">
    <!-- ── Left: Controls ── -->
    <div class="panel">
      <div class="panel-title">Configuration</div>

      <lbl>Output Dimensions</lbl>
      <div class="dim-row">
        <input type="number" id="width" value="80" min="1" max="4096">
        <div class="dim-sep">×</div>
        <input type="number" id="height" value="160" min="1" max="4096">
      </div>

      <lbl>Output Format</lbl>
      <div class="radio-group">
        <div class="radio-btn"><input type="radio" name="fmt" id="fmt-jpeg" value="jpeg" checked><label for="fmt-jpeg">JPEG<br><span style="color:var(--dim);font-size:.55rem">.jpg</span></label></div>
        <div class="radio-btn"><input type="radio" name="fmt" id="fmt-png"  value="png"><label for="fmt-png">PNG<br><span style="color:var(--dim);font-size:.55rem">.png</span></label></div>
      </div>

      <lbl>Crop Mode</lbl>
      <div class="radio-group">
        <div class="radio-btn"><input type="radio" name="cropmode" id="cm-manual" value="manual" checked><label for="cm-manual">Manual<br><span style="color:var(--dim);font-size:.55rem">drag box</span></label></div>
        <div class="radio-btn"><input type="radio" name="cropmode" id="cm-auto"   value="auto"><label for="cm-auto">Auto<br><span style="color:var(--dim);font-size:.55rem">center</span></label></div>
        <div class="radio-btn"><input type="radio" name="cropmode" id="cm-stretch" value="stretch"><label for="cm-stretch">Stretch<br><span style="color:var(--dim);font-size:.55rem">fill</span></label></div>
      </div>

      <div id="dropzone">
        <input type="file" id="file-input" accept="image/*" multiple>
        <span class="drop-icon">⬡</span>
        <div class="drop-text"><strong>Drop images here</strong><br>or click to browse<br><span style="font-size:.6rem">JPG · PNG · BMP · WEBP · GIF</span></div>
      </div>

      <div class="file-queue" id="file-queue"></div>
      <div class="err-msg" id="err-msg"></div>

      <button class="btn btn-primary" id="convert-btn" disabled>▶ CONVERT</button>

      <div id="progress-wrap">
        <div class="prog-label"><span id="prog-text">Processing...</span><span id="prog-pct">0%</span></div>
        <div class="prog-bar-bg"><div class="prog-bar" id="prog-bar"></div></div>
      </div>
    </div>

    <!-- ── Right ── -->
    <div class="right-col">

      <!-- Crop Editor (shows after first image loaded) -->
      <div class="panel" id="crop-panel" style="display:none">
        <div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:1rem">
          <div class="panel-title" style="margin-bottom:0">Crop Editor</div>
          <div style="display:flex;gap:.5rem;align-items:center">
            <span id="img-nav" style="font-family:var(--mono);font-size:.65rem;color:var(--dim)"></span>
            <button class="btn btn-reset" id="btn-prev">◀</button>
            <button class="btn btn-reset" id="btn-next">▶</button>
            <button class="btn btn-reset" id="btn-reset-crop">Reset</button>
          </div>
        </div>

        <div class="crop-editor active">
          <!-- Stage -->
          <div class="crop-stage-wrap" id="crop-stage">
            <img id="crop-img" src="" alt="crop preview">
            <div id="crop-box">
              <div class="cb-bl"></div><div class="cb-br"></div>
              <div class="rh rh-n" data-dir="n"></div>
              <div class="rh rh-s" data-dir="s"></div>
              <div class="rh rh-e" data-dir="e"></div>
              <div class="rh rh-w" data-dir="w"></div>
            </div>
          </div>

          <!-- Preview pane -->
          <div class="preview-pane">
            <div class="preview-label">Output Preview</div>
            <canvas id="preview-canvas" width="80" height="160"></canvas>
            <div class="crop-coords">
              X: <span id="cx-val">0</span><br>
              Y: <span id="cy-val">0</span><br>
              W: <span id="cw-val">0</span><br>
              H: <span id="ch-val">0</span>
            </div>
          </div>
        </div>
      </div>

      <!-- Results -->
      <div class="panel" id="results-panel">
        <div class="results-header">
          <div class="panel-title" style="margin-bottom:0">Results</div>
          <div style="display:flex;align-items:center;gap:.75rem">
            <span class="stat-badge" id="stat-badge">0 files</span>
            <button class="btn btn-zip" id="dl-all-btn">↓ ZIP ALL</button>
          </div>
        </div>
        <div class="result-grid" id="result-grid"></div>
      </div>

    </div><!-- /right-col -->
  </div><!-- /layout -->
</div><!-- /container -->

<script>
// ═══════════════════════════════════════════════════════════════
//  State
// ═══════════════════════════════════════════════════════════════
let files = [];          // File objects queued
let cropData = {};       // { fileIndex: {x,y,w,h} } — in original image coords
let results = [];
let currentIdx = 0;

// Loaded image natural dimensions
let natW = 0, natH = 0;

// Crop box state (in display pixels on the stage)
let box = {x:0,y:0,w:0,h:0};
let drag = null;   // {type:'move'|'n'|'s'|'e'|'w', startX,startY,startBox}

// ═══════════════════════════════════════════════════════════════
//  DOM refs
// ═══════════════════════════════════════════════════════════════
const fileInput   = document.getElementById('file-input');
const dropzone    = document.getElementById('dropzone');
const convertBtn  = document.getElementById('convert-btn');
const fileQueue   = document.getElementById('file-queue');
const resultGrid  = document.getElementById('result-grid');
const resultsPanel= document.getElementById('results-panel');
const progWrap    = document.getElementById('progress-wrap');
const progBar     = document.getElementById('prog-bar');
const progText    = document.getElementById('prog-text');
const progPct     = document.getElementById('prog-pct');
const errMsg      = document.getElementById('err-msg');
const statBadge   = document.getElementById('stat-badge');
const dlAllBtn    = document.getElementById('dl-all-btn');
const cropPanel   = document.getElementById('crop-panel');
const cropStage   = document.getElementById('crop-stage');
const cropImg     = document.getElementById('crop-img');
const cropBox     = document.getElementById('crop-box');
const previewCanvas= document.getElementById('preview-canvas');
const pctx        = previewCanvas.getContext('2d');
const imgNav      = document.getElementById('img-nav');

function getMode()    { return document.querySelector('input[name=cropmode]:checked').value; }
function getOutW()    { return parseInt(document.getElementById('width').value)||80; }
function getOutH()    { return parseInt(document.getElementById('height').value)||160; }
function getFmt()     { return document.querySelector('input[name=fmt]:checked').value; }
function formatBytes(b){ return b<1024?b+' B':b<1048576?(b/1024).toFixed(1)+' KB':(b/1048576).toFixed(1)+' MB'; }

// ═══════════════════════════════════════════════════════════════
//  Drag & drop / file handling
// ═══════════════════════════════════════════════════════════════
dropzone.addEventListener('dragover', e=>{e.preventDefault();dropzone.classList.add('drag-over')});
dropzone.addEventListener('dragleave', ()=>dropzone.classList.remove('drag-over'));
dropzone.addEventListener('drop', e=>{
  e.preventDefault(); dropzone.classList.remove('drag-over');
  addFiles([...e.dataTransfer.files].filter(f=>f.type.startsWith('image/')));
});
fileInput.addEventListener('change', ()=>addFiles([...fileInput.files]));

function addFiles(newFiles){
  const start = files.length;
  files = [...files, ...newFiles];
  renderQueue();
  convertBtn.disabled = files.length===0;
  hideErr();
  if(files.length>0 && start===0) loadCropEditor(0);
}

function renderQueue(){
  fileQueue.innerHTML = files.map((f,i)=>`
    <div class="file-item">
      <span class="fname">${f.name}</span>
      <span class="fsize">${formatBytes(f.size)}</span>
    </div>`).join('');
}

// ═══════════════════════════════════════════════════════════════
//  Crop Editor
// ═══════════════════════════════════════════════════════════════
function loadCropEditor(idx){
  if(!files[idx]){ cropPanel.style.display='none'; return; }
  currentIdx = idx;
  imgNav.textContent = `${idx+1} / ${files.length}`;
  cropPanel.style.display='block';

  const url = URL.createObjectURL(files[idx]);
  cropImg.onload = ()=>{
    URL.revokeObjectURL(url);
    natW = cropImg.naturalWidth;
    natH = cropImg.naturalHeight;
    // Restore saved crop or set default
    if(cropData[idx]){
      setBoxFromNat(cropData[idx]);
    } else {
      defaultBox();
    }
    drawBox();
    updatePreview();
  };
  cropImg.src = url;
}

// Default box = centered crop at target aspect ratio
function defaultBox(){
  const sw = cropStage.clientWidth;
  const sh = cropImg.clientHeight;
  const ratio = getOutW() / getOutH();
  let bw, bh;
  if(sw/sh > ratio){ bh=sh; bw=Math.round(bh*ratio); }
  else             { bw=sw; bh=Math.round(bw/ratio); }
  bw = Math.min(bw,sw); bh = Math.min(bh,sh);
  box = {x:Math.round((sw-bw)/2), y:Math.round((sh-bh)/2), w:bw, h:bh};
}

// Set display box from natural coords
function setBoxFromNat(nat){
  const scaleX = cropImg.clientWidth  / natW;
  const scaleY = cropImg.clientHeight / natH;
  box = {
    x: Math.round(nat.x*scaleX),
    y: Math.round(nat.y*scaleY),
    w: Math.round(nat.w*scaleX),
    h: Math.round(nat.h*scaleY),
  };
}

// Save current display box back to natural coords for current file
function saveNat(){
  const scaleX = natW / cropImg.clientWidth;
  const scaleY = natH / cropImg.clientHeight;
  cropData[currentIdx] = {
    x: Math.round(box.x*scaleX),
    y: Math.round(box.y*scaleY),
    w: Math.round(box.w*scaleX),
    h: Math.round(box.h*scaleY),
  };
  // Update coord display
  const d = cropData[currentIdx];
  document.getElementById('cx-val').textContent = d.x;
  document.getElementById('cy-val').textContent = d.y;
  document.getElementById('cw-val').textContent = d.w;
  document.getElementById('ch-val').textContent = d.h;
}

function drawBox(){
  cropBox.style.left  = box.x+'px';
  cropBox.style.top   = box.y+'px';
  cropBox.style.width = box.w+'px';
  cropBox.style.height= box.h+'px';
}

// Clamp box to stage
function clampBox(){
  const sw = cropImg.clientWidth;
  const sh = cropImg.clientHeight;
  box.w = Math.max(10, Math.min(box.w, sw));
  box.h = Math.max(10, Math.min(box.h, sh));
  box.x = Math.max(0,  Math.min(box.x, sw-box.w));
  box.y = Math.max(0,  Math.min(box.y, sh-box.h));
}

// Keep aspect ratio locked when resizing
function lockAspect(edge, dx, dy){
  const ratio = getOutW() / getOutH();
  let {x,y,w,h} = drag.startBox;

  if(edge==='n'){
    h = h - dy; y = y + dy;
    w = Math.round(h*ratio);
    x = drag.startBox.x + Math.round((drag.startBox.w - w)/2);
  } else if(edge==='s'){
    h = h + dy;
    w = Math.round(h*ratio);
    x = drag.startBox.x + Math.round((drag.startBox.w - w)/2);
  } else if(edge==='e'){
    w = w + dx;
    h = Math.round(w/ratio);
    y = drag.startBox.y + Math.round((drag.startBox.h - h)/2);
  } else if(edge==='w'){
    w = w - dx; x = x + dx;
    h = Math.round(w/ratio);
    y = drag.startBox.y + Math.round((drag.startBox.h - h)/2);
  }
  if(w<20){ w=20; h=Math.round(w/ratio); }
  return {x,y,w,h};
}

// ── Pointer events ────────────────────────────────────────────
cropBox.addEventListener('pointerdown', e=>{
  if(getMode()!=='manual') return;
  const dir = e.target.dataset.dir;
  e.preventDefault(); e.stopPropagation();
  drag = {type: dir||'move', startX:e.clientX, startY:e.clientY, startBox:{...box}};
  cropBox.setPointerCapture(e.pointerId);
});

cropStage.addEventListener('pointerdown', e=>{
  if(getMode()!=='manual') return;
  if(e.target===cropBox||cropBox.contains(e.target)) return;
  e.preventDefault();
  // Start a new box from scratch
  const rect = cropStage.getBoundingClientRect();
  const sx = e.clientX - rect.left;
  const sy = e.clientY - rect.top;
  drag = {type:'draw', startX:sx, startY:sy, startBox:{x:sx,y:sy,w:0,h:0}};
  box = {x:sx,y:sy,w:0,h:0};
  drawBox();
});

window.addEventListener('pointermove', e=>{
  if(!drag) return;
  const dx = e.clientX - drag.startX;
  const dy = e.clientY - drag.startY;

  if(drag.type==='move'){
    box.x = drag.startBox.x + dx;
    box.y = drag.startBox.y + dy;
  } else if(drag.type==='draw'){
    const rect = cropStage.getBoundingClientRect();
    const cx = e.clientX - rect.left;
    const cy = e.clientY - rect.top;
    const ratio = getOutW()/getOutH();
    let w = cx - drag.startX;
    let h = cy - drag.startY;
    // lock aspect from the larger delta
    if(Math.abs(w) > Math.abs(h)) h = w/ratio;
    else w = h*ratio;
    box.x = w<0 ? drag.startX+w : drag.startX;
    box.y = h<0 ? drag.startY+h : drag.startY;
    box.w = Math.abs(w);
    box.h = Math.abs(h);
  } else {
    // resize handle
    const nb = lockAspect(drag.type, dx, dy);
    box = nb;
  }

  clampBox();
  drawBox();
  saveNat();
  updatePreview();
});

window.addEventListener('pointerup', ()=>{ drag=null; });

// Recalc on window resize
window.addEventListener('resize', ()=>{
  if(!files[currentIdx]) return;
  requestAnimationFrame(()=>{
    if(cropData[currentIdx]) setBoxFromNat(cropData[currentIdx]);
    else defaultBox();
    drawBox();
  });
});

// Nav buttons
document.getElementById('btn-prev').addEventListener('click',()=>{ if(currentIdx>0) loadCropEditor(currentIdx-1); });
document.getElementById('btn-next').addEventListener('click',()=>{ if(currentIdx<files.length-1) loadCropEditor(currentIdx+1); });
document.getElementById('btn-reset-crop').addEventListener('click',()=>{ delete cropData[currentIdx]; defaultBox(); drawBox(); saveNat(); updatePreview(); });

// Re-run default when dimensions change
document.getElementById('width').addEventListener('change',  ()=>{ if(files.length) { delete cropData[currentIdx]; defaultBox(); drawBox(); saveNat(); updatePreview(); }});
document.getElementById('height').addEventListener('change', ()=>{ if(files.length) { delete cropData[currentIdx]; defaultBox(); drawBox(); saveNat(); updatePreview(); }});

// Mode toggle
document.querySelectorAll('input[name=cropmode]').forEach(r=>r.addEventListener('change',()=>{
  const visible = getMode()==='manual';
  cropBox.style.pointerEvents = visible ? 'all':'none';
  cropBox.style.opacity = visible ? '1':'0.3';
}));

// ── Preview canvas ────────────────────────────────────────────
function updatePreview(){
  if(!cropImg.naturalWidth) return;
  const tw = getOutW(), th = getOutH();
  previewCanvas.width  = tw;
  previewCanvas.height = th;

  const mode = getMode();
  if(mode==='stretch'){
    pctx.drawImage(cropImg, 0,0, tw, th);
  } else if(mode==='auto'){
    // center crop preview
    const ratio = tw/th;
    const srcRatio = natW/natH;
    let sx=0,sy=0,sw=natW,sh=natH;
    if(srcRatio>ratio){ sw=Math.round(natH*ratio); sx=(natW-sw)/2; }
    else              { sh=Math.round(natW/ratio); sy=(natH-sh)/2; }
    // draw using the img element for simplicity
    const scaleX=cropImg.clientWidth/natW, scaleY=cropImg.clientHeight/natH;
    pctx.drawImage(cropImg, sx*scaleX, sy*scaleY, sw*scaleX, sh*scaleY, 0,0, tw, th);
  } else {
    // manual
    pctx.drawImage(cropImg, box.x, box.y, box.w, box.h, 0,0, tw, th);
  }
}

// ═══════════════════════════════════════════════════════════════
//  Convert
// ═══════════════════════════════════════════════════════════════
convertBtn.addEventListener('click', async ()=>{
  if(!files.length) return;
  hideErr();
  results = [];
  resultGrid.innerHTML='';
  resultsPanel.style.display='none';
  progWrap.style.display='block';
  convertBtn.disabled=true;

  const width  = getOutW();
  const height = getOutH();
  const fmt    = getFmt();
  const mode   = getMode();

  for(let i=0;i<files.length;i++){
    const f = files[i];
    progBar.style.width = Math.round(i/files.length*100)+'%';
    progPct.textContent  = Math.round(i/files.length*100)+'%';
    progText.textContent = `Processing ${i+1}/${files.length}: ${f.name}`;

    try {
      const fd = new FormData();
      fd.append('file', f);
      fd.append('width', width);
      fd.append('height', height);
      fd.append('format', fmt);
      fd.append('cropmode', mode);

      // Attach crop coords if manual mode and we have them
      if(mode==='manual' && cropData[i]){
        const d = cropData[i];
        fd.append('crop_x', d.x);
        fd.append('crop_y', d.y);
        fd.append('crop_w', d.w);
        fd.append('crop_h', d.h);
      }

      const res  = await fetch('/convert', {method:'POST', body:fd});
      const data = await res.json();
      if(data.error) throw new Error(data.error);
      results.push(data);
      addCard(data);
    } catch(e){
      showErr(`Error: ${f.name} — ${e.message}`);
    }
  }

  progBar.style.width='100%';
  progPct.textContent='100%';
  progText.textContent='Done!';
  if(results.length){
    statBadge.textContent=`${results.length} file${results.length>1?'s':''}`;
    resultsPanel.style.display='block';
  }
  convertBtn.disabled=false;
  files=[]; cropData={}; renderQueue(); fileInput.value='';
  cropPanel.style.display='none';
});

function addCard(r){
  const card = document.createElement('div');
  card.className='result-card';
  card.innerHTML=`
    <img src="data:image/jpeg;base64,${r.preview_b64}" alt="${r.out_name}">
    <div class="card-info">
      <div class="card-name">${r.out_name}</div>
      <div class="card-meta">${r.dims} · ${formatBytes(r.size)}</div>
      <button class="btn btn-sm" onclick="downloadOne('${r.out_name}')">↓ Save</button>
    </div>`;
  resultGrid.appendChild(card);
}

// ═══════════════════════════════════════════════════════════════
//  Downloads
// ═══════════════════════════════════════════════════════════════
async function downloadOne(name){
  const res = await fetch('/download/'+encodeURIComponent(name));
  triggerDownload(await res.blob(), name);
}
dlAllBtn.addEventListener('click', async ()=>{
  dlAllBtn.textContent='...';
  const res = await fetch('/download_zip',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({files:results.map(r=>r.out_name)})});
  triggerDownload(await res.blob(),'nextube_images.zip');
  dlAllBtn.textContent='↓ ZIP ALL';
});
function triggerDownload(blob,name){
  const url=URL.createObjectURL(blob);
  const a=document.createElement('a');
  a.href=url;a.download=name;
  document.body.appendChild(a);a.click();
  document.body.removeChild(a);URL.revokeObjectURL(url);
}
window.downloadOne=downloadOne;

function showErr(msg){errMsg.textContent=msg;errMsg.style.display='block'}
function hideErr(){errMsg.style.display='none'}
</script>
</body>
</html>
"""

# ── HTTP Server ───────────────────────────────────────────────────────────────
class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args): pass

    def send_json(self, data, status=200):
        body = json.dumps(data).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = urlparse(self.path).path
        if path in ("/", "/index.html"):
            body = HTML.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif path.startswith("/download/"):
            fname = path[len("/download/"):]
            fpath = OUTPUT_DIR / fname
            if not fpath.exists():
                self.send_response(404); self.end_headers(); return
            data = fpath.read_bytes()
            mt, _ = mimetypes.guess_type(str(fpath))
            self.send_response(200)
            self.send_header("Content-Type", mt or "application/octet-stream")
            self.send_header("Content-Disposition", f'attachment; filename="{fname}"')
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        else:
            self.send_response(404); self.end_headers()

    def do_POST(self):
        path = urlparse(self.path).path
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length)
        if path == "/convert":       self._handle_convert(body)
        elif path == "/download_zip": self._handle_zip(body)
        else: self.send_response(404); self.end_headers()

    def _parse_multipart(self, body):
        ct = self.headers.get("Content-Type", "")
        boundary = None
        for part in ct.split(";"):
            part = part.strip()
            if part.startswith("boundary="):
                boundary = part[len("boundary="):].strip('"').encode()
        if not boundary: return {}
        result = {}
        for part in body.split(b"--" + boundary)[1:]:
            if part.strip() in (b"", b"--", b"--\r\n"): continue
            if b"\r\n\r\n" not in part: continue
            hraw, content = part.split(b"\r\n\r\n", 1)
            content = content.rstrip(b"\r\n")
            name = filename = None
            for line in hraw.decode(errors="replace").splitlines():
                if "Content-Disposition" in line:
                    for seg in line.split(";"):
                        seg = seg.strip()
                        if seg.startswith('name='): name = seg[5:].strip('"')
                        elif seg.startswith('filename='): filename = seg[9:].strip('"')
            if name: result[name] = {"value": content, "filename": filename}
        return result

    def _handle_convert(self, body):
        try:
            f = self._parse_multipart(body)
            image_data = f["file"]["value"]
            filename   = f["file"]["filename"] or "image.jpg"
            width      = int(f["width"]["value"])
            height     = int(f["height"]["value"])
            fmt        = f["format"]["value"].decode()
            mode       = f.get("cropmode", {}).get("value", b"auto").decode()

            crop_box = None
            if mode == "manual" and "crop_x" in f:
                crop_box = (
                    int(f["crop_x"]["value"]),
                    int(f["crop_y"]["value"]),
                    int(f["crop_w"]["value"]),
                    int(f["crop_h"]["value"]),
                )
            elif mode == "stretch":
                crop_box = None  # process_image stretches when crop_box=None and mode handled below

            # Pass mode to process_image via crop_box logic
            if mode == "stretch":
                # Open image and stretch directly — no crop
                img = Image.open(io.BytesIO(image_data))
                img = img.resize((width, height), Image.LANCZOS)
                stem = Path(filename).stem
                if fmt == "jpeg":
                    buf = io.BytesIO(); img.convert("RGB").save(buf, "JPEG", quality=80)
                    out_bytes = buf.getvalue(); out_name = f"{stem}_{width}x{height}.jpg"
                else:
                    buf = io.BytesIO(); img.save(buf, "PNG", optimize=True)
                    out_bytes = buf.getvalue(); out_name = f"{stem}_{width}x{height}.png"
                prev = io.BytesIO(); img.convert("RGB").save(prev, "JPEG", quality=72)
                result = {"out_name":out_name,"out_bytes":out_bytes,
                          "preview_b64":base64.b64encode(prev.getvalue()).decode(),
                          "size":len(out_bytes),"dims":f"{width}x{height}","format":fmt}
            else:
                result = process_image(image_data, filename, width, height, fmt, crop_box)

            (OUTPUT_DIR / result["out_name"]).write_bytes(result["out_bytes"])
            self.send_json({"out_name":result["out_name"],"preview_b64":result["preview_b64"],
                            "size":result["size"],"dims":result["dims"],"format":result["format"]})
        except Exception as e:
            self.send_json({"error": str(e)}, 500)

    def _handle_zip(self, body):
        try:
            names = json.loads(body).get("files", [])
            buf = io.BytesIO()
            with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zf:
                for name in names:
                    fpath = OUTPUT_DIR / name
                    if fpath.exists(): zf.write(fpath, name)
            buf.seek(0); content = buf.read()
            self.send_response(200)
            self.send_header("Content-Type", "application/zip")
            self.send_header("Content-Disposition", 'attachment; filename="nextube_images.zip"')
            self.send_header("Content-Length", str(len(content)))
            self.end_headers()
            self.wfile.write(content)
        except Exception as e:
            self.send_json({"error": str(e)}, 500)


# ── Entry point ───────────────────────────────────────────────────────────────
if __name__ == "__main__":
    port = 5000
    server = HTTPServer(("0.0.0.0", port), Handler)
    url = f"http://localhost:{port}"
    print(f"\n  Nextube Image Converter v2")
    print(f"  ────────────────────────")
    print(f"  Running at : {url}")
    print(f"  Output dir : {OUTPUT_DIR.resolve()}")
    print(f"  Press Ctrl+C to stop\n")
    threading.Timer(0.8, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n  Stopped.")
