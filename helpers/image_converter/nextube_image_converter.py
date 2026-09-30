#!/usr/bin/env python3
"""
Nextube Image Converter v3
Converts images to 80×160 JPEG for Nextube displays.
Features: interactive crop editor, bulk processing, .zipper theme import.
Run with: python nextube_image_converter.py  →  opens http://localhost:5000
"""

import os, io, sys, zipfile, threading, webbrowser, json, base64, mimetypes, uuid, shutil
from pathlib import Path
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, unquote

try:
    from PIL import Image
except ImportError:
    print("Pillow not found. Installing...")
    os.system(f"{sys.executable} -m pip install Pillow")
    from PIL import Image

DEFAULT_W, DEFAULT_H = 80, 160
OUTPUT_DIR = Path("nextube_output")
OUTPUT_DIR.mkdir(exist_ok=True)

# ── Zipper theme support ──────────────────────────────────────────────────────
# A .zipper is a ZIP of PNG assets created by the original Nextube desktop app.
# Maps lowercase PNG stem → path within a theme folder on LittleFS:
#   /images/themes/{ThemeName}/{out_path}
#
# Default suggested crop — tuned against real 300×300 zipper assets.
# Centred horizontally; Y=107 frames the digit correctly (not the geometric
# centre, which clips the top of tall digits).  Scaled proportionally when
# source dimensions differ from the reference 300×300.
ZIPPER_CROP_REF   = (300, 300)          # reference source size
ZIPPER_CROP_XYWH  = (112, 107, 76, 152) # x, y, w, h at the reference size
ZIPPER_ROLE_MAP = {
    '0': 'Numbers/0.jpg', '1': 'Numbers/1.jpg', '2': 'Numbers/2.jpg',
    '3': 'Numbers/3.jpg', '4': 'Numbers/4.jpg', '5': 'Numbers/5.jpg',
    '6': 'Numbers/6.jpg', '7': 'Numbers/7.jpg', '8': 'Numbers/8.jpg',
    '9': 'Numbers/9.jpg',
    'am':    'AMPM/am.jpg',
    'pm':    'AMPM/pm.jpg',
    'blank': 'AMPM/blank.jpg',
    'colon': 'AMPM/colon.jpg',
}
ZIPPER_STEM_ORDER = list(ZIPPER_ROLE_MAP.keys())  # canonical display order

STAGING_DIR = OUTPUT_DIR / "_staging"
STAGING_DIR.mkdir(exist_ok=True)


def suggest_crop(img_w, img_h, out_w=DEFAULT_W, out_h=DEFAULT_H):
    """Centre-crop box (x, y, w, h) at out_w:out_h aspect ratio."""
    ratio = out_w / out_h
    if img_w / img_h > ratio:           # image wider than target — trim sides
        w = int(img_h * ratio); h = img_h
        return (img_w - w) // 2, 0, w, h
    else:                               # image taller than target — trim top/bottom
        w = img_w; h = int(img_w / ratio)
        return 0, (img_h - h) // 2, w, h


def make_thumb(img, cx, cy, cw, ch, tw=DEFAULT_W, th=DEFAULT_H):
    """Crop to (cx,cy,cw,ch), resize to tw×th, return base64 JPEG string."""
    cropped = img.crop((cx, cy, cx + cw, cy + ch))
    cropped = cropped.resize((tw, th), Image.LANCZOS)
    buf = io.BytesIO()
    cropped.convert("RGB").save(buf, "JPEG", quality=75)
    return base64.b64encode(buf.getvalue()).decode()


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
        out_name = f"{stem}.jpg"
    else:
        buf = io.BytesIO()
        img.save(buf, format="PNG", optimize=True)
        out_bytes = buf.getvalue()
        out_name = f"{stem}.png"

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
/* Design tokens mirror the clock's own web UI (data/web/index.html) — same
 * navy background, the --hi pink-crimson brand accent, card treatment,
 * radius/shadow scale and system font stack — so this tool reads as part of
 * the same project instead of a standalone skin. --panel/--panel2 stand in
 * for the clock's --card/--accent (its input-background token); --accent2
 * takes the clock's --warn amber as the one deliberately-distinct "special
 * action" color (zipper import), the same role --warn already plays there. */
:root {
  --bg:#13152a; --panel:#1b1e36; --panel2:#262c4c;
  --border:rgba(255,255,255,.08); --border2:rgba(255,255,255,.16);
  --accent:#ef4565; --accent-dark:#a13350; --accent-rgb:239,69,101;
  --accent2:#e8a23a; --accent2-dark:#b9791f;
  --text:#eef0f6; --dim:#8d94ac;
  --success:#4ecca3; --warn:#e8a23a; --danger:#e34b3c;
  --mono:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;
  --sans:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;
  --radius-sm:6px; --radius:10px; --radius-lg:14px;
  --shadow-sm:0 1px 3px rgba(0,0,0,.3); --shadow:0 6px 20px rgba(0,0,0,.32);
  --ease:cubic-bezier(.4,0,.2,1);
}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--text);font-family:var(--sans);min-height:100vh;
  overflow-x:hidden;-webkit-font-smoothing:antialiased;text-rendering:optimizeLegibility}
.container{max-width:1300px;margin:0 auto;padding:1.5rem;position:relative;z-index:1}

/* Header */
header{display:flex;align-items:center;gap:1rem;margin-bottom:1.5rem;padding:1.1rem 1.5rem;
  background:var(--panel);border:1px solid var(--border);border-radius:var(--radius-lg);box-shadow:var(--shadow-sm)}
.logo{width:40px;height:40px;border-radius:var(--radius);flex-shrink:0;
  background:linear-gradient(135deg,var(--accent),var(--accent-dark));
  display:flex;align-items:center;justify-content:center;font-size:1.3rem;
  box-shadow:0 2px 5px rgba(var(--accent-rgb),.2)}
h1{font-size:1.15rem;font-weight:700;color:var(--text)}
h1 span{color:var(--accent)}
.subtitle{font-size:.75rem;color:var(--dim);margin-top:.2rem}

/* Layout */
.layout{display:grid;grid-template-columns:280px 1fr;gap:1.25rem;align-items:start}
.right-col{display:flex;flex-direction:column;gap:1.25rem}

/* Panel — same diagonal-wash card treatment as the clock UI's .card */
.panel{background:linear-gradient(150deg,var(--panel2),var(--panel) 65%);border:1px solid var(--border);
  border-radius:var(--radius-lg);padding:1.3rem;position:relative;
  box-shadow:var(--shadow-sm),0 14px 34px -10px rgba(var(--accent-rgb),.14);
  transition:box-shadow .2s var(--ease),border-color .2s var(--ease)}
.panel:hover{box-shadow:var(--shadow-sm),0 18px 38px -8px rgba(var(--accent-rgb),.2);border-color:rgba(var(--accent-rgb),.18)}
.panel-title{font-size:.95rem;font-weight:600;color:var(--text);margin-bottom:1rem;padding-bottom:.7rem;
  border-bottom:1px solid var(--border);display:flex;align-items:center;gap:.55rem}
.panel-title::before{content:'';width:3px;height:1rem;border-radius:2px;
  background:linear-gradient(var(--accent),var(--accent-dark));flex:0 0 auto}

/* Form elements */
lbl{display:block;font-size:.8rem;font-weight:500;color:var(--dim);margin-bottom:.35rem;margin-top:.9rem}
lbl:first-of-type{margin-top:0}
.dim-row{display:grid;grid-template-columns:1fr auto 1fr;gap:.4rem;align-items:center}
.dim-sep{color:var(--accent);text-align:center;font-weight:600}
input[type=number],input[type=text],select{width:100%;background:var(--panel2);border:1px solid var(--border);
  color:var(--text);padding:.55rem .7rem;font-family:var(--sans);font-size:.88rem;border-radius:var(--radius-sm);
  outline:none;box-shadow:inset 0 1px 3px rgba(0,0,0,.25);transition:border-color .15s var(--ease),box-shadow .15s var(--ease)}
input[type=number]:focus,input[type=text]:focus,select:focus{border-color:var(--accent);
  box-shadow:inset 0 1px 3px rgba(0,0,0,.25),0 0 0 3px rgba(var(--accent-rgb),.18)}
