import hashlib
import json
import os
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from io import BytesIO
from json import JSONDecodeError
from pathlib import Path
from typing import Any, NoReturn, cast
from urllib.parse import parse_qs, urlparse

from PIL import Image, ImageDraw, ImageFont

from monitor_service.activity_animation import (
    activity_animation_etag,
    pack_activity_animation,
    render_activity_preview_gif,
)
from monitor_service.claude_usage import CLAUDE_USAGE_URL
from monitor_service.collectors import count_busy_claude_sessions, count_busy_codex_sessions
from monitor_service.config import ACTIVITY_ANIMATION_STYLES, SCREENSAVERS, get_config_path, get_readonly_home, load_config, normalize_activity_animation_config, normalize_config, now_ms, save_config
from monitor_service.logutil import log_event
from monitor_service.renderer import BOOT_LOG_X, BOOT_MASCOT_CX, HEIGHT, THEME_DEFINITIONS, WIDTH, draw_boot_decor, pack_frame, render_tool_image, set_activity_box, set_device_ip
from monitor_service.screensavers import render_screensaver_preview_gif
from monitor_service.types import ActivityAnimationConfig, RuntimeStatus, ServiceConfig, ToolSnapshot
from monitor_service.usage import build_snapshot, compute_activity_states

# Factor de escala para el preview PNG en la web (el OLED real es 128x64).
PREVIEW_SCALE = 3
FALLBACK_FRAME_DETAIL = "Generando frames"

# Long-poll de actividad: el servidor retiene el GET del ESP hasta que cambia el
# estado (o hasta el tope), para que reaccione casi al instante sin sondear el frame.
ACTIVITY_LONGPOLL_HOLD_SECONDS = 20.0
ACTIVITY_LONGPOLL_STEP_SECONDS = 0.25

# Telemetria del ESP para la web: se mide la cadencia con la que el ESP sondea
# /api/esp/frames (su heartbeat) para estimar si esta conectado y cuando aplicara un
# cambio recien guardado. Huecos mayores a este tope se tratan como reconexion y no
# contaminan la cadencia estimada. Ventana minima para considerarlo "en linea".
ESP_MAX_REASONABLE_GAP_MS = 120000
ESP_ONLINE_FALLBACK_MS = 30000

_frame_cache_lock = threading.Lock()
_frame_payload: bytes | None = None
_frame_etag = ""
_frame_updated_at_ms = 0
_frame_refreshing = False
_frame_last_error = ""
# Estados de actividad cacheados junto al frame: el refresco en background ya
# construye el snapshot, asi que send_frames no debe reconstruirlo en el path
# critico (cada poll del ESP) y dispararle un timeout de lectura.
_frame_claude_activity = "idle"
_frame_codex_activity = "idle"
# Firma de las barras y momento del ultimo cambio, para el atenuado anti burn-in.
_bars_signature: str | None = None
_bars_changed_at_ms = 0

# Telemetria del ESP (protegida por su propio lock): heartbeat, cadencia estimada e IP.
_esp_lock = threading.Lock()
_esp_last_seen_ms = 0
_esp_last_frames_ms = 0
_esp_frames_interval_ms = 0
_esp_last_anim_fetch_ms = 0
_esp_ip = ""

APP_HTML = r"""<!doctype html>
<html lang="es">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Usage Monitor</title>
  <style>
    :root {
      --bg: #f5f6fb;
      --panel: #ffffff;
      --panel-2: #fbfcfe;
      --text: #0f172a;
      --muted: #64748b;
      --line: #e7e9f0;
      --soft: #f1f3f9;
      --accent: #4f46e5;
      --accent-2: #6366f1;
      --accent-soft: #eef2ff;
      --ok: #16a34a;
      --ok-soft: #ecfdf5;
      --warn: #b45309;
      --warn-soft: #fffbeb;
      --danger: #dc2626;
      --danger-soft: #fef2f2;
      --radius: 12px;
      --radius-sm: 8px;
      --shadow: 0 1px 2px rgba(15,23,42,.04), 0 10px 30px rgba(15,23,42,.06);
      --shadow-lg: 0 2px 6px rgba(15,23,42,.06), 0 18px 44px rgba(15,23,42,.10);
    }
    * { box-sizing: border-box; }
    body { margin: 0; color: var(--text); font-family: Inter, ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; background: var(--bg); background-image: radial-gradient(1200px 560px at 100% -10%, #e8ecff 0%, rgba(245,246,251,0) 46%), radial-gradient(900px 500px at -10% 0%, #eafff4 0%, rgba(245,246,251,0) 40%); background-attachment: fixed; }
    .icon { width: 18px; height: 18px; stroke: currentColor; stroke-width: 2; stroke-linecap: round; stroke-linejoin: round; fill: none; flex: 0 0 auto; }
    .app { min-height: 100vh; display: grid; grid-template-columns: 248px 1fr; }
    aside { border-right: 1px solid var(--line); background: rgba(255,255,255,.72); backdrop-filter: blur(8px); padding: 20px 14px; position: sticky; top: 0; align-self: start; height: 100vh; }
    .brand { display: flex; align-items: center; gap: 10px; font-weight: 750; font-size: 15px; margin: 0 6px 22px; letter-spacing: -.01em; }
    .brand .icon-chip { width: 32px; height: 32px; }
    nav { display: grid; gap: 4px; }
    nav button { display: flex; align-items: center; gap: 10px; border: 0; width: 100%; text-align: left; border-radius: 9px; background: transparent; color: var(--muted); padding: 10px 11px; font: inherit; font-weight: 600; cursor: pointer; transition: background .15s ease, color .15s ease; }
    nav button .icon { width: 17px; height: 17px; }
    nav button:hover { background: var(--soft); color: var(--text); }
    nav button.active { background: var(--accent-soft); color: var(--accent); font-weight: 700; }
    main { padding: 26px 28px; max-width: 1200px; width: 100%; }
    header { display: flex; align-items: flex-start; justify-content: space-between; gap: 16px; margin-bottom: 22px; }
    h1 { margin: 0; font-size: 25px; line-height: 1.15; letter-spacing: -.02em; }
    h2 { margin: 0; font-size: 15px; letter-spacing: -.01em; }
    p { margin: 6px 0; color: var(--muted); line-height: 1.45; }
    .muted { color: var(--muted); }
    .grid { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 14px; }
    .panel { background: var(--panel); border: 1px solid var(--line); border-radius: var(--radius); padding: 18px; box-shadow: var(--shadow); transition: transform .16s ease, box-shadow .16s ease; }
    .panel + .panel, .grid + .panel { margin-top: 14px; }
    .panel:hover { box-shadow: var(--shadow-lg); }
    .panel-head { display: flex; align-items: center; justify-content: space-between; gap: 12px; margin-bottom: 16px; }
    .section-title { display: flex; align-items: center; gap: 12px; min-width: 0; }
    .section-title h2 { line-height: 1.2; }
    .section-sub { margin: 2px 0 0; font-size: 12px; color: var(--muted); line-height: 1.35; }
    .icon-chip { width: 36px; height: 36px; border-radius: 10px; display: inline-flex; align-items: center; justify-content: center; background: var(--accent-soft); color: var(--accent); flex: 0 0 auto; }
    .icon-chip .icon { width: 19px; height: 19px; }
    .icon-chip.amber { background: #fff4e5; color: #c2540a; }
    .icon-chip.slate { background: #eef1f6; color: #475569; }
    .icon-chip.green { background: var(--ok-soft); color: var(--ok); }
    .metric { display: flex; align-items: baseline; gap: 8px; margin-top: 6px; }
    .metric strong { font-size: 32px; line-height: 1; letter-spacing: -.02em; }
    .metric .muted { font-size: 12px; font-weight: 600; }
    .pill.has-icon::before { content: none; }
    .tool-head { display: flex; align-items: flex-start; gap: 12px; margin-bottom: 2px; }
    .tool-id { flex: 1 1 auto; min-width: 0; }
    .tool-id h2 { font-size: 16px; }
    .tool-id p { margin: 3px 0 0; font-size: 12.5px; }
    .window-block { margin-top: 14px; padding-top: 14px; border-top: 1px solid var(--line); }
    .window-head { display: flex; align-items: center; justify-content: space-between; gap: 10px; }
    .window-label { font-size: 11px; font-weight: 700; letter-spacing: .06em; text-transform: uppercase; color: var(--muted); }
    .window-meta { display: flex; flex-wrap: wrap; gap: 14px; font-size: 12px; color: var(--muted); margin-top: 2px; }
    .window-meta span { display: inline-flex; align-items: center; gap: 6px; }
    .window-meta .icon { width: 14px; height: 14px; }
    .pill { display: inline-flex; align-items: center; gap: 6px; min-height: 26px; padding: 0 11px; border-radius: 999px; border: 1px solid var(--line); color: var(--muted); font-size: 12px; font-weight: 650; background: var(--panel); }
    .pill .icon { width: 13px; height: 13px; }
    .pill.ok { color: var(--ok); border-color: #bbf7d0; background: var(--ok-soft); }
    .pill.warn { color: var(--warn); border-color: #fde68a; background: var(--warn-soft); }
    .pill.danger { color: var(--danger); border-color: #fecaca; background: var(--danger-soft); }
    .pill.pending { color: var(--warn); border-color: #fde68a; background: var(--warn-soft); }
    .pill.ok::before, .pill.warn::before, .pill.danger::before, .pill.pending::before { content: ""; width: 7px; height: 7px; border-radius: 999px; background: currentColor; flex: 0 0 auto; }
    .bar { height: 9px; border-radius: 999px; background: var(--soft); overflow: hidden; margin: 12px 0 10px; }
    .bar span { display: block; height: 100%; width: 0%; border-radius: 999px; background: linear-gradient(90deg, var(--accent), var(--accent-2)); transition: width .45s cubic-bezier(.4,0,.2,1); }
    .subgrid { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 10px; margin-top: 14px; }
    .kv { display: flex; align-items: center; gap: 10px; padding: 11px; border: 1px solid var(--line); border-radius: var(--radius-sm); background: var(--panel-2); }
    .kv .icon-chip { width: 30px; height: 30px; border-radius: 8px; }
    .kv .icon-chip .icon { width: 16px; height: 16px; }
    .kv-body { min-width: 0; }
    .kv-body span { display: block; color: var(--muted); font-size: 11px; margin-bottom: 2px; text-transform: uppercase; letter-spacing: .04em; }
    .kv-body strong { font-size: 14px; word-break: break-word; }
    form { display: grid; gap: 16px; }
    label { display: grid; gap: 6px; color: var(--muted); font-size: 12.5px; font-weight: 600; }
    input, select { width: 100%; min-height: 40px; border: 1px solid var(--line); border-radius: var(--radius-sm); background: var(--panel); color: var(--text); padding: 9px 11px; font: inherit; transition: border-color .15s ease, box-shadow .15s ease; }
    input:focus, select:focus { outline: none; border-color: var(--accent); box-shadow: 0 0 0 3px var(--accent-soft); }
    button.primary { display: inline-flex; align-items: center; justify-content: center; gap: 8px; min-height: 42px; border: 0; border-radius: var(--radius-sm); background: var(--accent); color: #fff; font-weight: 700; cursor: pointer; padding: 0 16px; box-shadow: 0 6px 16px rgba(79,70,229,.25); transition: filter .15s ease, transform .1s ease; }
    button.primary:hover { filter: brightness(1.07); }
    button.primary:active { transform: translateY(1px); }
    button.primary .icon { width: 16px; height: 16px; }
    .form-grid { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 14px; }
    .page { display: none; animation: fade-in .25s ease; }
    .page.active { display: block; }
    pre { margin: 0; overflow: auto; background: #0b1020; color: #d7def0; padding: 14px; border-radius: var(--radius-sm); font-size: 12px; line-height: 1.5; }
    .preview-grid { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 16px; }
    .preview-tile { border: 1px solid var(--line); border-radius: var(--radius-sm); padding: 12px; background: var(--panel-2); }
    .preview-tile .preview-name { display: flex; align-items: center; gap: 7px; margin-bottom: 8px; font-size: 12px; font-weight: 650; color: var(--muted); }
    .oled-preview { width: 100%; max-width: 384px; aspect-ratio: 2 / 1; image-rendering: pixelated; background: #000; border: 1px solid var(--line); border-radius: 8px; display: block; opacity: 1; box-shadow: inset 0 0 26px rgba(90,160,255,.10); }
    .oled-preview.loaded { opacity: 1; }
    @keyframes skeleton { 0% { background-position: 180% 0; } 100% { background-position: -80% 0; } }
    .oled-preview:not(.loaded),
    .theme-card img:not(.loaded),
    .anim-gallery .anim-card img:not(.loaded) {
      background-color: var(--soft);
      background-image: linear-gradient(100deg, rgba(255,255,255,0) 30%, rgba(255,255,255,.7) 50%, rgba(255,255,255,0) 70%);
      background-size: 220% 100%;
      background-repeat: no-repeat;
      box-shadow: none;
      animation: skeleton 1.25s ease-in-out infinite;
    }
    .live-dot { width: 9px; height: 9px; border-radius: 999px; background: var(--ok); animation: pulse 1.8s infinite; }
    @keyframes pulse { 0% { box-shadow: 0 0 0 0 rgba(22,163,74,.45); } 70% { box-shadow: 0 0 0 8px rgba(22,163,74,0); } 100% { box-shadow: 0 0 0 0 rgba(22,163,74,0); } }
    .theme-gallery { display: grid; grid-template-columns: repeat(auto-fill, minmax(190px, 1fr)); gap: 14px; }
    .anim-gallery { display: grid; grid-template-columns: repeat(auto-fill, minmax(150px, 1fr)); gap: 14px; }
    /* El ratio del recuadro es dinamico (lo fija updateAnimPreview segun ancho/alto)
       para que el preview no se estire. Selector mas especifico que .theme-card img. */
    .anim-gallery .anim-card img { aspect-ratio: 48 / 5; object-fit: contain; }
    .theme-card { text-align: left; cursor: pointer; padding: 10px; border: 1px solid var(--line); border-radius: 11px; background: var(--panel); display: grid; gap: 8px; font: inherit; color: inherit; transition: border-color .15s ease, box-shadow .15s ease, transform .15s ease; animation: fade-in .25s ease; }
    .theme-card:hover { transform: translateY(-2px); box-shadow: var(--shadow-lg); }
    .theme-card.active { border-color: var(--accent); box-shadow: 0 0 0 2px var(--accent) inset; }
    .theme-card img { width: 100%; aspect-ratio: 2 / 1; image-rendering: pixelated; background: #000; border-radius: 7px; display: block; opacity: 1; }
    .theme-card img.loaded { opacity: 1; }
    .theme-card-label { display: flex; align-items: center; justify-content: space-between; font-weight: 650; font-size: 13px; }
    .theme-card-badge { display: inline-flex; align-items: center; gap: 4px; font-size: 11px; color: var(--ok); font-weight: 700; opacity: 0; transition: opacity .15s ease; }
    .theme-card-badge .icon { width: 12px; height: 12px; }
    .theme-card.active .theme-card-badge { opacity: 1; }
    .bar span { transition: width .45s cubic-bezier(.4,0,.2,1); }
    @keyframes fade-in { from { opacity: 0; transform: translateY(4px); } to { opacity: 1; transform: none; } }
    .toast { position: fixed; bottom: 18px; right: 18px; max-width: 360px; display: flex; gap: 10px; align-items: flex-start; background: var(--panel); color: var(--text); padding: 12px 14px; border-radius: 10px; border: 1px solid var(--line); border-left: 4px solid var(--muted); box-shadow: 0 14px 36px rgba(0,0,0,.18); opacity: 0; transform: translateY(10px); transition: opacity .2s ease, transform .2s ease; pointer-events: none; z-index: 50; }
    .toast.show { opacity: 1; transform: none; }
    .toast .toast-icon { width: 16px; height: 16px; margin-top: 1px; border-radius: 999px; flex: 0 0 auto; background: var(--muted); }
    .toast .toast-title { font-weight: 700; font-size: 13px; }
    .toast .toast-detail { color: var(--muted); font-size: 12px; margin-top: 2px; line-height: 1.35; }
    .toast.success { border-left-color: var(--ok); }
    .toast.success .toast-icon { background: var(--ok); }
    .toast.error { border-left-color: var(--danger); }
    .toast.error .toast-icon { background: var(--danger); }
    .toast.info { border-left-color: #2563eb; }
    .toast.info .toast-icon { background: #2563eb; }
    .toast.loading { border-left-color: #2563eb; }
    .toast.loading .toast-icon { background: transparent; border: 2px solid #cbd5e1; border-top-color: #2563eb; animation: spin .7s linear infinite; }
    @keyframes spin { to { transform: rotate(360deg); } }
    .pills { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; justify-content: flex-end; }
    .status-grid { display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 10px; }
    .svg-sprite { position: absolute; width: 0; height: 0; overflow: hidden; }
    @media (max-width: 900px) { .status-grid { grid-template-columns: repeat(2, 1fr); } }
    @media (max-width: 760px) {
      .app { grid-template-columns: 1fr; }
      aside { position: static; height: auto; border-right: 0; border-bottom: 1px solid var(--line); }
      nav { grid-template-columns: repeat(3, 1fr); }
      main { padding: 16px; }
      .grid, .form-grid, .subgrid, .preview-grid, .status-grid { grid-template-columns: 1fr; }
      header { display: block; }
      .pills { justify-content: flex-start; margin-top: 10px; }
    }
  </style>
</head>
<body>
  <svg class="svg-sprite" aria-hidden="true" xmlns="http://www.w3.org/2000/svg">
    <symbol id="i-home" viewBox="0 0 24 24"><path d="M3 11l9-8 9 8"/><path d="M5 10v10a1 1 0 0 0 1 1h12a1 1 0 0 0 1-1V10"/></symbol>
    <symbol id="i-gauge" viewBox="0 0 24 24"><path d="M4 19a8 8 0 1 1 16 0"/><path d="M12 19l4-6"/></symbol>
    <symbol id="i-sliders" viewBox="0 0 24 24"><path d="M4 21v-7M4 10V3M12 21v-9M12 8V3M20 21v-5M20 12V3M1 14h6M9 8h6M17 16h6"/></symbol>
    <symbol id="i-cpu" viewBox="0 0 24 24"><rect x="6" y="6" width="12" height="12" rx="1"/><rect x="9" y="9" width="6" height="6"/><path d="M9 2v2M15 2v2M9 20v2M15 20v2M2 9h2M2 15h2M20 9h2M20 15h2"/></symbol>
    <symbol id="i-clock" viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/></symbol>
    <symbol id="i-repeat" viewBox="0 0 24 24"><path d="M17 2l4 4-4 4"/><path d="M3 11V9a4 4 0 0 1 4-4h14"/><path d="M7 22l-4-4 4-4"/><path d="M21 13v2a4 4 0 0 1-4 4H3"/></symbol>
    <symbol id="i-zap" viewBox="0 0 24 24"><path d="M13 2L3 14h7l-1 8 10-12h-7z"/></symbol>
    <symbol id="i-globe" viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><path d="M3 12h18"/><path d="M12 3a15 15 0 0 1 0 18a15 15 0 0 1 0-18z"/></symbol>
    <symbol id="i-sparkles" viewBox="0 0 24 24"><path d="M12 3l1.6 4.9L18.5 9.5l-4.9 1.6L12 16l-1.6-4.9L5.5 9.5l4.9-1.6z"/><path d="M19 14l.7 2.3L22 17l-2.3.7L19 20l-.7-2.3L16 17l2.3-.7z"/></symbol>
    <symbol id="i-terminal" viewBox="0 0 24 24"><rect x="2" y="4" width="20" height="16" rx="2"/><path d="M7 9l3 3-3 3M13 15h4"/></symbol>
    <symbol id="i-activity" viewBox="0 0 24 24"><path d="M22 12h-4l-3 9L9 3l-3 9H2"/></symbol>
    <symbol id="i-bell" viewBox="0 0 24 24"><path d="M18 8a6 6 0 0 0-12 0c0 7-3 9-3 9h18s-3-2-3-9"/><path d="M13.7 21a2 2 0 0 1-3.4 0"/></symbol>
    <symbol id="i-moon" viewBox="0 0 24 24"><path d="M21 12.8A9 9 0 1 1 11.2 3 7 7 0 0 0 21 12.8z"/></symbol>
    <symbol id="i-check" viewBox="0 0 24 24"><path d="M20 6L9 17l-5-5"/></symbol>
    <symbol id="i-palette" viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><circle cx="8" cy="10" r="1.1"/><circle cx="12" cy="8" r="1.1"/><circle cx="16" cy="10" r="1.1"/></symbol>
    <symbol id="i-film" viewBox="0 0 24 24"><rect x="3" y="3" width="18" height="18" rx="2"/><path d="M7 3v18M17 3v18M3 8h4M3 16h4M17 8h4M17 16h4"/></symbol>
    <symbol id="i-shield" viewBox="0 0 24 24"><path d="M12 2l8 4v6c0 5-3.5 8-8 10-4.5-2-8-5-8-10V6z"/></symbol>
    <symbol id="i-folder" viewBox="0 0 24 24"><path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/></symbol>
    <symbol id="i-code" viewBox="0 0 24 24"><path d="M16 18l6-6-6-6M8 6l-6 6 6 6"/></symbol>
    <symbol id="i-monitor" viewBox="0 0 24 24"><rect x="2" y="3" width="20" height="14" rx="2"/><path d="M8 21h8M12 17v4"/></symbol>
    <symbol id="i-database" viewBox="0 0 24 24"><ellipse cx="12" cy="5" rx="8" ry="3"/><path d="M4 5v6c0 1.7 3.6 3 8 3s8-1.3 8-3V5"/><path d="M4 11v6c0 1.7 3.6 3 8 3s8-1.3 8-3v-6"/></symbol>
    <symbol id="i-hash" viewBox="0 0 24 24"><path d="M4 9h16M4 15h16M10 3L8 21M16 3l-2 18"/></symbol>
    <symbol id="i-message" viewBox="0 0 24 24"><path d="M21 12a8 8 0 0 1-11.5 7.2L4 21l1.8-5.5A8 8 0 1 1 21 12z"/></symbol>
    <symbol id="i-target" viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><circle cx="12" cy="12" r="5"/><circle cx="12" cy="12" r="1.5"/></symbol>
    <symbol id="i-save" viewBox="0 0 24 24"><path d="M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2z"/><path d="M17 21v-8H7v8M7 3v5h8"/></symbol>
  </svg>
  <div id="toast" class="toast">
    <div class="toast-icon"></div>
    <div>
      <div id="toast-title" class="toast-title"></div>
      <div id="toast-detail" class="toast-detail"></div>
    </div>
  </div>
  <div class="app">
    <aside>
      <div class="brand">
        <span class="icon-chip"><svg class="icon"><use href="#i-cpu"></use></svg></span>
        Usage Monitor
      </div>
      <nav>
        <button class="active" data-page="home"><svg class="icon"><use href="#i-home"></use></svg>Home</button>
        <button data-page="usage"><svg class="icon"><use href="#i-gauge"></use></svg>Uso</button>
        <button data-page="config"><svg class="icon"><use href="#i-sliders"></use></svg>Configuracion</button>
      </nav>
    </aside>
    <main>
      <header>
        <div>
          <h1>Claude y Codex</h1>
          <p>Servicio local para alimentar el monitor ESP32.</p>
        </div>
        <div class="pills">
          <span id="service-pill" class="pill">Cargando</span>
          <span id="esp-pill" class="pill">ESP sin datos</span>
        </div>
      </header>

      <section id="page-home" class="page active">
        <div class="grid" id="cards"></div>
        <div class="panel">
          <div class="panel-head">
            <div class="section-title">
              <span class="icon-chip"><svg class="icon"><use href="#i-cpu"></use></svg></span>
              <div>
                <h2>Dispositivo ESP32</h2>
                <p class="section-sub">Conexion en vivo con el monitor</p>
              </div>
            </div>
            <span id="esp-state-pill" class="pill">Sin datos</span>
          </div>
          <div class="status-grid">
            <div class="kv"><span class="icon-chip"><svg class="icon"><use href="#i-clock"></use></svg></span><div class="kv-body"><span>Ultimo sondeo</span><strong id="esp-last-seen">-</strong></div></div>
            <div class="kv"><span class="icon-chip"><svg class="icon"><use href="#i-repeat"></use></svg></span><div class="kv-body"><span>Cadencia de sondeo</span><strong id="esp-interval">-</strong></div></div>
            <div class="kv"><span class="icon-chip"><svg class="icon"><use href="#i-zap"></use></svg></span><div class="kv-body"><span>Proximo sondeo</span><strong id="esp-next-poll">-</strong></div></div>
            <div class="kv"><span class="icon-chip"><svg class="icon"><use href="#i-globe"></use></svg></span><div class="kv-body"><span>IP</span><strong id="esp-ip">-</strong></div></div>
          </div>
          <p id="esp-effect" class="muted" style="margin:14px 0 0">Los cambios de configuracion se aplican en el proximo sondeo del ESP.</p>
        </div>
        <div class="panel">
          <div class="panel-head">
            <div class="section-title">
              <span class="icon-chip"><svg class="icon"><use href="#i-monitor"></use></svg></span>
              <div>
                <h2>Preview pantallas (128x64)</h2>
                <p class="section-sub">Lo que se renderiza en cada OLED</p>
              </div>
            </div>
            <span class="live-dot" title="Actualizando en vivo"></span>
          </div>
          <div class="preview-grid">
            <div class="preview-tile">
              <div class="preview-name"><svg class="icon"><use href="#i-sparkles"></use></svg>Claude</div>
              <img id="preview-claude" class="oled-preview" alt="preview claude">
            </div>
            <div class="preview-tile">
              <div class="preview-name"><svg class="icon"><use href="#i-terminal"></use></svg>Codex</div>
              <img id="preview-codex" class="oled-preview" alt="preview codex">
            </div>
          </div>
        </div>
        <div class="panel">
          <div class="panel-head">
            <div class="section-title">
              <span class="icon-chip"><svg class="icon"><use href="#i-code"></use></svg></span>
              <div>
                <h2>Snapshot para ESP32</h2>
                <p class="section-sub">JSON que consume el firmware</p>
              </div>
            </div>
          </div>
          <pre id="snapshot-json">{}</pre>
        </div>
      </section>

      <section id="page-usage" class="page">
        <div class="panel">
          <div class="panel-head">
            <div class="section-title">
              <span class="icon-chip"><svg class="icon"><use href="#i-gauge"></use></svg></span>
              <div>
                <h2>Uso y ventanas</h2>
                <p class="section-sub">Porcentajes y ventanas de Claude y Codex</p>
              </div>
            </div>
          </div>
          <form id="usage-form">
            <div class="form-grid">
              <label>Claude 5h restante %
                <input id="claude-remaining" type="number" min="0" max="100" step="1">
              </label>
              <label>Claude weekly restante %
                <input id="claude-weekly-remaining" type="number" min="0" max="100" step="1">
              </label>
              <label>Codex 5h restante %
                <input id="codex-remaining" type="number" min="0" max="100" step="1">
              </label>
              <label>Codex weekly restante %
                <input id="codex-weekly-remaining" type="number" min="0" max="100" step="1">
              </label>
              <label>Inicio ventana Claude 5h
                <input id="claude-start" type="datetime-local">
              </label>
              <label>Reset Claude 5h
                <input id="claude-reset" type="datetime-local">
              </label>
              <label>Inicio ventana Claude weekly
                <input id="claude-weekly-start" type="datetime-local">
              </label>
              <label>Reset Claude weekly
                <input id="claude-weekly-reset" type="datetime-local">
              </label>
              <label>Inicio ventana Codex 5h
                <input id="codex-start" type="datetime-local">
              </label>
              <label>Reset Codex 5h
                <input id="codex-reset" type="datetime-local">
              </label>
              <label>Inicio ventana Codex weekly
                <input id="codex-weekly-start" type="datetime-local">
              </label>
              <label>Reset Codex weekly
                <input id="codex-weekly-reset" type="datetime-local">
              </label>
              <label>Claude esperando respuesta
                <select id="claude-waiting">
                  <option value="false">No</option>
                  <option value="true">Si</option>
                </select>
              </label>
              <label>Codex esperando respuesta
                <select id="codex-waiting">
                  <option value="false">No</option>
                  <option value="true">Si</option>
                </select>
              </label>
            </div>
            <button class="primary" type="submit"><svg class="icon"><use href="#i-save"></use></svg>Guardar uso</button>
          </form>
        </div>
      </section>

      <section id="page-config" class="page">
        <div class="panel">
          <div class="panel-head">
            <div class="section-title">
              <span class="icon-chip"><svg class="icon"><use href="#i-sliders"></use></svg></span>
              <div>
                <h2>Configuracion del servicio</h2>
                <p class="section-sub">Etiquetas, cache y atenuado anti burn-in</p>
              </div>
            </div>
          </div>
          <form id="config-form">
            <div class="form-grid">
              <label>Etiqueta Claude
                <input id="claude-label" type="text">
              </label>
              <label>Etiqueta Codex
                <input id="codex-label" type="text">
              </label>
              <label>Tolerancia %
                <input id="tolerance" type="number" min="0" max="100" step="1">
              </label>
              <label>Estado Codex
                <input id="codex-status" type="text">
              </label>
              <label>Cache frames ESP32 (s)
                <input id="frame-cache-ttl-sec" type="number" min="1" step="1">
              </label>
              <label>Polling Claude (min)
                <input id="claude-ttl-min" type="number" min="1" step="1">
              </label>
              <label>Polling Codex (s)
                <input id="codex-ttl-sec" type="number" min="1" step="1">
              </label>
              <label>Atenuar brillo tras (s, 0=nunca)
                <input id="dim-after-sec" type="number" min="0" step="10">
              </label>
              <label>Brillo atenuado (%)
                <input id="dim-pct" type="number" min="0" max="100" step="5">
              </label>
            </div>
            <button class="primary" type="submit"><svg class="icon"><use href="#i-save"></use></svg>Guardar configuracion</button>
          </form>
        </div>
        <div class="panel">
          <div class="panel-head">
            <div class="section-title">
              <span class="icon-chip"><svg class="icon"><use href="#i-film"></use></svg></span>
              <div>
                <h2>Animacion de actividad</h2>
                <p class="section-sub">Esquina superior derecha cuando Claude o Codex trabaja; al esperar, la pantalla se invierte</p>
              </div>
            </div>
          </div>
          <form id="activity-form">
            <div class="muted" style="margin-bottom:6px">Estilo (clic en el preview para elegir)</div>
            <input id="anim-style" type="hidden">
            <div id="anim-gallery" class="anim-gallery"></div>
            <div class="form-grid" style="margin-top:14px">
              <label>Velocidad / frame (ms)
                <input id="anim-interval" type="number" min="20" step="10">
              </label>
              <label>Ancho del recuadro (px)
                <input id="anim-width" type="number" min="8" max="128" step="1">
              </label>
              <label>Alto del recuadro (px)
                <input id="anim-height" type="number" min="1" max="16" step="1">
              </label>
              <label>Invertir pantalla al esperar
                <select id="anim-invert">
                  <option value="true">Si</option>
                  <option value="false">No</option>
                </select>
              </label>
              <label>Parpadeo de inversion (ms)
                <input id="anim-blink" type="number" min="100" step="50">
              </label>
              <label>Ventana ocupado Codex (s)
                <input id="anim-codex-window" type="number" min="1" step="1">
              </label>
              <label>Obsoleto tras (s)
                <input id="anim-stale" type="number" min="60" step="10">
              </label>
              <label>Incluir subagentes de Codex
                <select id="anim-codex-subagents">
                  <option value="true">Si</option>
                  <option value="false">No</option>
                </select>
              </label>
              <label>Incluir subagentes de Claude
                <select id="anim-claude-subagents">
                  <option value="true">Si</option>
                  <option value="false">No</option>
                </select>
              </label>
            </div>
            <button class="primary" type="submit" style="margin-top:14px"><svg class="icon"><use href="#i-save"></use></svg>Guardar animacion</button>
          </form>
        </div>
        <div class="panel">
          <div class="panel-head">
            <div class="section-title">
              <span class="icon-chip"><svg class="icon"><use href="#i-palette"></use></svg></span>
              <div>
                <h2>Temas de pantallas</h2>
                <p class="section-sub">Clic en un tema para aplicarlo al instante</p>
              </div>
            </div>
          </div>
          <div id="theme-gallery" class="theme-gallery"></div>
        </div>
        <div class="panel">
          <div class="panel-head">
            <div class="section-title">
              <span class="icon-chip"><svg class="icon"><use href="#i-shield"></use></svg></span>
              <div>
                <h2>Salvapantallas (anti burn-in)</h2>
                <p class="section-sub">A pantalla completa cuando el servicio esta caido. Clic para aplicar</p>
              </div>
            </div>
          </div>
          <div id="saver-gallery" class="theme-gallery"></div>
        </div>
        <div class="panel">
          <div class="panel-head">
            <div class="section-title">
              <span class="icon-chip"><svg class="icon"><use href="#i-folder"></use></svg></span>
              <div>
                <h2>Rutas montadas</h2>
                <p class="section-sub">Diagnostico de rutas y fuentes del servicio</p>
              </div>
            </div>
          </div>
          <pre id="runtime-json">{}</pre>
        </div>
      </section>
    </main>
  </div>

  <script>
    let currentConfig = null;

    function $(id) { return document.getElementById(id); }
    let toastTimer = null;
    // Toast con variantes (loading/success/error/info), titulo y detalle. 'loading' no
    // se auto-oculta: queda fijo hasta que la operacion termina y lo reemplaza.
    function setToast(variant, title, detail) {
      const element = $('toast');
      $('toast-title').textContent = title;
      $('toast-detail').textContent = detail || '';
      $('toast-detail').style.display = detail ? 'block' : 'none';
      element.className = 'toast show ' + variant;
      if (toastTimer) { clearTimeout(toastTimer); toastTimer = null; }
      if (variant !== 'loading') {
        toastTimer = setTimeout(() => element.classList.remove('show'), variant === 'error' ? 6000 : 3400);
      }
    }
    // Estado de telemetria del ESP y seguimiento de "cuando toma efecto" un cambio.
    let espStatus = null;
    let espStatusAt = 0;
    let pendingEffect = null;
    function estServerNow() { return espStatus ? espStatus.server_now_ms + (Date.now() - espStatusAt) : Date.now(); }
    function secsText(ms) {
      const total = Math.max(0, Math.round(Number(ms || 0) / 1000));
      if (total < 60) return total + ' s';
      return Math.floor(total / 60) + ' m ' + (total % 60) + ' s';
    }
    function agoText(ms) { return 'hace ' + secsText(ms); }
    function effectEstimateText(status) {
      if (!status || !status.online) return 'El ESP esta sin conexion; se aplicara cuando reconecte.';
      const next = Number(status.next_poll_in_ms);
      if (next >= 0) return 'Se aplicara en ~' + secsText(next) + ' (proximo sondeo del ESP).';
      return 'Se aplicara en el proximo sondeo del ESP.';
    }
    async function saveConfig(body, label) {
      setToast('loading', 'Guardando ' + label + '...', 'Enviando al servicio');
      try {
        await postJson('/api/config', body);
        let status = null;
        try { status = await loadJson('/api/esp/status'); } catch (error) { status = null; }
        if (status) {
          espStatus = status;
          espStatusAt = Date.now();
          pendingEffect = { baselineFrames: Number(status.last_frames_ms || 0), label: label };
          renderEspStatus();
        }
        setToast('success', label + ' guardado', effectEstimateText(status));
        await refresh();
      } catch (error) {
        setToast('error', 'No se pudo guardar ' + label, String(error).slice(0, 160));
      }
    }
    function clamp(value) { return Math.max(0, Math.min(100, Number(value || 0))); }
    function toLocalInput(ms) {
      const date = new Date(ms);
      date.setMinutes(date.getMinutes() - date.getTimezoneOffset());
      return date.toISOString().slice(0, 16);
    }
    function fromLocalInput(value) { return new Date(value).getTime(); }
    function paceClass(pace, waiting) {
      if (waiting) return 'danger';
      if (pace === 'over') return 'danger';
      if (pace === 'under') return 'ok';
      return 'warn';
    }
    function paceText(pace) {
      if (pace === 'over') return 'Usando de mas';
      if (pace === 'under') return 'Usando de menos';
      if (pace === 'on_track') return 'En ritmo';
      return 'Sin datos';
    }
    function activityText(activity) {
      if (activity === 'busy') return 'Trabajando';
      if (activity === 'waiting') return 'Esperando respuesta';
      return 'Inactivo';
    }
    function activityClass(activity) {
      if (activity === 'busy') return 'warn';
      if (activity === 'waiting') return 'danger';
      return 'ok';
    }
    function dateText(ms) {
      const value = Number(ms || 0);
      if (value <= 0) return 'Sin datos';
      return new Date(value).toLocaleString();
    }
    function minutesText(seconds) {
      const totalMinutes = Math.max(0, Math.round(Number(seconds || 0) / 60));
      const days = Math.floor(totalMinutes / 1440);
      const hours = Math.floor((totalMinutes % 1440) / 60);
      const minutes = totalMinutes % 60;
      if (days > 0) return `${days}d ${hours}h`;
      if (hours > 0) return `${hours}h ${minutes}m`;
      return `${minutes}m`;
    }
    function icon(name, cls) { return '<svg class="icon ' + (cls || '') + '" aria-hidden="true"><use href="#i-' + name + '"></use></svg>'; }
    function activityIcon(activity) {
      if (activity === 'busy') return 'activity';
      if (activity === 'waiting') return 'bell';
      return 'moon';
    }
    function toolIcon(label) { return /codex/i.test(label) ? 'terminal' : 'sparkles'; }
    function toolChipClass(label) { return /codex/i.test(label) ? 'slate' : 'amber'; }
    function renderWindow(label, window) {
      return `
        <div class="window-block">
          <div class="window-head">
            <span class="window-label">${label}</span>
            <span class="pill ${paceClass(window.pace, false)}">${paceText(window.pace)}</span>
          </div>
          <div class="metric"><strong>${Math.round(window.remaining_percent)}%</strong><span class="muted">restante</span></div>
          <div class="bar"><span style="width:${clamp(window.remaining_percent)}%"></span></div>
          <div class="window-meta">
            <span>${icon('clock')}Reset ${minutesText(window.reset_in_seconds)}</span>
            <span>${icon('target')}Esperado ${Math.round(window.expected_remaining_percent)}%</span>
          </div>
        </div>`;
    }
    function renderTool(tool) {
      return `
        <article class="panel tool-card">
          <div class="tool-head">
            <span class="icon-chip ${toolChipClass(tool.label)}">${icon(toolIcon(tool.label))}</span>
            <div class="tool-id">
              <h2>${tool.label}</h2>
              <p>${tool.status_text}</p>
            </div>
            <span class="pill has-icon ${activityClass(tool.activity)}">${icon(activityIcon(tool.activity))}${activityText(tool.activity)}</span>
          </div>
          ${renderWindow('5h', tool.current)}
          ${renderWindow('Semanal', tool.weekly)}
          <div class="subgrid">
            <div class="kv"><span class="icon-chip">${icon('database')}</span><div class="kv-body"><span>Fuente cuota</span><strong>${tool.source}</strong></div></div>
            <div class="kv"><span class="icon-chip">${icon('clock')}</span><div class="kv-body"><span>Ultima lectura</span><strong>${dateText(tool.usage_observed_at_ms)}</strong></div></div>
            <div class="kv"><span class="icon-chip">${icon('message')}</span><div class="kv-body"><span>Mensajes</span><strong>${tool.observed_messages}</strong></div></div>
            <div class="kv"><span class="icon-chip">${icon('hash')}</span><div class="kv-body"><span>Tokens</span><strong>${tool.observed_tokens}</strong></div></div>
          </div>
        </article>`;
    }
    async function loadJson(path) {
      const response = await fetch(path, { cache: 'no-store' });
      if (!response.ok) throw new Error(await response.text());
      return response.json();
    }
    async function postJson(path, body) {
      const response = await fetch(path, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
      if (!response.ok) throw new Error(await response.text());
      return response.json();
    }
    let themeCache = null;
    let galleryBuilt = false;
    let animGalleryBuilt = false;
    let saverGalleryBuilt = false;
    const SCREENSAVERS = [
      { id: 'black', label: 'Negro' },
      { id: 'snake', label: 'Culebra' },
      { id: 'pipes', label: 'Pipes' },
      { id: 'matrix', label: 'Matrix' },
      { id: 'dvd', label: 'DVD' },
      { id: 'maze', label: 'Laberinto' },
      { id: 'flower', label: 'Flower Box' }
    ];
    const ANIM_STYLES = [
      { id: 'spinner', label: 'Spinner' },
      { id: 'dots', label: 'Puntos' },
      { id: 'dots-right', label: 'Puntos derecha' },
      { id: 'stars-right', label: 'Estrellas derecha' },
      { id: 'pulse', label: 'Pulso' },
      { id: 'bars', label: 'Barras' },
      { id: 'ball', label: 'Pelota' },
      { id: 'wave', label: 'Onda' },
      { id: 'worm', label: 'Gusano' }
    ];
    async function getThemes() {
      if (!themeCache) themeCache = (await loadJson('/api/themes')).themes;
      return themeCache;
    }
    function swapImage(imgEl, url) {
      const loader = new Image();
      loader.onload = () => { imgEl.src = url; imgEl.classList.add('loaded'); };
      loader.src = url;
    }
    async function selectTheme(id) {
      const config = structuredClone(currentConfig);
      config.theme = id;
      await saveConfig(config, 'Tema');
    }
    async function ensureThemeGallery() {
      if (galleryBuilt) return;
      const gallery = $('theme-gallery');
      if (!gallery) return;
      for (const theme of await getThemes()) {
        const card = document.createElement('button');
        card.type = 'button';
        card.className = 'theme-card';
        card.dataset.theme = theme.id;
        card.innerHTML = '<img alt="' + theme.label + '"><div class="theme-card-label"><span>' + theme.label + '</span><span class="theme-card-badge">' + icon('check') + 'Activo</span></div>';
        card.addEventListener('click', () => selectTheme(theme.id));
        gallery.appendChild(card);
      }
      galleryBuilt = true;
    }
    function updateThemeGallery(stamp) {
      document.querySelectorAll('.theme-card[data-theme]').forEach((card) => {
        const id = card.dataset.theme;
        card.classList.toggle('active', id === currentConfig.theme);
        swapImage(card.querySelector('img'), '/api/esp/preview/claude.png?theme=' + id + '&t=' + stamp);
      });
    }
    async function selectScreensaver(id) {
      const config = structuredClone(currentConfig);
      config.screensaver = id;
      await saveConfig(config, 'Salvapantallas');
    }
    function ensureSaverGallery() {
      if (saverGalleryBuilt) return;
      const gallery = $('saver-gallery');
      if (!gallery) return;
      for (const saver of SCREENSAVERS) {
        const card = document.createElement('button');
        card.type = 'button';
        card.className = 'theme-card';
        card.dataset.saver = saver.id;
        // El preview es un GIF que no depende de datos: se carga una vez para que anime.
        card.innerHTML = '<img alt="' + saver.label + '" src="/api/esp/screensaver-preview.gif?saver=' + saver.id + '"><div class="theme-card-label"><span>' + saver.label + '</span><span class="theme-card-badge">' + icon('check') + 'Activo</span></div>';
        card.querySelector('img').addEventListener('load', (event) => event.target.classList.add('loaded'));
        card.addEventListener('click', () => selectScreensaver(saver.id));
        gallery.appendChild(card);
      }
      saverGalleryBuilt = true;
    }
    function updateSaverGallery() {
      document.querySelectorAll('[data-saver]').forEach((card) => {
        card.classList.toggle('active', card.dataset.saver === currentConfig.screensaver);
      });
    }
    // Refresco periodico: actualiza datos vivos pero NO reescribe los formularios, para
    // no pisar lo que el usuario esta editando. Los formularios se llenan en refresh().
    async function refreshLive() {
      const snapshot = await loadJson('/api/esp/snapshot');
      const status = await loadJson('/api/status');
      currentConfig = await loadJson('/api/config');
      await ensureThemeGallery();
      ensureSaverGallery();
      $('service-pill').textContent = 'Servicio online';
      $('service-pill').className = 'pill ok';
      $('cards').innerHTML = renderTool(snapshot.claude) + renderTool(snapshot.codex);
      $('snapshot-json').textContent = JSON.stringify(snapshot, null, 2);
      $('runtime-json').textContent = JSON.stringify(status, null, 2);
      const stamp = Date.now();
      swapImage($('preview-claude'), '/api/esp/preview/claude.png?t=' + stamp);
      swapImage($('preview-codex'), '/api/esp/preview/codex.png?t=' + stamp);
      updateThemeGallery(stamp);
      updateSaverGallery();
    }
    // Refresco completo: ademas rellena los formularios (carga inicial y tras guardar).
    async function refresh() {
      await refreshLive();
      if (currentConfig) fillForms(currentConfig);
    }
    async function pollEspStatus() {
      try {
        const status = await loadJson('/api/esp/status');
        espStatus = status;
        espStatusAt = Date.now();
        // Confirmacion real: el ESP sondeo frames despues de guardar -> el cambio ya aplico.
        if (pendingEffect && Number(status.last_frames_ms) > Number(pendingEffect.baselineFrames)) {
          setToast('success', 'Aplicado en el ESP', pendingEffect.label + ' ya esta activo en el dispositivo.');
          pendingEffect = null;
        }
        renderEspStatus();
      } catch (error) { /* el servicio puede estar reiniciando; se reintenta */ }
    }
    function renderEspStatus() {
      const pill = $('esp-pill');
      const statePill = $('esp-state-pill');
      if (!espStatus || !espStatus.last_seen_ms) {
        pill.textContent = 'ESP sin datos';
        pill.className = 'pill';
        statePill.textContent = 'Sin datos';
        statePill.className = 'pill';
        $('esp-last-seen').textContent = '-';
        $('esp-interval').textContent = '-';
        $('esp-next-poll').textContent = '-';
        $('esp-ip').textContent = '-';
        return;
      }
      const now = estServerNow();
      const age = now - espStatus.last_seen_ms;
      const window = Math.max(30000, Number(espStatus.poll_interval_ms || 0) * 3 + 8000);
      const online = age <= window;
      pill.textContent = (online ? 'ESP conectado · ' : 'ESP sin conexion · ') + agoText(age);
      pill.className = 'pill ' + (online ? 'ok' : 'danger');
      statePill.textContent = online ? 'Conectado' : 'Sin conexion';
      statePill.className = 'pill ' + (online ? 'ok' : 'danger');
      $('esp-last-seen').textContent = agoText(age);
      $('esp-interval').textContent = espStatus.poll_interval_ms > 0 ? '~' + secsText(espStatus.poll_interval_ms) : 'Estimando...';
      let next = Number(espStatus.next_poll_in_ms);
      if (next >= 0) {
        next = Math.max(0, next - (now - espStatus.server_now_ms));
        $('esp-next-poll').textContent = online ? '~' + secsText(next) : 'En pausa';
      } else {
        $('esp-next-poll').textContent = 'Desconocido';
      }
      $('esp-ip').textContent = espStatus.ip || '-';
      const effect = $('esp-effect');
      if (pendingEffect) {
        statePill.classList.add('pending');
        effect.textContent = online
          ? 'Pendiente: ' + pendingEffect.label + ' — se aplicara en ~' + secsText(next >= 0 ? next : 0) + '.'
          : 'Pendiente: ' + pendingEffect.label + ' — esperando reconexion del ESP.';
      } else {
        effect.textContent = 'Los cambios de configuracion se aplican en el proximo sondeo del ESP.';
      }
    }
    function fillForms(config) {
      $('claude-label').value = config.claude.label;
      $('codex-label').value = config.codex.label;
      $('tolerance').value = config.tolerance_percent;
      $('codex-status').value = config.codex.status_text;
      $('claude-remaining').value = Math.round(config.claude.current.remaining_percent);
      $('claude-weekly-remaining').value = Math.round(config.claude.weekly.remaining_percent);
      $('codex-remaining').value = Math.round(config.codex.current.remaining_percent);
      $('codex-weekly-remaining').value = Math.round(config.codex.weekly.remaining_percent);
      $('claude-start').value = toLocalInput(config.claude.current.window_start_ms);
      $('claude-reset').value = toLocalInput(config.claude.current.window_reset_ms);
      $('claude-weekly-start').value = toLocalInput(config.claude.weekly.window_start_ms);
      $('claude-weekly-reset').value = toLocalInput(config.claude.weekly.window_reset_ms);
      $('codex-start').value = toLocalInput(config.codex.current.window_start_ms);
      $('codex-reset').value = toLocalInput(config.codex.current.window_reset_ms);
      $('codex-weekly-start').value = toLocalInput(config.codex.weekly.window_start_ms);
      $('codex-weekly-reset').value = toLocalInput(config.codex.weekly.window_reset_ms);
      $('claude-waiting').value = String(config.claude.waiting_for_user);
      $('codex-waiting').value = String(config.codex.waiting_for_user);
      $('frame-cache-ttl-sec').value = Math.max(1, Number(config.frame_cache_ttl_seconds || 5));
      $('claude-ttl-min').value = Math.max(1, Math.round((config.claude_usage_ttl_seconds || 300) / 60));
      $('codex-ttl-sec').value = Math.max(1, Number(config.codex_usage_ttl_seconds || 30));
      $('dim-after-sec').value = Math.max(0, Number(config.dim_after_seconds ?? 600));
      $('dim-pct').value = Math.min(100, Math.max(0, Number(config.dim_brightness_percent ?? 30)));
      const anim = config.activity_animation || {};
      $('anim-style').value = anim.style || 'spinner';
      $('anim-interval').value = Math.max(20, Number(anim.interval_ms || 120));
      $('anim-invert').value = String(anim.invert_on_waiting !== false);
      $('anim-blink').value = Math.max(100, Number(anim.invert_blink_ms || 600));
      $('anim-width').value = Math.min(128, Math.max(8, Number(anim.frame_width || 48)));
      $('anim-height').value = Math.min(16, Math.max(1, Number(anim.frame_height || 5)));
      $('anim-codex-window').value = Math.max(1, Number(anim.codex_busy_window_seconds || 15));
      $('anim-stale').value = Math.max(60, Number(anim.stale_seconds || 1800));
      $('anim-codex-subagents').value = String(anim.include_codex_subagents !== false);
      $('anim-claude-subagents').value = String(anim.include_claude_subagents === true);
      updateAnimPreview();
    }
    function ensureAnimGallery() {
      if (animGalleryBuilt) return;
      const gallery = $('anim-gallery');
      if (!gallery) return;
      for (const style of ANIM_STYLES) {
        const card = document.createElement('button');
        card.type = 'button';
        card.className = 'theme-card anim-card';
        card.dataset.style = style.id;
        card.innerHTML = '<img alt="' + style.label + '"><div class="theme-card-label"><span>' + style.label + '</span><span class="theme-card-badge">' + icon('check') + 'Activo</span></div>';
        card.addEventListener('click', () => selectAnimStyle(style.id));
        gallery.appendChild(card);
      }
      animGalleryBuilt = true;
    }
    function selectAnimStyle(id) {
      $('anim-style').value = id;
      updateAnimPreview();
    }
    function updateAnimPreview() {
      ensureAnimGallery();
      const selected = $('anim-style').value || 'spinner';
      const interval = Math.max(20, Number($('anim-interval').value || 120));
      const width = Math.min(128, Math.max(8, Number($('anim-width').value || 48)));
      const height = Math.min(16, Math.max(1, Number($('anim-height').value || 5)));
      const stamp = Date.now();
      document.querySelectorAll('.anim-card').forEach((card) => {
        const id = card.dataset.style;
        card.classList.toggle('active', id === selected);
        const img = card.querySelector('img');
        img.style.aspectRatio = width + ' / ' + height;
        swapImage(img, '/api/esp/activity-preview.gif?style=' + id + '&interval=' + interval + '&width=' + width + '&height=' + height + '&t=' + stamp);
      });
    }
    function configFromForms() {
      const config = structuredClone(currentConfig);
      config.tolerance_percent = Number($('tolerance').value);
      config.claude.label = $('claude-label').value;
      config.codex.label = $('codex-label').value;
      config.codex.status_text = $('codex-status').value;
      config.claude.current.remaining_percent = Number($('claude-remaining').value);
      config.claude.weekly.remaining_percent = Number($('claude-weekly-remaining').value);
      config.codex.current.remaining_percent = Number($('codex-remaining').value);
      config.codex.weekly.remaining_percent = Number($('codex-weekly-remaining').value);
      config.claude.current.window_start_ms = fromLocalInput($('claude-start').value);
      config.claude.current.window_reset_ms = fromLocalInput($('claude-reset').value);
      config.claude.weekly.window_start_ms = fromLocalInput($('claude-weekly-start').value);
      config.claude.weekly.window_reset_ms = fromLocalInput($('claude-weekly-reset').value);
      config.codex.current.window_start_ms = fromLocalInput($('codex-start').value);
      config.codex.current.window_reset_ms = fromLocalInput($('codex-reset').value);
      config.codex.weekly.window_start_ms = fromLocalInput($('codex-weekly-start').value);
      config.codex.weekly.window_reset_ms = fromLocalInput($('codex-weekly-reset').value);
      config.claude.waiting_for_user = $('claude-waiting').value === 'true';
      config.codex.waiting_for_user = $('codex-waiting').value === 'true';
      config.frame_cache_ttl_seconds = Math.max(1, Number($('frame-cache-ttl-sec').value || 5));
      config.claude_usage_ttl_seconds = Math.max(1, Number($('claude-ttl-min').value || 5)) * 60;
      config.codex_usage_ttl_seconds = Math.max(1, Number($('codex-ttl-sec').value || 30));
      config.dim_after_seconds = Math.max(0, Number($('dim-after-sec').value || 0));
      config.dim_brightness_percent = Math.min(100, Math.max(0, Number($('dim-pct').value || 30)));
      config.activity_animation = {
        style: $('anim-style').value,
        interval_ms: Math.max(20, Number($('anim-interval').value || 120)),
        invert_on_waiting: $('anim-invert').value === 'true',
        invert_blink_ms: Math.max(100, Number($('anim-blink').value || 600)),
        frame_width: Math.min(128, Math.max(8, Number($('anim-width').value || 48))),
        frame_height: Math.min(16, Math.max(1, Number($('anim-height').value || 5))),
        codex_busy_window_seconds: Math.max(1, Number($('anim-codex-window').value || 15)),
        stale_seconds: Math.max(60, Number($('anim-stale').value || 1800)),
        include_codex_subagents: $('anim-codex-subagents').value === 'true',
        include_claude_subagents: $('anim-claude-subagents').value === 'true'
      };
      return config;
    }
    document.querySelectorAll('nav button').forEach((button) => {
      button.addEventListener('click', () => {
        document.querySelectorAll('nav button').forEach((item) => item.classList.remove('active'));
        document.querySelectorAll('.page').forEach((page) => page.classList.remove('active'));
        button.classList.add('active');
        $('page-' + button.dataset.page).classList.add('active');
      });
    });
    $('usage-form').addEventListener('submit', (event) => {
      event.preventDefault();
      saveConfig(configFromForms(), 'Uso');
    });
    $('config-form').addEventListener('submit', (event) => {
      event.preventDefault();
      saveConfig(configFromForms(), 'Configuracion');
    });
    $('activity-form').addEventListener('submit', (event) => {
      event.preventDefault();
      saveConfig(configFromForms(), 'Animacion');
    });
    $('anim-interval').addEventListener('input', updateAnimPreview);
    $('anim-width').addEventListener('input', updateAnimPreview);
    $('anim-height').addEventListener('input', updateAnimPreview);
    function markServiceError(error) {
      $('service-pill').textContent = 'Servicio con error';
      $('service-pill').className = 'pill danger';
      if (error !== undefined) $('snapshot-json').textContent = String(error);
    }
    refresh().catch(markServiceError);
    pollEspStatus();
    setInterval(() => refreshLive().catch(markServiceError), 5000);
    setInterval(() => pollEspStatus(), 2500);
    setInterval(() => renderEspStatus(), 1000);
  </script>
</body>
</html>"""