select option{background:var(--panel2);color:var(--text)}
.radio-group{display:flex;gap:.4rem;flex-wrap:wrap}
.radio-btn{flex:1;min-width:60px}
.radio-btn input{display:none}
.radio-btn label{display:block;text-align:center;padding:.45rem .4rem;border:1px solid var(--border);
  border-radius:var(--radius-sm);cursor:pointer;font-size:.72rem;font-family:var(--sans);
  transition:all .15s var(--ease);background:var(--panel2);line-height:1.4;margin:0;color:var(--dim)}
.radio-btn input:checked+label{border-color:transparent;color:#fff;
  background:linear-gradient(135deg,var(--accent),var(--accent-dark));
  box-shadow:0 2px 5px rgba(var(--accent-rgb),.15)}

/* Drop zone */
#dropzone{border:2px dashed var(--border);border-radius:var(--radius);padding:1.75rem 1rem;text-align:center;
  cursor:pointer;transition:all .2s var(--ease);position:relative;background:var(--panel2);margin-top:.9rem}
#dropzone.drag-over{border-color:var(--accent);background:rgba(var(--accent-rgb),.06)}
#dropzone.zipper-drag{border-color:var(--accent2);background:rgba(232,162,58,.08)}
#dropzone input[type=file]{position:absolute;inset:0;opacity:0;cursor:pointer;width:100%;height:100%}
.drop-icon{font-size:2rem;margin-bottom:.5rem;display:block}
.drop-text{font-size:.78rem;color:var(--dim);line-height:1.6}
.drop-text strong{color:var(--accent)}
.drop-text .zipper-hint{color:var(--accent2);font-size:.68rem;display:block;margin-top:.3rem}

/* Buttons */
.btn{display:inline-flex;align-items:center;gap:.4rem;padding:.6rem 1.1rem;border:none;border-radius:var(--radius-sm);
  font-family:var(--sans);font-size:.82rem;font-weight:600;letter-spacing:.01em;cursor:pointer;
  transition:filter .15s var(--ease),transform .15s var(--ease),box-shadow .2s var(--ease)}
.btn:active{transform:translateY(1px)}
.btn-primary{background:linear-gradient(135deg,var(--accent),var(--accent-dark));color:#fff;width:100%;
  justify-content:center;margin-top:1rem;box-shadow:0 2px 5px rgba(var(--accent-rgb),.15),0 7px 16px -8px rgba(var(--accent-rgb),.32)}
.btn-primary:hover{transform:translateY(-1px);box-shadow:0 3px 7px rgba(var(--accent-rgb),.18),0 10px 20px -8px rgba(var(--accent-rgb),.4)}
.btn-primary:disabled{opacity:.4;cursor:not-allowed;transform:none}
.btn-sm{background:none;border:1px solid var(--border);color:var(--dim);font-size:.72rem;padding:.35rem .65rem}
.btn-sm:hover{border-color:var(--text);color:var(--text)}
.btn-zip{background:linear-gradient(135deg,var(--accent2),var(--accent2-dark));color:#1a1300;font-size:.8rem;
  padding:.5rem .95rem;font-weight:700;box-shadow:0 2px 5px rgba(232,162,58,.2),0 7px 16px -8px rgba(232,162,58,.4)}
.btn-zip:hover{transform:translateY(-1px);box-shadow:0 3px 7px rgba(232,162,58,.24),0 10px 20px -8px rgba(232,162,58,.5)}
.btn-zip:disabled{opacity:.4;cursor:not-allowed;transform:none}
.btn-reset{background:none;border:1px solid var(--border);color:var(--dim);font-size:.72rem;padding:.35rem .65rem}
.btn-reset:hover{border-color:var(--text);color:var(--text)}
.btn-zipper-import{background:rgba(232,162,58,.12);border:1px solid var(--accent2);color:var(--accent2);
  width:100%;margin-top:.5rem;justify-content:center;font-size:.78rem}
.btn-zipper-import:hover{background:rgba(232,162,58,.22)}

/* Progress */
#progress-wrap{margin-top:1rem;display:none}
.prog-label{font-size:.72rem;color:var(--dim);margin-bottom:.35rem;display:flex;justify-content:space-between}
.prog-bar-bg{height:4px;background:var(--border);border-radius:2px;overflow:hidden}
.prog-bar{height:100%;background:linear-gradient(90deg,var(--accent),var(--success));border-radius:2px;width:0%;transition:width .3s var(--ease)}

/* File queue */
.file-queue{margin-top:.75rem;max-height:100px;overflow-y:auto;scrollbar-width:thin;scrollbar-color:var(--border) transparent}
.file-item{font-size:.72rem;color:var(--dim);padding:.25rem 0;border-bottom:1px solid var(--border);display:flex;justify-content:space-between;gap:.5rem}
.file-item .fname{overflow:hidden;text-overflow:ellipsis;white-space:nowrap;color:var(--text)}
.file-item .fsize{color:var(--dim);flex-shrink:0;font-family:var(--mono)}

/* ── Zipper panel ─────────────────────────────────────────────────────────── */
.zipper-theme-row{display:flex;align-items:flex-end;gap:.6rem;margin-bottom:.9rem}
.zipper-theme-row .field-wrap{flex:1}
.zipper-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(72px,1fr));gap:.5rem;margin:.5rem 0 .75rem}
.zipper-asset{background:var(--panel2);border:1px solid var(--border);border-radius:var(--radius-sm);overflow:hidden;cursor:pointer;transition:border-color .15s var(--ease);position:relative}
.zipper-asset:hover{border-color:var(--border2)}
.zipper-asset.zactive{border-color:var(--accent);box-shadow:0 0 0 1px var(--accent)}
.zipper-asset.crop-edited::after{content:'✎';position:absolute;top:2px;right:3px;font-size:.65rem;color:var(--warn);line-height:1}
.zipper-asset img{width:100%;aspect-ratio:1/2;object-fit:cover;display:block}
.zipper-asset-label{font-size:.65rem;color:var(--text);padding:.2rem .3rem .05rem;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.zipper-asset-path{font-size:.58rem;color:var(--dim);padding:0 .3rem .2rem;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.zipper-missing{font-size:.75rem;color:var(--warn);background:rgba(232,162,58,.1);border:1px solid rgba(232,162,58,.25);border-radius:var(--radius-sm);padding:.4rem .6rem;margin-bottom:.75rem}
.zipper-missing span{color:var(--dim)}
.zipper-actions{display:flex;align-items:center;gap:.75rem;flex-wrap:wrap}
.zipper-status{font-size:.75rem;color:var(--dim)}
.zipper-loading{display:flex;align-items:center;justify-content:center;padding:2rem;font-size:.82rem;color:var(--dim);gap:.75rem}

/* ── Crop Editor ─────────────────────────────────────────────────────────── */
.crop-editor{display:none;gap:1.25rem}
.crop-editor.active{display:grid;grid-template-columns:1fr 120px}
.crop-stage-wrap{position:relative;background:#000;border:1px solid var(--border);border-radius:var(--radius-sm);overflow:hidden;line-height:0;cursor:crosshair;user-select:none;width:fit-content;max-width:100%}
#crop-img{display:block;max-width:100%;max-height:480px;width:auto;height:auto}
.crop-overlay{position:absolute;inset:0;pointer-events:none}
#crop-box{
  position:absolute;
  border:2px solid var(--accent);
  box-shadow:0 0 0 9999px rgba(0,0,0,.55);
  cursor:move;
  pointer-events:all;
}
#crop-box::before,#crop-box::after,
.cb-bl,.cb-br{
  content:'';position:absolute;
  width:12px;height:12px;
  border-color:var(--accent);border-style:solid;
}
#crop-box::before{top:-1px;left:-1px;border-width:2px 0 0 2px}
#crop-box::after {top:-1px;right:-1px;border-width:2px 2px 0 0}
.cb-bl{bottom:-1px;left:-1px;border-width:0 0 2px 2px}
.cb-br{bottom:-1px;right:-1px;border-width:0 2px 2px 0}
.rh{position:absolute;width:10px;height:10px;background:var(--accent);border-radius:2px;pointer-events:all}
.rh-n {top:-5px;left:50%;transform:translateX(-50%);cursor:n-resize}
.rh-s {bottom:-5px;left:50%;transform:translateX(-50%);cursor:s-resize}
.rh-e {right:-5px;top:50%;transform:translateY(-50%);cursor:e-resize}
.rh-w {left:-5px;top:50%;transform:translateY(-50%);cursor:w-resize}
.preview-pane{display:flex;flex-direction:column;gap:.75rem}
.preview-label{font-size:.72rem;color:var(--dim);text-transform:uppercase;letter-spacing:.03em}
#preview-canvas{width:80px;height:160px;border:1px solid var(--border);display:block;image-rendering:pixelated;background:#000;border-radius:var(--radius-sm)}
.crop-coords{font-size:.7rem;color:var(--dim)}
.crop-coords label{display:flex;align-items:center;justify-content:space-between;gap:.4rem;margin-top:.4rem}
.crop-coords label:first-of-type{margin-top:0}
.coord-input{width:58px;background:var(--panel2);border:1px solid var(--border);color:var(--text);
  font-family:var(--mono);font-size:.72rem;padding:.25rem .4rem;border-radius:var(--radius-sm);outline:none;
  text-align:right;-moz-appearance:textfield;box-shadow:inset 0 1px 3px rgba(0,0,0,.25);transition:border-color .15s var(--ease)}
.coord-input:focus{border-color:var(--accent);box-shadow:inset 0 1px 3px rgba(0,0,0,.25),0 0 0 3px rgba(var(--accent-rgb),.18)}
.coord-apply{width:100%;margin-top:.6rem;justify-content:center}

/* Results */
#results-panel{display:none}
.results-header{display:flex;justify-content:space-between;align-items:center;margin-bottom:1rem}
.stat-badge{font-size:.72rem;color:var(--success);background:rgba(78,204,163,.12);border:1px solid rgba(78,204,163,.3);padding:.25rem .55rem;border-radius:var(--radius-sm)}
.result-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(120px,1fr));gap:.75rem}
.result-card{background:var(--panel2);border:1px solid var(--border);border-radius:var(--radius);overflow:hidden;transition:border-color .2s var(--ease);animation:fadeUp .3s ease both}
.result-card:hover{border-color:var(--accent)}
@keyframes fadeUp{from{opacity:0;transform:translateY(8px)}to{opacity:1;transform:translateY(0)}}
.result-card img{width:100%;aspect-ratio:1/2;object-fit:cover;display:block}
.card-info{padding:.5rem}
.card-name{font-size:.72rem;color:var(--text);white-space:nowrap;overflow:hidden;text-overflow:ellipsis;margin-bottom:.25rem}
.card-meta{font-size:.65rem;color:var(--dim);margin-bottom:.4rem;font-family:var(--mono)}

/* Error */
.err-msg{font-size:.78rem;color:var(--danger);padding:.5rem .6rem;background:rgba(227,75,60,.1);border:1px solid rgba(227,75,60,.3);border-radius:var(--radius-sm);margin-top:.75rem;display:none}

::-webkit-scrollbar{width:8px;height:8px}
::-webkit-scrollbar-track{background:transparent}
::-webkit-scrollbar-thumb{background-color:rgba(255,255,255,.14);border-radius:8px;border:2px solid var(--bg);background-clip:padding-box}
::-webkit-scrollbar-thumb:hover{background-color:rgba(255,255,255,.22)}
::selection{background:rgba(var(--accent-rgb),.35);color:#fff}
:focus-visible{outline:2px solid var(--accent);outline-offset:2px;border-radius:2px}

@media(max-width:760px){.layout{grid-template-columns:1fr}.crop-editor.active{grid-template-columns:1fr}}
</style>
</head>
<body>
<div class="container">
  <header>
    <div class="logo">🖼️</div>
    <div>
      <h1>Nextube <span>Image Converter</span></h1>
      <div class="subtitle">JPEG · PNG output · interactive crop · bulk · .zipper import</div>
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

      <lbl>Crop Mode</lbl>
      <div class="radio-group">
        <div class="radio-btn"><input type="radio" name="cropmode" id="cm-manual" value="manual" checked><label for="cm-manual">Manual<br><span style="color:var(--dim);font-size:.55rem">drag box</span></label></div>
        <div class="radio-btn"><input type="radio" name="cropmode" id="cm-auto"   value="auto"><label for="cm-auto">Auto<br><span style="color:var(--dim);font-size:.55rem">center</span></label></div>
        <div class="radio-btn"><input type="radio" name="cropmode" id="cm-stretch" value="stretch"><label for="cm-stretch">Stretch<br><span style="color:var(--dim);font-size:.55rem">fill</span></label></div>
      </div>

      <lbl>Output Format</lbl>
      <div class="radio-group">
        <div class="radio-btn"><input type="radio" name="outfmt" id="fmt-jpeg" value="jpeg" checked><label for="fmt-jpeg">JPEG<br><span style="color:var(--dim);font-size:.55rem">themes</span></label></div>
        <div class="radio-btn"><input type="radio" name="outfmt" id="fmt-png"  value="png"><label for="fmt-png">PNG<br><span style="color:var(--dim);font-size:.55rem">system icons</span></label></div>
      </div>

      <input type="file" id="folder-input" webkitdirectory accept="image/*" style="display:none">
      <input type="file" id="zipper-input" accept=".zipper" style="display:none">

      <div id="dropzone">
        <input type="file" id="file-input" accept="image/*" multiple>
        <span class="drop-icon">⬡</span>
        <div class="drop-text">
          <strong>Drop images here</strong><br>
          or click to browse<br>
          <span style="font-size:.6rem">JPG · PNG · BMP · WEBP · GIF</span>
          <span class="zipper-hint">📦 Drop a .zipper theme file to import</span>
        </div>
      </div>
      <button class="btn btn-reset" id="btn-folder" style="width:100%;margin-top:.5rem;justify-content:center" onclick="document.getElementById('folder-input').click()">📁 Browse Folder</button>
      <button class="btn btn-zipper-import" id="btn-zipper" onclick="document.getElementById('zipper-input').click()">📦 Import .zipper Theme</button>

      <div id="file-queue-head" style="display:none;justify-content:space-between;align-items:center;margin-top:.75rem">
        <span style="font-size:.72rem;color:var(--dim)">Queued files</span>
        <button class="btn btn-reset" id="btn-clear-files" style="font-size:.65rem">✕ Clear</button>
      </div>
      <div class="file-queue" id="file-queue"></div>
      <div class="err-msg" id="err-msg"></div>

      <button class="btn btn-primary" id="convert-btn" disabled>▶ Convert</button>

      <div id="progress-wrap">
        <div class="prog-label"><span id="prog-text">Processing...</span><span id="prog-pct">0%</span></div>
        <div class="prog-bar-bg"><div class="prog-bar" id="prog-bar"></div></div>
      </div>
    </div>

    <!-- ── Right ── -->
    <div class="right-col">

      <!-- Zipper Import Panel (shown after .zipper is extracted) -->
      <div class="panel" id="zipper-panel" style="display:none">
        <div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:.85rem">
          <div class="panel-title" style="margin-bottom:0">Zipper Theme Import</div>
          <button class="btn btn-reset" id="btn-clear-zipper" style="font-size:.6rem">✕ Clear</button>
        </div>
        <div class="zipper-theme-row">
          <div class="field-wrap">
            <lbl style="margin-top:0">Theme Name</lbl>
            <input type="text" id="zipper-theme-name" placeholder="MyTheme" style="font-size:.82rem">
          </div>
        </div>
        <div id="zipper-grid" class="zipper-grid"></div>
        <div id="zipper-missing" class="zipper-missing" style="display:none">
          ⚠ Missing assets: <span id="zipper-missing-list"></span>
        </div>
        <div class="zipper-actions">
          <button class="btn btn-zip" id="btn-convert-zipper">↓ Convert &amp; Download Theme ZIP</button>
          <span class="zipper-status" id="zipper-status"></span>
        </div>
        <div style="font-family:var(--mono);font-size:.57rem;color:var(--dim);margin-top:.6rem;line-height:1.7">
          Click any thumbnail to fine-tune its crop in the editor below.<br>
          The ZIP unpacks directly into your LittleFS <code style="color:var(--accent)">/images/themes/</code> folder.
        </div>
      </div>

      <!-- Crop Editor (shows after first image loaded) -->
      <div class="panel" id="crop-panel" style="display:none">
        <div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:1rem">
          <div class="panel-title" style="margin-bottom:0">Crop Editor</div>
          <div style="display:flex;gap:.5rem;align-items:center">
            <span id="img-nav" style="font-family:var(--mono);font-size:.65rem;color:var(--dim)"></span>
            <button class="btn btn-reset" id="btn-prev">◀</button>
            <button class="btn btn-reset" id="btn-next">▶</button>
            <button class="btn btn-reset" id="btn-reset-crop">Reset</button>
            <button class="btn btn-reset" id="btn-apply-all" style="display:none"
              title="Copy this crop box to every image in the batch">Apply to All</button>
            <label id="lock-crop-label"
              style="display:none;align-items:center;gap:.3rem;font-family:var(--mono);
                     font-size:.62rem;color:var(--dim);cursor:pointer;white-space:nowrap;
                     user-select:none;padding:.3rem .5rem;border:1px solid var(--border);border-radius:3px"
              title="Mirror every crop adjustment to all images automatically">
              <input type="checkbox" id="lock-crop" style="accent-color:var(--accent);cursor:pointer">
              Lock to all
            </label>
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
              <label>X: <input type="number" id="cx-val" class="coord-input"></label>
              <label>Y: <input type="number" id="cy-val" class="coord-input"></label>
              <label>W: <input type="number" id="cw-val" class="coord-input"></label>
              <label>H: <input type="number" id="ch-val" class="coord-input"></label>
              <button class="btn btn-reset coord-apply" id="btn-apply-coords" type="button">Apply XY/WH</button>
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
            <button class="btn btn-zip" id="dl-all-btn">↓ Zip All</button>
          </div>
        </div>
        <div class="result-grid" id="result-grid"></div>
      </div>

    </div><!-- /right-col -->
  </div><!-- /layout -->
</div><!-- /container -->

<script>
// ═══════════════════════════════════════════════════════════════
//  State — regular image mode
// ═══════════════════════════════════════════════════════════════
let files = [];
let fileRelPaths = [];
let cropData = {};
let results = [];
let currentIdx = 0;
let lockCrop = false;
let natW = 0, natH = 0;
let box = {x:0,y:0,w:0,h:0};
let drag = null;

// ═══════════════════════════════════════════════════════════════
//  State — zipper mode
// ═══════════════════════════════════════════════════════════════
let zipperMode = false;
let zipperSessionId = null;
let zipperAssets = [];          // [{stem, out_path, staging_file, w, h, suggested_crop, thumb_b64}]
let zipperCropOverrides = {};   // {stem: {x,y,w,h}} — user-adjusted crops
let zipperCurrentIdx = -1;

// ═══════════════════════════════════════════════════════════════
//  DOM refs
// ═══════════════════════════════════════════════════════════════
const fileInput    = document.getElementById('file-input');
const dropzone     = document.getElementById('dropzone');
const convertBtn   = document.getElementById('convert-btn');
const fileQueue    = document.getElementById('file-queue');
const resultGrid   = document.getElementById('result-grid');
const resultsPanel = document.getElementById('results-panel');
const progWrap     = document.getElementById('progress-wrap');
const progBar      = document.getElementById('prog-bar');
const progText     = document.getElementById('prog-text');
const progPct      = document.getElementById('prog-pct');
const errMsg       = document.getElementById('err-msg');
const statBadge    = document.getElementById('stat-badge');
const dlAllBtn     = document.getElementById('dl-all-btn');
const cropPanel    = document.getElementById('crop-panel');
const cropStage    = document.getElementById('crop-stage');
const cropImg      = document.getElementById('crop-img');
const cropBox      = document.getElementById('crop-box');
const previewCanvas= document.getElementById('preview-canvas');
const pctx         = previewCanvas.getContext('2d');
const imgNav       = document.getElementById('img-nav');
const zipperPanel  = document.getElementById('zipper-panel');

function getMode()    { return document.querySelector('input[name=cropmode]:checked').value; }
function getOutW()    { return parseInt(document.getElementById('width').value)||80; }
function getOutH()    { return parseInt(document.getElementById('height').value)||160; }
function getFmt()     { return document.querySelector('input[name=outfmt]:checked').value; }
function formatBytes(b){ return b<1024?b+' B':b<1048576?(b/1024).toFixed(1)+' KB':(b/1048576).toFixed(1)+' MB'; }

// ═══════════════════════════════════════════════════════════════
//  Drag & drop / file handling
// ═══════════════════════════════════════════════════════════════
dropzone.addEventListener('dragover', e=>{
  e.preventDefault();
  const isZipper = [...e.dataTransfer.items].some(i=>i.kind==='file'&&(i.type===''||i.type==='application/zip'));
  dropzone.classList.toggle('drag-over', !isZipper);
  dropzone.classList.toggle('zipper-drag', isZipper);
});
dropzone.addEventListener('dragleave', ()=>{
  dropzone.classList.remove('drag-over','zipper-drag');
});
dropzone.addEventListener('drop', e=>{
  e.preventDefault();
  dropzone.classList.remove('drag-over','zipper-drag');
  const dropped = [...e.dataTransfer.files];
  const zipperFile = dropped.find(f=>f.name.toLowerCase().endsWith('.zipper'));
  if(zipperFile){ handleZipperDrop(zipperFile); return; }
  addFiles(dropped.filter(f=>f.type.startsWith('image/')), []);
});
fileInput.addEventListener('change', ()=>addFiles([...fileInput.files], []));
document.getElementById('folder-input').addEventListener('change', e=>{
  const newFiles = [...e.target.files];
  const relPaths = newFiles.map(f=>f.webkitRelativePath||'');
  addFiles(newFiles, relPaths);
  e.target.value='';
});
document.getElementById('zipper-input').addEventListener('change', e=>{
  if(e.target.files[0]) handleZipperDrop(e.target.files[0]);
  e.target.value='';
});

function addFiles(newFiles, relPaths){
  // Switching from zipper mode back to regular images
  if(zipperMode) clearZipper();
  relPaths = relPaths||[];
  while(relPaths.length < newFiles.length) relPaths.push('');
  const start = files.length;
  files = [...files, ...newFiles];
  fileRelPaths = [...fileRelPaths, ...relPaths];
  renderQueue();
  convertBtn.disabled = files.length===0;
  hideErr();
  if(files.length>0 && start===0) loadCropEditor(0);
  updateBatchCropUI();
}

function renderQueue(){
  fileQueue.innerHTML = files.map((f,i)=>`
    <div class="file-item">
      <span class="fname" title="${fileRelPaths[i]||f.name}">${fileRelPaths[i]||f.name}</span>
      <span class="fsize">${formatBytes(f.size)}</span>
    </div>`).join('');
  document.getElementById('file-queue-head').style.display = files.length ? 'flex' : 'none';
}

// Resets the regular-image queue — the same cleanup a successful Convert
// used to do automatically. Now that Convert leaves the queue and crop
// editor in place (so a crop can be tweaked and re-converted without
// re-uploading), this is how you explicitly start a fresh batch instead.
function clearFiles(){
  files=[]; fileRelPaths=[]; cropData={}; renderQueue(); fileInput.value='';
  lockCrop=false; document.getElementById('lock-crop').checked=false;
  convertBtn.disabled=true;
  cropPanel.style.display='none'; updateBatchCropUI();
}
document.getElementById('btn-clear-files').addEventListener('click', clearFiles);

// ═══════════════════════════════════════════════════════════════
//  Zipper import
// ═══════════════════════════════════════════════════════════════
async function handleZipperDrop(file){
  clearFiles();   // clear any regular image queue

  zipperPanel.style.display='block';
  document.getElementById('zipper-grid').innerHTML =
    '<div class="zipper-loading"><span>📦</span> Extracting…</div>';
  document.getElementById('zipper-missing').style.display='none';
  document.getElementById('zipper-status').textContent='';

  try {
    const fd = new FormData();
    fd.append('file', file);
    const res = await fetch('/extract_zipper', {method:'POST', body:fd});
    const data = await res.json();
    if(data.error) throw new Error(data.error);

    zipperMode = true;
    zipperSessionId = data.session_id;
    zipperAssets = data.assets;
    zipperCropOverrides = {};
    zipperCurrentIdx = -1;

    document.getElementById('zipper-theme-name').value = data.theme_name || '';

    if(data.missing && data.missing.length){
      document.getElementById('zipper-missing').style.display='block';
      document.getElementById('zipper-missing-list').textContent = data.missing.join(', ');
    }

    renderZipperGrid();

    // Auto-open first asset in crop editor
    if(zipperAssets.length) loadZipperCropEditor(0);

  } catch(e){
    document.getElementById('zipper-grid').innerHTML='';
    showErr('Zipper extract failed: '+e.message);
  }
}

function renderZipperGrid(){
  const grid = document.getElementById('zipper-grid');
  grid.innerHTML = zipperAssets.map((a,i)=>`
    <div class="zipper-asset" data-idx="${i}" onclick="loadZipperCropEditor(${i})"
         title="${a.out_path}">
      <img src="data:image/jpeg;base64,${a.thumb_b64}" alt="${a.stem}">
      <div class="zipper-asset-label">${a.stem}</div>
      <div class="zipper-asset-path">${a.out_path}</div>
    </div>`).join('');
}

function loadZipperCropEditor(idx){
  if(idx<0||idx>=zipperAssets.length) return;
  zipperCurrentIdx = idx;
  const asset = zipperAssets[idx];

  cropPanel.style.display='block';
  imgNav.textContent = `${asset.stem} → ${asset.out_path}`;

  // Highlight active card
  document.querySelectorAll('.zipper-asset').forEach((el,i)=>
    el.classList.toggle('zactive', i===idx));

  cropImg.onload = ()=>{
    natW = cropImg.naturalWidth;
    natH = cropImg.naturalHeight;
    // Use user override if already edited, otherwise use server-suggested crop
    const crop = zipperCropOverrides[asset.stem] || asset.suggested_crop;
    setBoxFromNat(crop);
    drawBox();
    syncCoordInputs();
    updatePreview();
  };
  // Serve the full original image from staging
  cropImg.src = `/staging/${asset.staging_file}?t=${Date.now()}`;
}

function saveZipperCrop(){
  if(!zipperMode || zipperCurrentIdx<0 || !zipperAssets[zipperCurrentIdx]) return;
  const scaleX = natW / cropImg.clientWidth;
  const scaleY = natH / cropImg.clientHeight;
  const d = {
    x: Math.round(box.x*scaleX), y: Math.round(box.y*scaleY),
    w: Math.round(box.w*scaleX), h: Math.round(box.h*scaleY),
  };
  const stem = zipperAssets[zipperCurrentIdx].stem;
  zipperCropOverrides[stem] = d;
  // Mark card as having a custom (user-adjusted) crop
  const card = document.querySelector(`.zipper-asset[data-idx="${zipperCurrentIdx}"]`);
  if(card) card.classList.add('crop-edited');
  // Update coord display
  document.getElementById('cx-val').value = d.x;
  document.getElementById('cy-val').value = d.y;
  document.getElementById('cw-val').value = d.w;
  document.getElementById('ch-val').value = d.h;
}

async function convertZipper(){
  const themeName = document.getElementById('zipper-theme-name').value.trim() || 'MyTheme';
  const btn = document.getElementById('btn-convert-zipper');
  const status = document.getElementById('zipper-status');
  btn.disabled=true; btn.textContent='Converting…';
  status.textContent=''; status.style.color='var(--dim)';

  // Build crop list: user override takes priority over server suggestion
  const assets = zipperAssets.map(a=>({
    stem: a.stem,
    out_path: a.out_path,
    crop: zipperCropOverrides[a.stem] || a.suggested_crop,
  }));

  try {
    const res = await fetch('/convert_zipper', {
      method:'POST',
      headers:{'Content-Type':'application/json'},
      body: JSON.stringify({session_id:zipperSessionId, theme_name:themeName, assets}),
    });
    if(!res.ok){ const e=await res.json(); throw new Error(e.error||'Convert failed'); }
    const blob = await res.blob();
    const safe = themeName.replace(/[^a-zA-Z0-9_\- ]/g,'_');
    triggerDownload(blob, `${safe}_theme.zip`);
    status.textContent = `✓ ${zipperAssets.length} images packaged`;
    status.style.color = 'var(--success)';
  } catch(e){
    status.textContent = `Error: ${e.message}`;
    status.style.color = '#ff4444';
  }
  btn.disabled=false;
  btn.textContent='↓ Convert & Download Theme ZIP';
}

function clearZipper(){
  zipperMode=false; zipperSessionId=null;
  zipperAssets=[]; zipperCropOverrides={}; zipperCurrentIdx=-1;
  zipperPanel.style.display='none';
  cropPanel.style.display='none';
}

document.getElementById('btn-clear-zipper').addEventListener('click',()=>{
  clearZipper();
  convertBtn.disabled = files.length===0;
});
document.getElementById('btn-convert-zipper').addEventListener('click', convertZipper);

// ═══════════════════════════════════════════════════════════════
//  Crop Editor — regular mode
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
    if(cropData[idx]) setBoxFromNat(cropData[idx]);
    else defaultBox();
    drawBox();
    syncCoordInputs();
    updatePreview();
  };
  cropImg.src = url;
}