def get_service_status() -> RuntimeStatus:
    claude_home = get_readonly_home("CLAUDE_HOME")
    codex_home = get_readonly_home("CODEX_HOME")
    config_path = get_config_path()
    config = load_config()

    return {
        "service": "ok",
        "claude_home": str(claude_home),
        "codex_home": str(codex_home),
        "config_path": str(config_path),
        "claude_home_exists": claude_home.exists(),
        "codex_home_exists": codex_home.exists(),
        "config_exists": config_path.exists(),
        "claude_credentials_exists": (claude_home / ".credentials.json").exists(),
        "codex_sessions_exists": (codex_home / "sessions").exists(),
        "claude_usage_source": CLAUDE_USAGE_URL,
        "codex_usage_source": "sessions/**/rate_limits",
        "frame_cache_ttl_seconds": config["frame_cache_ttl_seconds"],
        "claude_usage_ttl_seconds": config["claude_usage_ttl_seconds"],
        "codex_usage_ttl_seconds": config["codex_usage_ttl_seconds"],
    }


def record_esp_seen(ip: str) -> None:
    # Heartbeat ligero para endpoints del ESP que no son el sondeo de frames.
    global _esp_last_seen_ms, _esp_ip
    with _esp_lock:
        _esp_last_seen_ms = now_ms()
        if ip != "":
            _esp_ip = ip