// Refreshes the X/Y/W/H fields from the current `box` (screen px → natural
// image px) without touching cropData/zipperCropOverrides — unlike saveNat(),
// which also persists the crop and marks a zipper asset "crop-edited". Used
// wherever the box changes for a reason other than the user editing it
// (loading a new image, switching between images).
function syncCoordInputs(){
  const scaleX = natW / cropImg.clientWidth;
  const scaleY = natH / cropImg.clientHeight;
  document.getElementById('cx-val').value = Math.round(box.x*scaleX);
  document.getElementById('cy-val').value = Math.round(box.y*scaleY);
  document.getElementById('cw-val').value = Math.round(box.w*scaleX);
  document.getElementById('ch-val').value = Math.round(box.h*scaleY);
}

function defaultBox(){
  const sw = cropImg.clientWidth;
  const sh = cropImg.clientHeight;
  const ratio = getOutW() / getOutH();
  let bw, bh;
  if(sw/sh > ratio){ bh=sh; bw=Math.round(bh*ratio); }
  else             { bw=sw; bh=Math.round(bw/ratio); }
  bw = Math.min(bw,sw); bh = Math.min(bh,sh);
  box = {x:Math.round((sw-bw)/2), y:Math.round((sh-bh)/2), w:bw, h:bh};
}