def record_esp_frames_poll(ip: str) -> None:
    # Registra el sondeo de frames (heartbeat principal) y estima la cadencia con una
    # media exponencial, ignorando huecos largos (reconexiones) para no sesgarla.
    global _esp_last_seen_ms, _esp_last_frames_ms, _esp_frames_interval_ms, _esp_ip
    current_ms = now_ms()
    with _esp_lock:
        gap_ms = current_ms - _esp_last_frames_ms
        if _esp_last_frames_ms > 0 and 0 < gap_ms <= ESP_MAX_REASONABLE_GAP_MS:
            if _esp_frames_interval_ms == 0:
                _esp_frames_interval_ms = gap_ms
            else:
                _esp_frames_interval_ms = int(_esp_frames_interval_ms * 0.6 + gap_ms * 0.4)
        _esp_last_frames_ms = current_ms
        _esp_last_seen_ms = current_ms
        if ip != "":
            _esp_ip = ip


def record_esp_anim_fetch(ip: str) -> None:
    # El ESP descargo el paquete de animacion: confirma que aplico ese cambio.
    global _esp_last_anim_fetch_ms, _esp_last_seen_ms, _esp_ip
    with _esp_lock:
        current_ms = now_ms()
        _esp_last_anim_fetch_ms = current_ms
        _esp_last_seen_ms = current_ms
        if ip != "":
            _esp_ip = ip


def get_esp_status() -> dict[str, object]:
    # Estado del ESP para la web: si esta en linea, hace cuanto se le vio, cadencia de
    # sondeo y cuanto falta para el proximo (cuando aplicara un cambio recien guardado).
    config = load_config()
    current_ms = now_ms()
    with _esp_lock:
        last_seen_ms = _esp_last_seen_ms
        last_frames_ms = _esp_last_frames_ms
        interval_ms = _esp_frames_interval_ms
        last_anim_fetch_ms = _esp_last_anim_fetch_ms
        ip = _esp_ip

    online_window_ms = max(ESP_ONLINE_FALLBACK_MS, interval_ms * 3) if interval_ms > 0 else ESP_ONLINE_FALLBACK_MS
    online = last_seen_ms > 0 and (current_ms - last_seen_ms) <= online_window_ms

    next_poll_in_ms = -1
    if interval_ms > 0 and last_frames_ms > 0:
        next_poll_in_ms = max(0, interval_ms - (current_ms - last_frames_ms))

    return {
        "online": online,
        "server_now_ms": current_ms,
        "last_seen_ms": last_seen_ms,
        "last_frames_ms": last_frames_ms,
        "last_anim_fetch_ms": last_anim_fetch_ms,
        "poll_interval_ms": interval_ms,
        "next_poll_in_ms": next_poll_in_ms,
        "ip": ip,
        "frame_cache_ttl_seconds": config["frame_cache_ttl_seconds"],
    }