function setBoxFromNat(nat){
  const scaleX = cropImg.clientWidth  / natW;
  const scaleY = cropImg.clientHeight / natH;
  box = {
    x: Math.round(nat.x*scaleX), y: Math.round(nat.y*scaleY),
    w: Math.round(nat.w*scaleX), h: Math.round(nat.h*scaleY),
  };
}

// Stores crop `d` (original image pixels) for whichever image is active and
// refreshes the X/Y/W/H fields to match. Shared by saveNat() (derives d from
// the dragged/resized on-screen box) and applyManualCoords() (d is already
// exact, typed by the user) so both end up in cropData/zipperCropOverrides
// the same way — including "Lock to all" propagation.
function persistCrop(d){
  if(zipperMode && zipperCurrentIdx>=0 && zipperAssets[zipperCurrentIdx]){
    const stem = zipperAssets[zipperCurrentIdx].stem;
    zipperCropOverrides[stem] = d;
    const card = document.querySelector(`.zipper-asset[data-idx="${zipperCurrentIdx}"]`);
    if(card) card.classList.add('crop-edited');
  } else {
    cropData[currentIdx] = d;
    if(lockCrop){
      for(let i=0;i<files.length;i++) if(i!==currentIdx) cropData[i]={...d};
    }
  }

  document.getElementById('cx-val').value = d.x;
  document.getElementById('cy-val').value = d.y;
  document.getElementById('cw-val').value = d.w;
  document.getElementById('ch-val').value = d.h;
}

function saveNat(){
  const scaleX = natW / cropImg.clientWidth;
  const scaleY = natH / cropImg.clientHeight;
  persistCrop({
    x: Math.round(box.x*scaleX), y: Math.round(box.y*scaleY),
    w: Math.round(box.w*scaleX), h: Math.round(box.h*scaleY),
  });
}

function drawBox(){
  cropBox.style.left  = box.x+'px';
  cropBox.style.top   = box.y+'px';
  cropBox.style.width = box.w+'px';
  cropBox.style.height= box.h+'px';
}

function clampBox(){
  const sw = cropImg.clientWidth, sh = cropImg.clientHeight;
  box.w = Math.max(10, Math.min(box.w, sw));
  box.h = Math.max(10, Math.min(box.h, sh));
  box.x = Math.max(0,  Math.min(box.x, sw-box.w));
  box.y = Math.max(0,  Math.min(box.y, sh-box.h));
}