def dumps_json(value: object) -> bytes:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def read_request_json(handler: BaseHTTPRequestHandler) -> dict[str, Any]:
    content_length = int(handler.headers.get("Content-Length", "0"))
    if content_length <= 0:
        raise RuntimeError("El cuerpo JSON está vacío")

    raw_body = handler.rfile.read(content_length)
    try:
        value = json.loads(raw_body.decode("utf-8"))
    except UnicodeDecodeError as error:
        raise RuntimeError(f"El cuerpo no está codificado en UTF-8: {error}") from error
    except JSONDecodeError as error:
        raise RuntimeError(f"JSON inválido: {error}") from error

    if not isinstance(value, dict):
        raise RuntimeError("El cuerpo debe ser un objeto JSON")

    return cast(dict[str, Any], value)


def build_snapshot_response() -> dict[str, object]:
    config = load_config()
    claude_home = get_readonly_home("CLAUDE_HOME")
    codex_home = get_readonly_home("CODEX_HOME")

    return build_snapshot(config, claude_home, codex_home)


def bars_signature(claude_snapshot: ToolSnapshot, codex_snapshot: ToolSnapshot) -> str:
    # Firma de las barras de uso (porcentaje restante de cada ventana, redondeado).
    # Sirve para detectar cuando las barras NO cambian, sin confundirse con el reloj
    # de reset que sí cambia cada minuto.
    return ",".join(
        str(round(window["remaining_percent"]))
        for window in (
            claude_snapshot["current"],
            claude_snapshot["weekly"],
            codex_snapshot["current"],
            codex_snapshot["weekly"],
        )
    )