// Manual X/Y/W/H entry — typed values are in ORIGINAL image pixels (same
// units the fields display), so a crop box can be re-entered exactly,
// giving pixel-identical crops for a fresh image or a later run instead of
// eyeballing a dragged box each time. Stored via persistCrop() with the
// typed numbers themselves — NOT saveNat()'s box→nat round-trip, which
// re-derives nat pixels from the on-screen box and can be off by a pixel
// after the scale-factor rounding, defeating the point of typing exact
// values. The on-screen box is still updated (setBoxFromNat) so the crop
// rectangle and preview reflect what was typed.
function applyManualCoords(){
  if(!cropImg.naturalWidth || getMode()!=='manual') return;
  const raw = id => Math.round(parseFloat(document.getElementById(id).value));
  let x = raw('cx-val'), y = raw('cy-val'), w = raw('cw-val'), h = raw('ch-val');
  if(!Number.isFinite(x)) x = 0;
  if(!Number.isFinite(y)) y = 0;
  if(!Number.isFinite(w) || w < 1) w = 1;
  if(!Number.isFinite(h) || h < 1) h = 1;
  w = Math.min(w, natW); h = Math.min(h, natH);
  x = Math.max(0, Math.min(x, natW - w));
  y = Math.max(0, Math.min(y, natH - h));
  setBoxFromNat({x, y, w, h});
  clampBox(); drawBox(); updatePreview();
  persistCrop({x, y, w, h});
}

function lockAspect(edge, dx, dy){
  const ratio = getOutW() / getOutH();
  let {x,y,w,h} = drag.startBox;
  if(edge==='n'){ h=h-dy; y=y+dy; w=Math.round(h*ratio); x=drag.startBox.x+Math.round((drag.startBox.w-w)/2); }
  else if(edge==='s'){ h=h+dy; w=Math.round(h*ratio); x=drag.startBox.x+Math.round((drag.startBox.w-w)/2); }
  else if(edge==='e'){ w=w+dx; h=Math.round(w/ratio); y=drag.startBox.y+Math.round((drag.startBox.h-h)/2); }
  else if(edge==='w'){ w=w-dx; x=x+dx; h=Math.round(w/ratio); y=drag.startBox.y+Math.round((drag.startBox.h-h)/2); }
  if(w<20){ w=20; h=Math.round(w/ratio); }
  return {x,y,w,h};
}

// ── Pointer events ────────────────────────────────────────────
cropBox.addEventListener('pointerdown', e=>{
  if(getMode()!=='manual') return;
  const dir = e.target.dataset.dir;
  e.preventDefault(); e.stopPropagation();
  drag = {type:dir||'move', startX:e.clientX, startY:e.clientY, startBox:{...box}};
  cropBox.setPointerCapture(e.pointerId);
});
cropStage.addEventListener('pointerdown', e=>{
  if(getMode()!=='manual') return;
  if(e.target===cropBox||cropBox.contains(e.target)) return;
  e.preventDefault();
  const rect = cropStage.getBoundingClientRect();
  const sx=e.clientX-rect.left, sy=e.clientY-rect.top;
  drag = {type:'draw', startX:sx, startY:sy, startBox:{x:sx,y:sy,w:0,h:0}};
  box = {x:sx,y:sy,w:0,h:0};
  drawBox();
});
window.addEventListener('pointermove', e=>{
  if(!drag) return;
  const dx=e.clientX-drag.startX, dy=e.clientY-drag.startY;
  if(drag.type==='move'){ box.x=drag.startBox.x+dx; box.y=drag.startBox.y+dy; }
  else if(drag.type==='draw'){
    const rect=cropStage.getBoundingClientRect();
    const cx=e.clientX-rect.left, cy=e.clientY-rect.top;
    const ratio=getOutW()/getOutH();
    let w=cx-drag.startX, h=cy-drag.startY;
    if(Math.abs(w)>Math.abs(h)) h=w/ratio; else w=h*ratio;
    box.x=w<0?drag.startX+w:drag.startX; box.y=h<0?drag.startY+h:drag.startY;
    box.w=Math.abs(w); box.h=Math.abs(h);
  } else { box=lockAspect(drag.type,dx,dy); }
  clampBox(); drawBox(); saveNat(); updatePreview();
});
window.addEventListener('pointerup', ()=>{ drag=null; });

window.addEventListener('keydown', e=>{
  if(getMode()!=='manual') return;
  if(cropPanel.style.display==='none') return;
  const tag=document.activeElement&&document.activeElement.tagName;
  if(tag==='INPUT'||tag==='SELECT'||tag==='TEXTAREA') return;
  const step=e.shiftKey?10:1;
  let moved=false;
  if(e.key==='ArrowLeft'){box.x-=step;moved=true;}
  if(e.key==='ArrowRight'){box.x+=step;moved=true;}
  if(e.key==='ArrowUp'){box.y-=step;moved=true;}
  if(e.key==='ArrowDown'){box.y+=step;moved=true;}
  if(moved){ e.preventDefault(); clampBox(); drawBox(); saveNat(); updatePreview(); }
});

window.addEventListener('resize', ()=>{
  if(!cropImg.naturalWidth) return;
  requestAnimationFrame(()=>{
    if(zipperMode && zipperCurrentIdx>=0){
      const crop = zipperCropOverrides[zipperAssets[zipperCurrentIdx].stem] ||
                   zipperAssets[zipperCurrentIdx].suggested_crop;
      setBoxFromNat(crop);
    } else if(cropData[currentIdx]){
      setBoxFromNat(cropData[currentIdx]);
    } else {
      defaultBox();
    }
    drawBox();
  });
});

// Manual X/Y/W/H entry — Apply button, or Enter inside any of the 4 fields.
document.getElementById('btn-apply-coords').addEventListener('click', applyManualCoords);
['cx-val','cy-val','cw-val','ch-val'].forEach(id=>{
  document.getElementById(id).addEventListener('keydown', e=>{
    if(e.key==='Enter'){ e.preventDefault(); applyManualCoords(); }
  });
});

// Nav buttons
document.getElementById('btn-prev').addEventListener('click',()=>{
  if(zipperMode){ if(zipperCurrentIdx>0) loadZipperCropEditor(zipperCurrentIdx-1); }
  else          { if(currentIdx>0) loadCropEditor(currentIdx-1); }
});
document.getElementById('btn-next').addEventListener('click',()=>{
  if(zipperMode){ if(zipperCurrentIdx<zipperAssets.length-1) loadZipperCropEditor(zipperCurrentIdx+1); }
  else          { if(currentIdx<files.length-1) loadCropEditor(currentIdx+1); }
});
document.getElementById('btn-reset-crop').addEventListener('click',()=>{
  if(zipperMode && zipperCurrentIdx>=0){
    const stem = zipperAssets[zipperCurrentIdx].stem;
    delete zipperCropOverrides[stem];
    const card = document.querySelector(`.zipper-asset[data-idx="${zipperCurrentIdx}"]`);
    if(card) card.classList.remove('crop-edited');
    setBoxFromNat(zipperAssets[zipperCurrentIdx].suggested_crop);
  } else {
    delete cropData[currentIdx];
    defaultBox();
  }
  drawBox(); saveNat(); updatePreview();
});

// Apply to All (regular mode only)
document.getElementById('btn-apply-all').addEventListener('click',()=>{
  if(!cropData[currentIdx]) saveNat();
  const src=cropData[currentIdx];
  if(!src) return;
  for(let i=0;i<files.length;i++) cropData[i]={...src};
  const btn=document.getElementById('btn-apply-all');
  const orig=btn.textContent;
  btn.textContent='✓ Applied!'; btn.style.color='var(--success)'; btn.style.borderColor='var(--success)';
  setTimeout(()=>{ btn.textContent=orig; btn.style.color=''; btn.style.borderColor=''; },1500);
});

document.getElementById('lock-crop').addEventListener('change', e=>{
  lockCrop=e.target.checked;
  if(lockCrop&&files.length>0){
    if(!cropData[currentIdx]) saveNat();
    const src=cropData[currentIdx];
    for(let i=0;i<files.length;i++) cropData[i]={...src};
  }
});

function updateBatchCropUI(){
  const show=files.length>1&&getMode()==='manual';
  document.getElementById('btn-apply-all').style.display    = show?'inline-flex':'none';
  document.getElementById('lock-crop-label').style.display  = show?'flex':'none';
  if(!show){ lockCrop=false; document.getElementById('lock-crop').checked=false; }
}

document.getElementById('width').addEventListener('change',  ()=>{ if(files.length){ delete cropData[currentIdx]; defaultBox(); drawBox(); saveNat(); updatePreview(); }});
document.getElementById('height').addEventListener('change', ()=>{ if(files.length){ delete cropData[currentIdx]; defaultBox(); drawBox(); saveNat(); updatePreview(); }});

document.querySelectorAll('input[name=cropmode]').forEach(r=>r.addEventListener('change',()=>{
  const visible=getMode()==='manual';
  cropBox.style.pointerEvents=visible?'all':'none';
  cropBox.style.opacity=visible?'1':'0.3';
  updateBatchCropUI();
}));

// ── Preview canvas ────────────────────────────────────────────
function updatePreview(){
  if(!cropImg.naturalWidth) return;
  const tw=getOutW(), th=getOutH();
  previewCanvas.width=tw; previewCanvas.height=th;
  const maxW=80, maxH=160, s=Math.min(maxW/tw,maxH/th);
  previewCanvas.style.width=Math.round(tw*s)+'px';
  previewCanvas.style.height=Math.round(th*s)+'px';
  const mode=getMode();
  if(mode==='stretch'){
    pctx.drawImage(cropImg,0,0,tw,th);
  } else if(mode==='auto'){
    const ratio=tw/th, srcRatio=natW/natH;
    let sx=0,sy=0,sw=natW,sh=natH;
    if(srcRatio>ratio){ sw=Math.round(natH*ratio); sx=(natW-sw)/2; }
    else              { sh=Math.round(natW/ratio); sy=(natH-sh)/2; }
    pctx.drawImage(cropImg,sx,sy,sw,sh,0,0,tw,th);
  } else {
    const scaleX=natW/cropImg.clientWidth, scaleY=natH/cropImg.clientHeight;
    pctx.drawImage(cropImg,
      box.x*scaleX,box.y*scaleY,box.w*scaleX,box.h*scaleY,
      0,0,tw,th);
  }
}

// ═══════════════════════════════════════════════════════════════
//  Convert — regular images
// ═══════════════════════════════════════════════════════════════
convertBtn.addEventListener('click', async ()=>{
  if(!files.length) return;
  hideErr();
  results=[]; resultGrid.innerHTML=''; resultsPanel.style.display='none';
  progWrap.style.display='block'; convertBtn.disabled=true;
  const width=getOutW(), height=getOutH(), fmt=getFmt(), mode=getMode();

  for(let i=0;i<files.length;i++){
    const f=files[i];
    progBar.style.width=Math.round(i/files.length*100)+'%';
    progPct.textContent=Math.round(i/files.length*100)+'%';
    progText.textContent=`Processing ${i+1}/${files.length}: ${f.name}`;
    try {
      const fd=new FormData();
      fd.append('file',f);
      fd.append('width',width); fd.append('height',height);
      fd.append('format',fmt); fd.append('cropmode',mode);
      if(fileRelPaths[i]) fd.append('relative_path',fileRelPaths[i]);
      if(mode==='manual'&&cropData[i]){
        const d=cropData[i];
        fd.append('crop_x',d.x); fd.append('crop_y',d.y);
        fd.append('crop_w',d.w); fd.append('crop_h',d.h);
      }
      const res=await fetch('/convert',{method:'POST',body:fd});
      const data=await res.json();
      if(data.error) throw new Error(data.error);
      results.push(data); addCard(data);
    } catch(e){ showErr(`Error: ${f.name} — ${e.message}`); }
  }

  progBar.style.width='100%'; progPct.textContent='100%'; progText.textContent='Done!';
  if(results.length){
    statBadge.textContent=`${results.length} file${results.length>1?'s':''}`;
    resultsPanel.style.display='block';
  }
  convertBtn.disabled=false;
  // The queue, crop editor and cropData are deliberately left in place (not
  // wiped like they used to be) — Convert can be clicked again right away
  // after tweaking a crop, with no re-upload needed. Use "Clear" (see
  // clearFiles()) to start a fresh batch instead.
});

function addCard(r){
  const displayName=r.out_name.split('/').pop();
  const card=document.createElement('div');
  card.className='result-card';
  card.innerHTML=`
    <img src="data:image/jpeg;base64,${r.preview_b64}" alt="${displayName}">
    <div class="card-info">
      <div class="card-name" title="${r.out_name}">${displayName}</div>
      <div class="card-meta">${r.dims} · ${formatBytes(r.size)}</div>
      <button class="btn btn-sm" onclick="downloadOne('${r.out_name}')">↓ Save</button>
    </div>`;
  resultGrid.appendChild(card);
}

// ═══════════════════════════════════════════════════════════════
//  Downloads
// ═══════════════════════════════════════════════════════════════
async function downloadOne(name){
  const res=await fetch('/download/'+encodeURIComponent(name));
  triggerDownload(await res.blob(), name);
}
dlAllBtn.addEventListener('click', async ()=>{
  dlAllBtn.textContent='...';
  const res=await fetch('/download_zip',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({files:results.map(r=>r.out_name)})});
  triggerDownload(await res.blob(),'nextube_images.zip');
  dlAllBtn.textContent='↓ Zip All';
});
function triggerDownload(blob,name){
  const url=URL.createObjectURL(blob);
  const a=document.createElement('a');
  a.href=url; a.download=name;
  document.body.appendChild(a); a.click();
  document.body.removeChild(a); URL.revokeObjectURL(url);
}
window.downloadOne=downloadOne;