def build_frames_payload() -> tuple[bytes, str, str, str, str]:
    # Construye el snapshot una sola vez y deriva imagenes + estados de actividad,
    # para que el path critico de /api/esp/frames no tenga que reconstruirlo.
    config = load_config()
    claude_home = get_readonly_home("CLAUDE_HOME")
    codex_home = get_readonly_home("CODEX_HOME")
    snapshot = build_snapshot(config, claude_home, codex_home)
    theme = config["theme"]
    # El recuadro reservado debe coincidir con el tamaño de la animación configurada.
    anim = config["activity_animation"]
    set_activity_box(anim["frame_width"], anim["frame_height"])

    claude_snapshot = cast(ToolSnapshot, snapshot["claude"])
    codex_snapshot = cast(ToolSnapshot, snapshot["codex"])
    claude_image = render_tool_image(claude_snapshot["label"], claude_snapshot, theme)
    codex_image = render_tool_image(codex_snapshot["label"], codex_snapshot, theme)

    # Concatena los dos framebuffers (claude + codex) y calcula su ETag.
    payload = pack_frame(claude_image) + pack_frame(codex_image)
    etag = '"' + hashlib.sha1(payload).hexdigest() + '"'

    return (payload, etag, claude_snapshot["activity"], codex_snapshot["activity"], bars_signature(claude_snapshot, codex_snapshot))


def load_oled_font() -> ImageFont.ImageFont:
    try:
        return ImageFont.load_default()
    except OSError as error:
        raise RuntimeError(f"No se pudo cargar la fuente OLED por defecto: {error}") from error


def wrap_text(text: str, draw: ImageDraw.ImageDraw, font: ImageFont.ImageFont, max_width: int, max_lines: int) -> list[str]:
    # Envuelve por palabras dentro de max_width px, hasta max_lines lineas.
    lines: list[str] = []
    current = ""
    for word in text.split():
        candidate = word if current == "" else current + " " + word
        if int(draw.textlength(candidate, font=font)) <= max_width or current == "":
            current = candidate
        else:
            lines.append(current)
            current = word
            if len(lines) >= max_lines:
                return lines[:max_lines]
    if current != "" and len(lines) < max_lines:
        lines.append(current)
    return lines[:max_lines]