function showErr(msg){ errMsg.textContent=msg; errMsg.style.display='block'; }
function hideErr(){ errMsg.style.display='none'; }
</script>
</body>
</html>
"""

# ── HTTP Server ───────────────────────────────────────────────────────────────
class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args): pass

    # This server handles one request at a time (plain HTTPServer, not
    # threaded) and never explicitly closes a connection — but a browser's
    # fetch() always sends "HTTP/1.1" on its request line, and
    # BaseHTTPRequestHandler.parse_request() honors THAT (not this class's
    # own protocol_version) when deciding whether to keep the connection
    # open for a following request. That's normally fine, but leaves this
    # single-threaded server sitting in handle_one_request() waiting on the
    # kept-alive socket instead of back at accept() — reproducibly wedges
    # the entire server (even unrelated requests time out) the moment a
    # second request follows a first one in the same page session, e.g.
    # Convert clicked twice without a reload in between. Forcing
    # close_connection here makes every request get its own fresh
    # connection, matching how this tool was actually being used before
    # anything relied on issuing back-to-back requests without a reload.
    def handle_one_request(self):
        super().handle_one_request()
        self.close_connection = True

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
        elif path.startswith("/staging/"):
            self._serve_staging(path[len("/staging/"):])
        elif path.startswith("/download/"):
            fname = unquote(path[len("/download/"):])
            fpath = (OUTPUT_DIR / fname).resolve()
            if not str(fpath).startswith(str(OUTPUT_DIR.resolve())):
                self.send_response(403); self.end_headers(); return
            if not fpath.exists():
                self.send_response(404); self.end_headers(); return
            data = fpath.read_bytes()
            mt, _ = mimetypes.guess_type(str(fpath))
            self.send_response(200)
            self.send_header("Content-Type", mt or "application/octet-stream")
            self.send_header("Content-Disposition", f'attachment; filename="{Path(fname).name}"')
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        else:
            self.send_response(404); self.end_headers()

    def do_POST(self):
        path = urlparse(self.path).path
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length)
        if   path == "/convert":         self._handle_convert(body)
        elif path == "/extract_zipper":  self._handle_extract_zipper(body)
        elif path == "/convert_zipper":  self._handle_convert_zipper(body)
        elif path == "/download_zip":    self._handle_zip(body)
        else: self.send_response(404); self.end_headers()

    # ── Staging file server ───────────────────────────────────────────────────
    def _serve_staging(self, rel):
        rel = unquote(rel.split("?")[0])   # strip query string (cache-bust param)
        fpath = (STAGING_DIR / rel).resolve()
        if not str(fpath).startswith(str(STAGING_DIR.resolve())):
            self.send_response(403); self.end_headers(); return
        if not fpath.exists():
            self.send_response(404); self.end_headers(); return
        data = fpath.read_bytes()
        mt, _ = mimetypes.guess_type(str(fpath))
        self.send_response(200)
        self.send_header("Content-Type", mt or "image/png")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    # ── Multipart parser ──────────────────────────────────────────────────────
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

    # ── Extract .zipper ───────────────────────────────────────────────────────
    def _handle_extract_zipper(self, body):
        try:
            f = self._parse_multipart(body)
            zip_data = f["file"]["value"]
            filename = f["file"]["filename"] or "theme.zipper"
            # Use the filename stem exactly — spaces, dashes, and underscores are preserved
            # so "neon yellow.zipper" → theme name "neon yellow", not mangled.
            theme_name = Path(filename).stem

            session_id = str(uuid.uuid4())
            staging = STAGING_DIR / session_id
            staging.mkdir(parents=True, exist_ok=True)

            assets = []
            found_stems = set()

            with zipfile.ZipFile(io.BytesIO(zip_data)) as zf:
                for info in zf.infolist():
                    if info.is_dir():
                        continue
                    stem = Path(info.filename).stem.lower()
                    if stem not in ZIPPER_ROLE_MAP or stem in found_stems:
                        continue
                    raw = zf.read(info.filename)
                    try:
                        img = Image.open(io.BytesIO(raw))
                        img.load()
                    except Exception:
                        continue

                    # Save original PNG to staging for the crop editor to serve
                    staging_name = f"{stem}.png"
                    (staging / staging_name).write_bytes(raw)

                    w, h = img.size
                    # Scale the reference crop to match actual image dimensions
                    sx = w / ZIPPER_CROP_REF[0]
                    sy = h / ZIPPER_CROP_REF[1]
                    cx = round(ZIPPER_CROP_XYWH[0] * sx)
                    cy = round(ZIPPER_CROP_XYWH[1] * sy)
                    cw = round(ZIPPER_CROP_XYWH[2] * sx)
                    ch = round(ZIPPER_CROP_XYWH[3] * sy)
                    thumb = make_thumb(img, cx, cy, cw, ch)

                    assets.append({
                        "stem":          stem,
                        "out_path":      ZIPPER_ROLE_MAP[stem],
                        "staging_file":  f"{session_id}/{staging_name}",
                        "w": w, "h": h,
                        "suggested_crop": {"x": cx, "y": cy, "w": cw, "h": ch},
                        "thumb_b64":     thumb,
                    })
                    found_stems.add(stem)

            # Sort by canonical display order
            assets.sort(key=lambda a: ZIPPER_STEM_ORDER.index(a["stem"])
                                       if a["stem"] in ZIPPER_STEM_ORDER else 99)

            missing = [s for s in ZIPPER_STEM_ORDER if s not in found_stems]

            self.send_json({
                "session_id": session_id,
                "theme_name": theme_name,
                "assets":     assets,
                "missing":    missing,
            })
        except Exception as e:
            import traceback; traceback.print_exc()
            self.send_json({"error": str(e)}, 500)

    # ── Convert zipper assets → theme ZIP ────────────────────────────────────
    def _handle_convert_zipper(self, body):
        try:
            data = json.loads(body)
            session_id = data.get("session_id", "")
            theme_name = data.get("theme_name", "MyTheme").strip() or "MyTheme"
            asset_list = data.get("assets", [])

            # Validate session path
            staging = (STAGING_DIR / session_id).resolve()
            if not str(staging).startswith(str(STAGING_DIR.resolve())):
                raise ValueError("Invalid session")
            if not staging.exists():
                raise ValueError("Session not found — re-import the .zipper file")

            out_zip = io.BytesIO()
            with zipfile.ZipFile(out_zip, "w", zipfile.ZIP_DEFLATED) as zf:
                for asset in asset_list:
                    stem     = asset.get("stem", "")
                    out_path = asset.get("out_path") or ZIPPER_ROLE_MAP.get(stem)
                    crop     = asset.get("crop")
                    if not out_path:
                        continue

                    staging_file = staging / f"{stem}.png"
                    if not staging_file.exists():
                        continue

                    img = Image.open(staging_file)

                    if crop:
                        cx = max(0, min(int(crop["x"]), img.width  - 1))
                        cy = max(0, min(int(crop["y"]), img.height - 1))
                        cw = max(1, min(int(crop["w"]), img.width  - cx))
                        ch = max(1, min(int(crop["h"]), img.height - cy))
                        img = img.crop((cx, cy, cx + cw, cy + ch))
                    else:
                        # Fallback: auto centre-crop
                        cx, cy, cw, ch = suggest_crop(img.width, img.height)
                        img = img.crop((cx, cy, cx + cw, cy + ch))

                    img = img.resize((DEFAULT_W, DEFAULT_H), Image.LANCZOS)
                    buf = io.BytesIO()
                    img.convert("RGB").save(buf, "JPEG", quality=80, optimize=True)

                    zf.writestr(f"{theme_name}/{out_path}", buf.getvalue())

            # Clean up staging directory
            shutil.rmtree(staging, ignore_errors=True)

            out_zip.seek(0)
            content = out_zip.read()
            safe = "".join(c for c in theme_name if c.isalnum() or c in "_- ")
            self.send_response(200)
            self.send_header("Content-Type", "application/zip")
            self.send_header("Content-Disposition",
                             f'attachment; filename="{safe}_theme.zip"')
            self.send_header("Content-Length", str(len(content)))
            self.end_headers()
            self.wfile.write(content)
        except Exception as e:
            import traceback; traceback.print_exc()
            self.send_json({"error": str(e)}, 500)

    # ── Regular image convert ─────────────────────────────────────────────────
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

            if mode == "stretch":
                img = Image.open(io.BytesIO(image_data))
                img = img.resize((width, height), Image.LANCZOS)
                stem = Path(filename).stem
                if fmt == "jpeg":
                    buf = io.BytesIO(); img.convert("RGB").save(buf, "JPEG", quality=80)
                    out_bytes = buf.getvalue(); out_name = f"{stem}.jpg"
                else:
                    buf = io.BytesIO(); img.save(buf, "PNG", optimize=True)
                    out_bytes = buf.getvalue(); out_name = f"{stem}.png"
                prev = io.BytesIO(); img.convert("RGB").save(prev, "JPEG", quality=72)
                result = {"out_name":out_name, "out_bytes":out_bytes,
                          "preview_b64":base64.b64encode(prev.getvalue()).decode(),
                          "size":len(out_bytes), "dims":f"{width}x{height}", "format":fmt}
            else:
                result = process_image(image_data, filename, width, height, fmt, crop_box)

            rel = f.get("relative_path", {}).get("value", b"").decode().strip()
            if rel:
                rel = rel.replace("\\", "/").lstrip("/")
                ext = ".jpg" if fmt == "jpeg" else ".png"
                out_rel = str(Path(rel).with_suffix(ext)).replace("\\", "/")
                out_path = (OUTPUT_DIR / out_rel).resolve()
                if not str(out_path).startswith(str(OUTPUT_DIR.resolve())):
                    raise ValueError("Invalid path")
                out_path.parent.mkdir(parents=True, exist_ok=True)
                result["out_name"] = out_rel
                out_path.write_bytes(result["out_bytes"])
            else:
                (OUTPUT_DIR / result["out_name"]).write_bytes(result["out_bytes"])

            self.send_json({"out_name":result["out_name"], "preview_b64":result["preview_b64"],
                            "size":result["size"], "dims":result["dims"], "format":result["format"]})
        except Exception as e:
            self.send_json({"error": str(e)}, 500)

    # ── ZIP download for regular results ─────────────────────────────────────
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
    # Clean up any leftover staging sessions from a previous run
    if STAGING_DIR.exists():
        for d in STAGING_DIR.iterdir():
            if d.is_dir():
                shutil.rmtree(d, ignore_errors=True)

    port = 5000
    server = HTTPServer(("0.0.0.0", port), Handler)
    url = f"http://localhost:{port}"
    print(f"\n  Nextube Image Converter v3")
    print(f"  ────────────────────────")
    print(f"  Running at : {url}")
    print(f"  Output dir : {OUTPUT_DIR.resolve()}")
    print(f"  Press Ctrl+C to stop\n")
    threading.Timer(0.8, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n  Stopped.")