def draw_boot_screen(is_claude: bool, detail: str) -> Image.Image:
    # Misma estética que el arranque del firmware (marco + mascot) para que el estado
    # "esperando cache" no se vea como una pantalla distinta.
    image = Image.new("1", (WIDTH, HEIGHT), 0)
    draw_boot_decor(image, is_claude)
    draw = ImageDraw.Draw(image)
    font = load_oled_font()
    name = "Claude" if is_claude else "Codex"
    name_width = int(draw.textlength(name, font=font))
    draw.text((BOOT_MASCOT_CX - name_width // 2, 42), name, font=font, fill=1)
    log_width = WIDTH - BOOT_LOG_X - 3
    lines = ["Esperando", "cache..."] + wrap_text(detail, draw, font, log_width, 2)
    for index, line in enumerate(lines):
        draw.text((BOOT_LOG_X, 6 + index * 12), line, font=font, fill=1)

    return image


def build_fallback_frames_payload(detail: str) -> tuple[bytes, str]:
    claude_image = draw_boot_screen(True, detail)
    codex_image = draw_boot_screen(False, detail)
    payload = pack_frame(claude_image) + pack_frame(codex_image)
    etag = '"' + hashlib.sha1(payload).hexdigest() + '"'

    return (payload, etag)


def refresh_frame_cache(reason: str) -> None:
    global _bars_changed_at_ms, _bars_signature, _frame_claude_activity, _frame_codex_activity
    global _frame_etag, _frame_last_error, _frame_payload, _frame_refreshing, _frame_updated_at_ms

    started_at = time.monotonic()
    try:
        try:
            payload, etag, claude_activity, codex_activity, signature = build_frames_payload()
        except (RuntimeError, OSError, ValueError, TypeError, KeyError) as error:
            with _frame_cache_lock:
                _frame_last_error = str(error)
            log_event("frame_cache_refresh_failed", reason=reason, detail=str(error))
            return

        duration_ms = int((time.monotonic() - started_at) * 1000)
        refreshed_ms = now_ms()
        with _frame_cache_lock:
            _frame_payload = payload
            _frame_etag = etag
            _frame_claude_activity = claude_activity
            _frame_codex_activity = codex_activity
            # Solo se reinicia el reloj de atenuado cuando las BARRAS cambian de verdad.
            if signature != _bars_signature:
                _bars_signature = signature
                _bars_changed_at_ms = refreshed_ms
            _frame_updated_at_ms = refreshed_ms
            _frame_last_error = ""

        log_event("frame_cache_refreshed", reason=reason, duration_ms=duration_ms, bytes=len(payload), etag=etag)
    finally:
        with _frame_cache_lock:
            _frame_refreshing = False


def start_frame_cache_refresh(reason: str) -> None:
    global _frame_refreshing

    with _frame_cache_lock:
        if _frame_refreshing:
            return
        _frame_refreshing = True

    thread = threading.Thread(target=refresh_frame_cache, args=(reason,), daemon=True)
    thread.start()
    log_event("frame_cache_refresh_started", reason=reason)


def get_frame_cache_ttl_ms() -> int:
    config = load_config()
    return config["frame_cache_ttl_seconds"] * 1000


def get_frame_payload_for_esp() -> tuple[bytes, str, str, str, str]:
    current_ms = now_ms()
    cache_ttl_ms = get_frame_cache_ttl_ms()

    with _frame_cache_lock:
        cached_payload = _frame_payload
        cached_etag = _frame_etag
        cached_claude_activity = _frame_claude_activity
        cached_codex_activity = _frame_codex_activity
        cache_age_ms = current_ms - _frame_updated_at_ms
        should_refresh = not _frame_refreshing and (_frame_payload is None or cache_age_ms >= cache_ttl_ms)
        refresh_reason = "empty" if _frame_payload is None else "stale"
        last_error = _frame_last_error

    if should_refresh:
        start_frame_cache_refresh(refresh_reason)

    if cached_payload is not None:
        return (cached_payload, cached_etag, "hit", cached_claude_activity, cached_codex_activity)

    fallback_detail = last_error[:21] if last_error != "" else FALLBACK_FRAME_DETAIL
    fallback_payload, fallback_etag = build_fallback_frames_payload(fallback_detail)

    return (fallback_payload, fallback_etag, "fallback", cached_claude_activity, cached_codex_activity)


def current_activity_states() -> tuple[str, str]:
    config = load_config()
    claude_home = get_readonly_home("CLAUDE_HOME")
    codex_home = get_readonly_home("CODEX_HOME")
    return compute_activity_states(config, claude_home, codex_home, now_ms())


def current_activity_and_counts(
    config: ServiceConfig, claude_home: Path, codex_home: Path, current_ms: int
) -> tuple[str, str, int, int]:
    # Actividad (idle/busy/waiting) y conteo de sesiones busy de cada herramienta, en una sola
    # llamada que reusa config/homes ya cargados (evita recargarlos por separado en el path de
    # frames). El conteo de Codex sale del mismo recorrido cacheado que la actividad, asi que no
    # vuelve a escanear los rollouts.
    anim_config = config["activity_animation"]
    stale_seconds = anim_config["stale_seconds"]
    claude_activity, codex_activity = compute_activity_states(config, claude_home, codex_home, current_ms)
    claude_count = count_busy_claude_sessions(claude_home, current_ms, stale_seconds)
    codex_count = count_busy_codex_sessions(
        codex_home,
        current_ms,
        anim_config["codex_busy_window_seconds"],
        stale_seconds,
        anim_config["include_codex_subagents"],
    )
    return (claude_activity, codex_activity, claude_count, codex_count)


def current_brightness_percent(config: ServiceConfig) -> int:
    # 100% normalmente; baja a dim_brightness_percent cuando las barras llevan mas de
    # dim_after_seconds sin cambiar. dim_after_seconds=0 desactiva el atenuado.
    dim_after = config["dim_after_seconds"]
    if dim_after <= 0:
        return 100
    with _frame_cache_lock:
        changed_at = _bars_changed_at_ms
    if changed_at == 0:
        return 100
    if (now_ms() - changed_at) > (dim_after * 1000):
        return config["dim_brightness_percent"]
    return 100


def wait_for_activity_change(prev_claude: str, prev_codex: str) -> tuple[str, str]:
    # Bloquea el hilo de ESTE request (ThreadingHTTPServer da uno por conexion) hasta
    # que el estado difiera del que el ESP ya conoce, o hasta el tope de espera.
    deadline = time.monotonic() + ACTIVITY_LONGPOLL_HOLD_SECONDS
    while True:
        claude_activity, codex_activity = current_activity_states()
        if claude_activity != prev_claude or codex_activity != prev_codex:
            return (claude_activity, codex_activity)
        if time.monotonic() >= deadline:
            return (claude_activity, codex_activity)
        time.sleep(ACTIVITY_LONGPOLL_STEP_SECONDS)


def render_preview_png(tool_id: str, theme: str) -> bytes:
    config = load_config()
    claude_home = get_readonly_home("CLAUDE_HOME")
    codex_home = get_readonly_home("CODEX_HOME")
    snapshot = build_snapshot(config, claude_home, codex_home)
    effective_theme = theme if theme != "" else config["theme"]
    anim = config["activity_animation"]
    set_activity_box(anim["frame_width"], anim["frame_height"])

    tool = cast(ToolSnapshot, snapshot[tool_id])
    image = render_tool_image(tool["label"], tool, effective_theme)
    scaled = image.resize((WIDTH * PREVIEW_SCALE, HEIGHT * PREVIEW_SCALE), Image.NEAREST)

    buffer = BytesIO()
    scaled.save(buffer, format="PNG")

    return buffer.getvalue()


def parse_int_query(raw_value: str, fallback: int) -> int:
    # Convierte un parámetro de query a entero o cae al valor base si está vacío/roto.
    if raw_value == "":
        return fallback
    try:
        return int(raw_value)
    except ValueError:
        return fallback


def build_preview_animation_config(style: str, interval_raw: str, width_raw: str, height_raw: str) -> ActivityAnimationConfig:
    # Config efímera para previsualizar estilo/velocidad/tamaño sin guardarlos todavía.
    # Se normaliza para que el preview respete los mismos límites que la config real.
    base = load_config()["activity_animation"]
    effective_style = style if style in ACTIVITY_ANIMATION_STYLES else base["style"]

    raw_preview: dict[str, Any] = {
        "style": effective_style,
        "interval_ms": parse_int_query(interval_raw, base["interval_ms"]),
        "invert_on_waiting": base["invert_on_waiting"],
        "invert_blink_ms": base["invert_blink_ms"],
        "frame_width": parse_int_query(width_raw, base["frame_width"]),
        "frame_height": parse_int_query(height_raw, base["frame_height"]),
        "codex_busy_window_seconds": base["codex_busy_window_seconds"],
        "stale_seconds": base["stale_seconds"],
    }
    return normalize_activity_animation_config(raw_preview, base)


def save_config_from_payload(payload: dict[str, Any]) -> ServiceConfig:
    config = normalize_config(payload)
    save_config(config)
    # Reconstruye el frame de inmediato para que el cambio (tema, uso, brillo) este listo
    # en cache antes del proximo sondeo del ESP y aplique en una sola pasada.
    start_frame_cache_refresh("config_changed")

    return config


class MonitorRequestHandler(BaseHTTPRequestHandler):
    def send_bytes(self, status_code: int, content_type: str, body: bytes) -> None:
        self.send_response(status_code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def send_json(self, status_code: int, body: object) -> None:
        self.send_bytes(status_code, "application/json; charset=utf-8", dumps_json(body))

    def send_html(self) -> None:
        self.send_bytes(200, "text/html; charset=utf-8", APP_HTML.encode("utf-8"))

    def send_error_json(self, status_code: int, message: str) -> None:
        self.send_json(status_code, {"status": "error", "message": message})

    def send_frames(self) -> None:
        # Captura la IP del ESP32 (cliente) para mostrarla en el header.
        client_ip = self.client_address[0]
        set_device_ip(client_ip if len(client_ip.split(".")) == 4 else "")
        # Heartbeat principal: este sondeo marca la cadencia con la que el ESP aplica cambios.
        record_esp_frames_poll(client_ip)

        # La IMAGEN del frame sale del cache (su render es lo caro y disparaba timeouts
        # HTTPC_ERROR_READ_TIMEOUT si se reconstruia por poll). La ACTIVIDAD y el conteo de
        # sesiones, en cambio, se calculan frescos (baratos, con el escaneo de Codex cacheado):
        # asi el header X-Act coincide con el long-poll en tiempo real y el ESP no parpadea
        # entre el frame cacheado (hasta 5s viejo) y la reaccion viva.
        payload, etag, cache_status, _cached_claude_activity, _cached_codex_activity = get_frame_payload_for_esp()
        # La config y los homes se cargan una sola vez y se reusan para actividad, conteos,
        # animacion, saver y brillo (antes se recargaban por separado en cada uno).
        config = load_config()
        claude_home = get_readonly_home("CLAUDE_HOME")
        codex_home = get_readonly_home("CODEX_HOME")
        claude_activity, codex_activity, claude_sessions, codex_sessions = current_activity_and_counts(
            config, claude_home, codex_home, now_ms()
        )

        # La versión de la animación y el salvapantallas elegido viajan en headers para
        # que el ESP los reciba incluso en 304 (y guarde el saver para usarlo offline).
        anim_etag = activity_animation_etag(config["activity_animation"])
        anim_style = config["activity_animation"]["style"]
        saver = config["screensaver"]
        # El servicio decide el brillo (conoce las barras reales) y el ESP solo lo aplica.
        brightness = str(current_brightness_percent(config))

        if self.headers.get("If-None-Match") == etag:
            self.send_response(304)
            self.send_header("ETag", etag)
            self.send_header("X-Frame-Cache", cache_status)
            self.send_header("X-Act-Claude", claude_activity)
            self.send_header("X-Act-Codex", codex_activity)
            self.send_header("X-Sessions-Claude", str(claude_sessions))
            self.send_header("X-Sessions-Codex", str(codex_sessions))
            self.send_header("X-Anim-Etag", anim_etag)
            self.send_header("X-Anim-Style", anim_style)
            self.send_header("X-Saver", saver)
            self.send_header("X-Brightness", brightness)
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            return

        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("ETag", etag)
        self.send_header("X-Frame-Cache", cache_status)
        self.send_header("X-Act-Claude", claude_activity)
        self.send_header("X-Act-Codex", codex_activity)
        self.send_header("X-Sessions-Claude", str(claude_sessions))
        self.send_header("X-Sessions-Codex", str(codex_sessions))
        self.send_header("X-Anim-Etag", anim_etag)
        self.send_header("X-Anim-Style", anim_style)
        self.send_header("X-Saver", saver)
        self.send_header("X-Brightness", brightness)
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        try:
            self.wfile.write(payload)
        except BrokenPipeError as error:
            log_event("esp_frames_client_disconnected", client_ip=client_ip, cache_status=cache_status, detail=str(error))

    def send_preview(self, tool_id: str) -> None:
        query = parse_qs(urlparse(self.path).query)
        theme_values = query.get("theme", [""])
        self.send_bytes(200, "image/png", render_preview_png(tool_id, theme_values[0]))

    def send_activity_animation(self) -> None:
        # Paquete binario que el ESP32 descarga y almacena (frames + parámetros).
        record_esp_anim_fetch(self.client_address[0])
        anim_config = load_config()["activity_animation"]
        etag = activity_animation_etag(anim_config)

        if self.headers.get("If-None-Match") == etag:
            self.send_response(304)
            self.send_header("ETag", etag)
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            return

        payload = pack_activity_animation(anim_config)
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("ETag", etag)
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        try:
            self.wfile.write(payload)
        except BrokenPipeError as error:
            log_event("esp_activity_animation_client_disconnected", detail=str(error))

    def send_activity_wait(self) -> None:
        # Long-poll: responde apenas la actividad cambia respecto a lo que el ESP envia
        # en ?c=<claude>&x=<codex>, o tras el tope de espera. Cuerpo minimo "<c> <x>".
        record_esp_seen(self.client_address[0])
        query = parse_qs(urlparse(self.path).query)
        prev_claude = query.get("c", [""])[0]
        prev_codex = query.get("x", [""])[0]
        claude_activity, codex_activity = wait_for_activity_change(prev_claude, prev_codex)
        body = f"{claude_activity} {codex_activity}".encode("ascii")
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=ascii")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("X-Act-Claude", claude_activity)
        self.send_header("X-Act-Codex", codex_activity)
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        try:
            self.wfile.write(body)
        except BrokenPipeError as error:
            log_event("esp_activity_wait_client_disconnected", detail=str(error))

    def send_activity_preview(self) -> None:
        query = parse_qs(urlparse(self.path).query)
        style = query.get("style", [""])[0]
        interval = query.get("interval", [""])[0]
        width = query.get("width", [""])[0]
        height = query.get("height", [""])[0]
        preview_config = build_preview_animation_config(style, interval, width, height)
        self.send_bytes(200, "image/gif", render_activity_preview_gif(preview_config))

    def send_screensaver_preview(self) -> None:
        query = parse_qs(urlparse(self.path).query)
        saver = query.get("saver", [""])[0]
        if saver not in SCREENSAVERS:
            saver = "black"
        self.send_bytes(200, "image/gif", render_screensaver_preview_gif(saver))

    def do_GET(self) -> None:
        parsed_path = urlparse(self.path).path

        try:
            if parsed_path in ["/", "/home", "/usage", "/config"]:
                self.send_html()
                return

            if parsed_path == "/api/config":
                self.send_json(200, load_config())
                return

            if parsed_path == "/api/status":
                self.send_json(200, get_service_status())
                return

            if parsed_path == "/api/esp/status":
                self.send_json(200, get_esp_status())
                return

            if parsed_path == "/api/themes":
                self.send_json(200, {"themes": THEME_DEFINITIONS})
                return

            if parsed_path == "/api/esp/snapshot":
                self.send_json(200, build_snapshot_response())
                return

            if parsed_path == "/api/esp/frames":
                self.send_frames()
                return

            if parsed_path == "/api/esp/activity-wait":
                self.send_activity_wait()
                return

            if parsed_path == "/api/esp/preview/claude.png":
                self.send_preview("claude")
                return

            if parsed_path == "/api/esp/preview/codex.png":
                self.send_preview("codex")
                return

            if parsed_path == "/api/esp/activity-animation":
                self.send_activity_animation()
                return

            if parsed_path == "/api/esp/activity-preview.gif":
                self.send_activity_preview()
                return

            if parsed_path == "/api/esp/screensaver-preview.gif":
                self.send_screensaver_preview()
                return

            self.send_error_json(404, f"Ruta no encontrada: {parsed_path}")
        except RuntimeError as error:
            self.send_error_json(500, str(error))

    def do_POST(self) -> None:
        parsed_path = urlparse(self.path).path

        try:
            if parsed_path == "/api/config":
                payload = read_request_json(self)
                self.send_json(200, save_config_from_payload(payload))
                return

            if parsed_path == "/api/usage/manual":
                payload = read_request_json(self)
                config = load_config()
                updated_config = save_config_from_payload({**config, **payload})
                self.send_json(200, updated_config)
                return

            self.send_error_json(404, f"Ruta no encontrada: {parsed_path}")
        except RuntimeError as error:
            self.send_error_json(400, str(error))

    def log_message(self, message_format: str, *args: object) -> None:
        print(f"{self.address_string()} - {message_format % args}")


def run_server() -> NoReturn:
    raw_host = os.environ.get("SERVICE_HOST")
    raw_port = os.environ.get("SERVICE_PORT")
    if raw_host is None or raw_host == "":
        raise RuntimeError("SERVICE_HOST no está configurado")
    if raw_port is None or raw_port == "":
        raise RuntimeError("SERVICE_PORT no está configurado")

    server_address = (raw_host, int(raw_port))
    server = ThreadingHTTPServer(server_address, MonitorRequestHandler)
    print(f"Servicio iniciado en http://{raw_host}:{raw_port}")
    server.serve_forever()


if __name__ == "__main__":
    run_server()
