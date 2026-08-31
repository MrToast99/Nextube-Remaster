#!/usr/bin/env python3
"""
social_relay.py — Local social-counter relay for Nextube.

Version: 1.1

Run this script on any PC that is on the same Wi-Fi network as your
Nextube clock.  It fetches public profile pages with a real browser
(Playwright/Chromium when available, curl otherwise) and serves the
counts as plain JSON so the ESP32 doesn't need API keys or deal with
bot checks.

Requirements:
    Python 3.6+   — no external packages required for curl/urllib mode.

    Playwright mode (recommended — best bot-detection bypass):
        pip install playwright
        playwright install chromium

Usage:
    python social_relay.py

Then open the Nextube web UI, go to Settings → Social Media Counters,
and enter the IP address printed below as the "Relay host".

Routes:
    http://<relay-host>:8888/tiktok?user=<username>
        -> {"followers": 12345}

    http://<relay-host>:8888/youtube?channel=<channel-id>
        -> {"subscribers": 12345}
        (no YouTube API key required — scrapes the public channel page)

    http://<relay-host>:8888/health
        -> OK

Fetch strategy (tried in order, first success wins):
    1. Playwright/Chromium — real browser; passes JS fingerprinting &
       TLS JA3 checks.  Highest reliability.  Falls back automatically
       if playwright is not installed.
    2. curl               — correct TLS fingerprint; no JS.  Ships with
       Windows 10+, macOS, and all major Linux distros.
    3. urllib             — pure Python fallback; may be blocked by WAF.
"""

__version__ = "1.1"

import base64
import contextlib
import gzip
import json
import os
import shutil
import subprocess
import random
import string
import uuid
import re
import socket
import ssl
import threading
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

# ── Dependency bootstrap ─────────────────────────────────────────────────────
# Automatically installs Playwright and playwright-stealth when missing so the
# relay works out-of-the-box with just "python social_relay.py".
def _bootstrap():
    import importlib.util as _ilu
    import sys as _sys

    def _pip(*pkgs):
        print(f"[relay] Installing {' '.join(pkgs)} …")
        try:
            subprocess.check_call(
                [_sys.executable, "-m", "pip", "install", "--quiet", *pkgs],
                stderr=subprocess.STDOUT,
            )
            return True
        except Exception as _e:
            print(f"[relay] pip install failed: {_e}")
            return False

    if _ilu.find_spec("playwright") is None:
        if _pip("playwright>=1.40"):
            print("[relay] Installing Chromium browser …")
            try:
                subprocess.check_call(
                    [_sys.executable, "-m", "playwright", "install", "chromium"],
                    stderr=subprocess.STDOUT,
                )
                print("[relay] Playwright + Chromium ready.")
            except Exception as _e:
                print(f"[relay] playwright install chromium failed: {_e}")

    if _ilu.find_spec("playwright_stealth") is None:
        if _pip("playwright-stealth>=1.0.6"):
            print("[relay] playwright-stealth ready.")

_bootstrap()

# ── Configuration ────────────────────────────────────────────────────────────
PORT      = 8888   # must match the port compiled into the ESP32 firmware
CACHE_TTL = 300    # seconds to cache a result before re-fetching (5 min)

_UA = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
    "AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/124.0.0.0 Safari/537.36"
)

# ── Playwright (optional) ─────────────────────────────────────────────────────
# Install with:  pip install playwright && playwright install chromium
try:
    from playwright.sync_api import sync_playwright, TimeoutError as PWTimeout
    _PLAYWRIGHT_AVAILABLE = True
except ImportError:
    _PLAYWRIGHT_AVAILABLE = False

# ── playwright-stealth (optional, strongly recommended) ───────────────────────
# Patches the 10+ browser signals that expose headless Chromium to WAF/bot
# detection: navigator.webdriver, window.chrome stub, plugin arrays, permission
# API, WebGL vendor string, hairline-feature checks, and more.
#
# Without stealth TikTok can reliably fingerprint the automation context even
# with a realistic User-Agent and real cookies.
#
# Install with:  pip install playwright-stealth
_STEALTH_AVAILABLE = False
_pw_stealth_apply  = None
_stealth_import_err = None
try:
    # playwright-stealth v1.x (AtubTech) — stealth_sync(page) API
    from playwright_stealth import stealth_sync as _pw_stealth_apply
    _STEALTH_AVAILABLE = True
except ImportError:
    try:
        # Some fork/newer builds expose a Stealth class instead.
        from playwright_stealth import Stealth as _PwStealth
        def _pw_stealth_apply(page):   # noqa: E306
            _PwStealth().apply_stealth_sync(page)
        _STEALTH_AVAILABLE = True
    except ImportError:
        import sys as _sys
        _stealth_import_err = (
            f"module not found for {_sys.executable}\n"
            f"                         Fix: {_sys.executable} -m pip install playwright-stealth"
        )
    except Exception as _e:
        _stealth_import_err = str(_e)
except Exception as _e:
    # Catch version mismatches, missing sub-dependencies, etc. so the relay
    # still starts; the error is shown in the startup banner.
    _stealth_import_err = str(_e)

# Singleton browser instance — started lazily on first use, kept alive for the
# duration of the relay process.  Access serialised via _pw_lock so concurrent
# HTTP requests don't open overlapping browser pages.
_pw_lock    = threading.Lock()
_pw_pw      = None   # playwright.sync_api.Playwright context manager result
_pw_browser = None   # playwright.sync_api.Browser
_pw_ready   = False  # True once _pw_ensure() has run (even if init failed)

# Playwright's sync API is thread-affinitized via greenlets: every call must
# run on the exact same OS thread that first called sync_playwright().start(),
# for the life of the process — calling it from a second thread fails with
# "Cannot switch to a different thread".  ThreadingHTTPServer (below) hands
# each HTTP request its own thread, so calling Playwright directly from a
# request thread breaks this.  (It didn't break under the original
# single-threaded HTTPServer, since every request — and so every Playwright
# call — ran on that one thread by construction; ThreadingHTTPServer traded
# that accidental safety for concurrency without preserving it here.)  Route
# every Playwright call — the one-time browser launch in _pw_ensure(), every
# fetch, and the shutdown cleanup — through this single persistent worker
# thread instead, so they always land on the same thread no matter which
# request thread asked for them.  _pw_lock above still guards the shared
# browser/context objects themselves; _pw_call only fixes thread identity.
_pw_executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="pw-worker")


def _pw_call(fn, *args, timeout=60, **kwargs):
    """
    Run fn(*args, **kwargs) on the dedicated Playwright worker thread and
    block until it completes.  Re-raises whatever fn raises, plus
    concurrent.futures.TimeoutError if it doesn't finish within `timeout`s.

    Default of 60s comfortably covers each fetcher's own worst-case internal
    wait (e.g. YouTube's 30s page.goto + 20s wait_for_function ≈ 50s) — a
    tighter default risked this wrapper giving up and falling back to
    curl/urllib before a still-running Playwright fetch would have
    succeeded on its own.  Note a timeout here only stops this caller from
    waiting; it does NOT cancel the job — it keeps running on the worker
    thread and (since there is only one worker) delays whatever fetch is
    submitted next until it finishes.
    """
    return _pw_executor.submit(fn, *args, **kwargs).result(timeout=timeout)


def _pw_ensure():
    """
    Lazily launch the Playwright Chromium browser singleton.
    Thread-safe via double-checked locking.  Returns True if the browser
    is available, False if Playwright is not installed or failed to start.
    """
    global _pw_pw, _pw_browser, _pw_ready
    if _pw_ready:
        return _pw_browser is not None
    with _pw_lock:
        if _pw_ready:
            return _pw_browser is not None
        _pw_ready = True
        if not _PLAYWRIGHT_AVAILABLE:
            return False
        try:
            _pw_pw = sync_playwright().start()
            _pw_browser = _pw_pw.chromium.launch(
                headless=True,
                args=[
                    # Remove the "--enable-automation" flag that Chromium
                    # injects by default — it sets window.chrome.app to a
                    # stub value that bot-detection scripts check for.
                    "--disable-blink-features=AutomationControlled",
                    # Suppress the "Chrome is being controlled by automated
                    # software" info-bar (irrelevant in headless, but removes
                    # one more fingerprinting surface).
                    "--disable-infobars",
                    # Prevents WebGL from reporting "SwiftShader" (CPU renderer)
                    # as the renderer string — TikTok's canvas fingerprint check
                    # flags this as a bot signal.
                    "--use-gl=angle",
                    "--use-angle=swiftshader-webgl",
                    # Recommended for stability in Linux/Docker environments.
                    "--no-sandbox",
                    "--disable-dev-shm-usage",
                ],
            )
            print("[relay] Playwright/Chromium started")
            return True
        except Exception as exc:
            print(f"[relay] Playwright init failed: {exc}")
            print("[relay] Falling back to curl/urllib.")
            _pw_browser = None
            return False


def _pw_stealth(page):
    """
    Apply all available stealth patches to a Playwright page object.

    Two layers:
      1. playwright-stealth library — patches ~12 browser APIs that headless
         Chromium exposes differently from a real browser session.
      2. Manual navigator.webdriver patch — belt-and-suspenders fallback for
         the single most-checked signal, applied via addInitScript so it runs
         before any page JS (including TikTok's WAF probe).

    Safe to call even if playwright-stealth is not installed.
    """
    if _STEALTH_AVAILABLE:
        _pw_stealth_apply(page)
    # Belt-and-suspenders: always patch webdriver even if stealth lib present,
    # since some site scripts run before playwright-stealth's init script.
    page.add_init_script(
        "Object.defineProperty(navigator, 'webdriver', {get: () => undefined})"
    )


@contextlib.contextmanager
def _pw_page():
    """
    Open a new Playwright browser context + page with stealth patches applied,
    yield the page, and guarantee both are closed on the way out — including
    when context creation, page creation, or stealth patching itself raises.

    Serialises access via _pw_lock (held for the whole call) so concurrent
    fetch requests don't open overlapping browser pages.  Shared by all three
    fetchers so the create/patch/close lifecycle only needs to be correct in
    one place instead of three independently-maintained copies.
    """
    with _pw_lock:
        ctx = None
        page = None
        try:
            ctx = _pw_browser.new_context(user_agent=_UA)
            page = ctx.new_page()
            _pw_stealth(page)   # patch webdriver + headless signals before navigation
            yield page
        finally:
            if page is not None:
                try:
                    page.close()
                except Exception:
                    pass
            if ctx is not None:
                try:
                    ctx.close()
                except Exception:
                    pass


def _pw_fetch_tiktok(username):
    """
    Use Playwright/Chromium to retrieve a TikTok follower count.

    Extraction strategy (first match wins):
      1. DOM element  — [data-e2e="followers-count"] text content,
                        parsed via _parse_abbrev_count.
      2. Page source  — followerCount / follower_count / fans JSON keys
                        embedded by TikTok's SIGI_STATE script tag.

    Returns an integer count, or None on any failure.
    """
    if not _pw_ensure():
        return None

    print(f"  [relay] Playwright: fetching TikTok @{username}…")
    try:
        with _pw_page() as page:
            page.goto(
                f"https://www.tiktok.com/@{username}",
                wait_until="domcontentloaded",
                timeout=30_000,
            )

            # Dismiss cookie consent banner if present (EU/UK users).  Catches
            # any Playwright error here (not just PWTimeout) — e.g. a
            # strict-mode violation if the selector matches more than one
            # element — since a failed dismiss should never abort the fetch.
            try:
                btn = page.locator('[data-e2e="cookie-banner-accept"]')
                if btn.is_visible(timeout=2_000):
                    btn.click()
            except Exception:
                pass

            # Attempt 1: follower count element.
            count = None
            try:
                page.wait_for_selector(
                    '[data-e2e="followers-count"]', timeout=8_000
                )
                el = page.query_selector('[data-e2e="followers-count"]')
                if el:
                    count = _parse_abbrev_count(el.inner_text().strip())
            except PWTimeout:
                pass

            # Attempt 2: JSON embedded in page source.
            src = None
            if count is None:
                src = page.content()
                for pat in (
                    r'"followerCount"\s*:\s*(\d+)',
                    r'"follower_count"\s*:\s*(\d+)',
                    r'"fans"\s*:\s*(\d+)',
                ):
                    m = re.search(pat, src)
                    if m:
                        count = int(m.group(1))
                        break

            if count is not None:
                print(f"  [relay] Playwright: @{username} → {count:,} followers  ✓")
            else:
                print(f"  [relay] Playwright: count not found for @{username}")
                # Diagnose WHY: TikTok's bot-detection can silently bounce a
                # profile request to the plain homepage (or a login/captcha
                # page) before any follower-count content ever exists — that
                # looks identical to a stale-selector failure from the
                # log line above, but has a different cause and no fix on
                # our end (a page structure change, by contrast, would mean
                # the extraction patterns need updating).  Landed URL/title
                # tell them apart at a glance.
                landed_url = page.url
                on_profile = f"@{username}".lower() in landed_url.lower()
                if not on_profile:
                    print(f"  [relay] Playwright: landed on {landed_url!r} "
                          f"instead of the profile — looks like TikTok "
                          f"redirected/blocked the request (bot-detection, "
                          f"CAPTCHA, or a login wall), not a page-structure "
                          f"change.")
                else:
                    print(f"  [relay] Playwright: stayed on the profile page "
                          f"but no follower count matched.")
                if src is None:
                    src = page.content()

                # Known WAF/challenge markers seen on TikTok's block pages —
                # a hit here means what came back is a challenge shell, not
                # the real profile, regardless of what the URL/title say.
                waf_markers = ("SlardarWAF", "verify you are human", "Access Denied",
                               "unusual traffic", "id=\"captcha")
                hit = next((w for w in waf_markers if w.lower() in src.lower()), None)
                if hit:
                    print(f"  [relay] Playwright: page contains challenge "
                          f"marker {hit!r} — this is a bot-check page, not "
                          f"the real profile.")

                # Visible text snippet — far more useful than a raw HTML dump
                # for eyeballing what TikTok actually served (a challenge
                # message, an age-gate, a genuinely restructured profile,
                # etc).  Collapsed to one line so it doesn't flood the log.
                try:
                    body_text = page.inner_text("body").strip()
                    snippet = " ".join(body_text.split())[:300]
                except Exception:
                    snippet = "(could not read visible text)"
                print(f"  [relay] Playwright: title={page.title()!r} "
                      f"({len(src)} chars) — visible text: {snippet!r}")
            return count

    except Exception as exc:
        print(f"  [relay] Playwright TikTok @{username}: {exc}")
        return None


def _yt_url(channel_id):
    """
    Build the correct YouTube channel URL for any supported identifier:
      UC…  — traditional channel ID (exactly 24 chars)  →  /channel/UC…
      @…   — modern handle                               →  /@handle
      bare — handle without @                             →  /@handle

    Real channel IDs are always exactly 24 characters ("UC" + a 22-char
    base64url-ish suffix) — checking the exact length (not just a loose
    "> 10") is what distinguishes a real channel ID from a bare handle that
    happens to start with "UC" (e.g. someone typing "UCBerkeleyOfficial"
    as a handle without the leading @).
    """
    cid = channel_id.strip()
    if cid.startswith("@"):
        return f"https://www.youtube.com/{cid}"
    if cid.startswith("UC") and len(cid) == 24:
        return f"https://www.youtube.com/channel/{cid}"
    # Bare handle (no @) — treat as @handle
    return f"https://www.youtube.com/@{cid}"


def _yt_dismiss_consent(page):
    """
    Dismiss YouTube's GDPR cookie-consent wall if Playwright landed on it.

    In EU/EEA regions YouTube redirects fresh browser sessions to
    consent.youtube.com before showing any content.  We check the landed
    URL first so there is no timeout penalty for users outside those regions.
    """
    if ("consent.youtube.com" not in page.url and
            "accounts.google.com" not in page.url):
        return   # not on a consent/sign-in page — fast exit

    # Try the most stable selectors in order; stop as soon as one clicks.
    for sel in (
        '[jsname="higCR"]',                     # "Accept all" internal name
        'button[aria-label="Accept all"]',      # English label
        'form[action*="consent"] button + button',  # second button = Accept
    ):
        try:
            btn = page.locator(sel).first
            if btn.is_visible(timeout=3_000):
                btn.click()
                page.wait_for_load_state("domcontentloaded", timeout=15_000)
                return
        except Exception:
            continue


def _pw_fetch_youtube(channel_id):
    """
    Use Playwright/Chromium to retrieve a YouTube subscriber count.

    Supports both UC… channel IDs and @handle identifiers.
    Dismisses YouTube's GDPR consent wall when present (EU/EEA regions).

    Returns an integer count, or None on any failure.
    """
    if not _pw_ensure():
        return None

    print(f"  [relay] Playwright: fetching YouTube {channel_id}…")
    try:
        with _pw_page() as page:
            page.goto(
                _yt_url(channel_id),
                wait_until="domcontentloaded",
                timeout=30_000,
            )
            # Dismiss GDPR consent wall before trying to read any content.
            _yt_dismiss_consent(page)

            def _yt_patterns(src):
                """Try all known YouTube subscriber count patterns; return int or None."""
                for pat, grp in (
                    # 2024+ metadataParts: "content":"N subscribers"
                    (r'"content"\s*:\s*"([\d,.]+\.?\d*\s*[KkMmBb]?\s*subscriber[^"]*)"', 1),
                    # Rendered HTML aria-label
                    (r'aria-label="([\d,.]+\.?\d*\s*[KkMmBb]?\s*subscriber[^"]*)"', 1),
                    # Legacy: simpleText nested inside accessibility block
                    (r'"subscriberCountText"(.{0,600}?)"simpleText"\s*:\s*"([^"]+)"', 2),
                    # Legacy flat format
                    (r'"subscriberCountText"\s*:\s*\{"simpleText"\s*:\s*"([^"]+)"', 1),
                    # Raw numeric string
                    (r'"subscriberCount"\s*:\s*"(\d+)"', 1),
                ):
                    m = re.search(pat, src, re.DOTALL)
                    if m:
                        c = _parse_abbrev_count(m.group(grp))
                        if c is not None:
                            return c
                return None

            # Fast path: ytInitialData is embedded in the initial HTML —
            # try source patterns immediately without any extra waits.
            count = _yt_patterns(page.content())

            # Slow path: subscriber count loaded by JS after hydration.
            # Wait for the DOM element to be populated, then re-scan source.
            if count is None:
                try:
                    page.wait_for_function(
                        "() => { const el = document.querySelector('#subscriber-count');"
                        " return el && el.textContent.trim().length > 0; }",
                        timeout=20_000,
                    )
                    count = _yt_patterns(page.content())
                except Exception:
                    pass

            if count is not None:
                print(f"  [relay] Playwright: {channel_id} → {count:,} subscribers  ✓")
            else:
                print(f"  [relay] Playwright: subscriber count not found for {channel_id}")
            return count

    except Exception as exc:
        print(f"  [relay] Playwright YouTube {channel_id}: {exc}")
        return None


def _pw_fetch_instagram(username):
    """
    Use Playwright/Chromium to retrieve an Instagram follower count.

    Extraction strategy (first match wins):
      1. JSON embedded in page source — "edge_followed_by":{"count":N}
         or "follower_count":N (present in window.__additionalDataLoaded /
         shared_data script blocks for public profiles).
      2. meta[name="description"] — Instagram sets this to
         "N Followers, M Following, K Posts – …" for public accounts.

    Returns an integer count, or None on any failure.
    """
    if not _pw_ensure():
        return None

    print(f"  [relay] Playwright: fetching Instagram @{username}…")
    try:
        with _pw_page() as page:
            page.goto(
                f"https://www.instagram.com/{username}/",
                wait_until="domcontentloaded",
                timeout=30_000,
            )
            src = page.content()

            # JSON patterns embedded in the page source
            for pat in (
                r'"edge_followed_by"\s*:\s*\{"count"\s*:\s*(\d+)',
                r'"follower_count"\s*:\s*(\d+)',
            ):
                m = re.search(pat, src)
                if m:
                    count = int(m.group(1))
                    print(f"  [relay] Playwright: @{username} → {count:,} followers  ✓")
                    return count

            # Meta description fallback: "1.2M Followers, 500 Following, …"
            try:
                meta = page.locator('meta[name="description"]').get_attribute(
                    'content', timeout=2_000)
                if meta:
                    m = re.match(r'([\d,.]+\s*[KkMmBb]?)\s*[Ff]ollower', meta)
                    if m:
                        count = _parse_abbrev_count(m.group(1))
                        if count is not None:
                            print(f"  [relay] Playwright: @{username} → {count:,} "
                                  f"followers (meta)  ✓")
                            return count
            except Exception:
                pass

            print(f"  [relay] Playwright: follower count not found for @{username}")
            return None

    except Exception as exc:
        print(f"  [relay] Playwright Instagram @{username}: {exc}")
        return None


_insta_cache = {}   # {username: (follower_count, timestamp)}


def fetch_instagram_followers(username):
    """
    Return the follower count for an Instagram username, or None on failure.
    Results are cached for CACHE_TTL seconds.

    Fetch order:
        1. Playwright/Chromium — real browser; best bot-detection bypass.
        2. curl / urllib       — tries the public profile page directly.
    """
    with _cache_lock:
        entry = _insta_cache.get(username)
        if entry and time.time() - entry[1] < CACHE_TTL:
            return entry[0]

    # ── 1. Playwright ────────────────────────────────────────────────────────
    if _PLAYWRIGHT_AVAILABLE:
        try:
            count = _pw_call(_pw_fetch_instagram, username)
        except Exception as exc:
            print(f"  [relay] Playwright worker error for @{username}: {exc}")
            count = None
        if count is not None:
            with _cache_lock:
                _insta_cache[username] = (count, time.time())
            return count
        print(f"  [relay] Playwright failed for @{username}, trying curl/urllib…")

    # ── 2. curl / urllib ─────────────────────────────────────────────────────
    url = f"https://www.instagram.com/{username}/"
    extra = {"x-ig-app-id": "936619743392459"}
    try:
        if _CURL:
            body, status = _curl_get(url, extra_headers=extra)
        else:
            body = _http_get(url, extra_headers=extra)
            status = 200

        if status == 200 and body:
            for pat in (
                r'"edge_followed_by"\s*:\s*\{"count"\s*:\s*(\d+)',
                r'"follower_count"\s*:\s*(\d+)',
            ):
                m = re.search(pat, body)
                if m:
                    count = int(m.group(1))
                    print(f"  [relay] curl/urllib: @{username} → {count:,} followers  ✓")
                    with _cache_lock:
                        _insta_cache[username] = (count, time.time())
                    return count

            # Last-resort: meta description in raw HTML
            m = re.search(
                r'<meta[^>]+name=["\']description["\'][^>]+content=["\']'
                r'([\d,.]+\s*[KkMmBb]?)\s*[Ff]ollower',
                body)
            if m:
                count = _parse_abbrev_count(m.group(1))
                if count is not None:
                    print(f"  [relay] curl/urllib: @{username} → {count:,} "
                          f"followers (meta)  ✓")
                    with _cache_lock:
                        _insta_cache[username] = (count, time.time())
                    return count

        print(f"  [relay] Instagram: count not found for @{username} (HTTP {status})")
    except Exception as exc:
        print(f"  [relay] Instagram curl/urllib @{username}: {exc}")

    return None


# ── TikTok cookies ────────────────────────────────────────────────────────────
# TikTok's WAF checks for the *presence* and rough *format* of session cookies,
# not cryptographic validity, for public profile page loads.  We generate
# plausible values locally so no browser export is ever required.

def _generate_tiktok_cookies():
    """
    Build a Cookie header value with randomly-generated TikTok session tokens.

    Each value is generated to exactly match the format/length observed in
    real tiktok.com sessions:

      msToken       URL-safe base64(110 random bytes) — 148 chars, one = pad
      ttwid         1%7C<b64_32B>%7C<ts>%7C<64_hex>
      tt_csrf_token 32 random URL-safe characters
      tt_chain_token base64(12 random bytes) — 16 chars + == pad
      _waftokenid   base64( JSON{"v":{"a":b64,"b":ts,"c":b64},"s":b64} )
      x-web-secsdk-uid  UUID v4
      tt_webid_v2   19-digit device ID
    """
    ts = int(time.time())

    # msToken — 110 random bytes encodes to 148 base64 chars (1 trailing =)
    ms_token = base64.urlsafe_b64encode(os.urandom(110)).decode()

    # ttwid — percent-encoded compound token matching real format
    ttwid_b64 = base64.urlsafe_b64encode(os.urandom(32)).decode().rstrip("=")
    ttwid_hex = "".join(random.choices("0123456789abcdef", k=64))
    ttwid = f"1%7C{ttwid_b64}%7C{ts}%7C{ttwid_hex}"

    # tt_csrf_token — 32 URL-safe random chars
    tt_csrf = "".join(random.choices(string.ascii_letters + string.digits + "-_", k=32))

    # tt_chain_token — base64 of 12 bytes → "XXXX...==" (16 chars, 2-char pad)
    tt_chain = base64.b64encode(os.urandom(12)).decode()

    # _waftokenid — base64-encoded JSON matching observed real structure
    waf_payload = json.dumps({
        "v": {
            "a": base64.b64encode(os.urandom(24)).decode(),
            "b": ts,
            "c": base64.b64encode(os.urandom(24)).decode(),
        },
        "s": base64.b64encode(os.urandom(24)).decode(),
    }, separators=(",", ":"))
    waf_token = base64.b64encode(waf_payload.encode()).decode().rstrip("=")

    # x-web-secsdk-uid — UUID v4
    secsdk_uid = str(uuid.uuid4())

    # tt_webid_v2 — 19-digit numeric device ID
    tt_webid = "".join(random.choices(string.digits, k=19))

    return (
        f"msToken={ms_token}; "
        f"ttwid={ttwid}; "
        f"tt_csrf_token={tt_csrf}; "
        f"tt_chain_token={tt_chain}; "
        f"_waftokenid={waf_token}; "
        f"x-web-secsdk-uid={secsdk_uid}; "
        f"tt_webid_v2={tt_webid}"
    )

# Pre-generate once at startup; refreshed on each relay restart.
_auto_cookie_str = _generate_tiktok_cookies()

# ── In-memory caches ─────────────────────────────────────────────────────────
_cache      = {}          # TikTok:  {username:   (follower_count, timestamp)}
_yt_cache   = {}          # YouTube: {channel_id: (subscriber_count, timestamp)}
_cache_lock = threading.Lock()


def local_ip():
    """Best-guess local LAN IP (the address the ESP32 should use)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def _http_get(url, extra_headers=None, timeout=15):
    """
    Perform a GET request with realistic Chrome headers.
    Returns the decoded response body, or raises on error.
    Handles gzip/deflate decompression automatically.
    """
    headers = {
        "User-Agent":                _UA,
        # Accept gzip — "identity" is a bot tell that real browsers never send.
        "Accept-Encoding":           "gzip, deflate",
        "Accept":                    "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8",
        "Accept-Language":           "en-US,en;q=0.9",
        "Cache-Control":             "max-age=0",
        "Sec-Fetch-Dest":            "document",
        "Sec-Fetch-Mode":            "navigate",
        "Sec-Fetch-Site":            "none",
        "Sec-Fetch-User":            "?1",
        "Upgrade-Insecure-Requests": "1",
    }
    if extra_headers:
        headers.update(extra_headers)

    ctx = ssl.create_default_context()
    req = urllib.request.Request(url, headers=headers)
    resp_cm = urllib.request.urlopen(req, timeout=timeout, context=ctx)

    with resp_cm as resp:
        raw      = resp.read()
        encoding = resp.headers.get("Content-Encoding", "")

    if encoding == "gzip":
        try:
            raw = gzip.decompress(raw)
        except Exception:
            pass
    elif encoding == "deflate":
        import zlib
        try:
            raw = zlib.decompress(raw)
        except Exception:
            pass

    return raw.decode("utf-8", errors="ignore")


# Detect curl once at startup.
_CURL = shutil.which("curl")


def _curl_get(url, cookie_str=None, extra_headers=None, timeout=15):
    """
    Fetch a URL using the system curl binary.

    curl's native TLS stack (Schannel on Windows, OpenSSL/LibreSSL on
    macOS/Linux) presents a browser-like ClientHello that passes TikTok's
    JA3/JA4 fingerprint checks.  Python's ssl module is blocked regardless
    of headers or cookies.

    Returns (body_str, http_status) on success, raises RuntimeError on error.
    curl ships with Windows 10+, macOS, and all major Linux distros.
    """
    if not _CURL:
        raise RuntimeError("curl not available")

    cmd = [
        _CURL, "-s", "-L",
        "--max-time", str(timeout),
        "--compressed",           # decompress gzip/brotli automatically
        # Append the HTTP status as a sentinel after the body so we can
        # extract it without a separate HEAD request.
        "-w", "\n~~HTTPSTATUS~~%{http_code}~~",
        "-H", f"User-Agent: {_UA}",
        "-H", "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8",
        "-H", "Accept-Language: en-US,en;q=0.9",
        "-H", "Cache-Control: max-age=0",
        "-H", "Sec-Fetch-Dest: document",
        "-H", "Sec-Fetch-Mode: navigate",
        "-H", "Sec-Fetch-Site: none",
        "-H", "Sec-Fetch-User: ?1",
        "-H", "Upgrade-Insecure-Requests: 1",
    ]
    if extra_headers:
        for k, v in extra_headers.items():
            cmd += ["-H", f"{k}: {v}"]
    if cookie_str:
        cmd += ["-H", f"Cookie: {cookie_str}"]
    cmd.append(url)

    try:
        result = subprocess.run(
            cmd, capture_output=True, timeout=timeout + 5
        )
    except subprocess.TimeoutExpired:
        raise RuntimeError("curl timed out")

    if result.returncode not in (0, 22):   # 22 = HTTP error with --fail (not used here)
        raise RuntimeError(
            f"curl exited {result.returncode}: "
            f"{result.stderr[:120].decode('utf-8', errors='replace').strip()}"
        )

    # Decode body and extract the appended status sentinel.
    raw = result.stdout.decode("utf-8", errors="ignore")
    status = 0
    sentinel = "\n~~HTTPSTATUS~~"
    if sentinel in raw:
        body, status_str = raw.rsplit(sentinel, 1)
        try:
            status = int(status_str.replace("~~", "").strip())
        except ValueError:
            pass
    else:
        body = raw

    return body, status


def fetch_followers(username):
    """
    Return the follower count for a TikTok username, or None on failure.
    Results are cached for CACHE_TTL seconds.

    Fetch order:
        1. Playwright/Chromium — real browser; JS renders; best reliability.
        2. curl                — correct TLS fingerprint; no JS execution.
        3. urllib              — pure-Python fallback; may be WAF-blocked.
    """
    with _cache_lock:
        entry = _cache.get(username)
        if entry and time.time() - entry[1] < CACHE_TTL:
            return entry[0]

    # ── 1. Playwright ────────────────────────────────────────────────────────
    if _PLAYWRIGHT_AVAILABLE:
        try:
            count = _pw_call(_pw_fetch_tiktok, username)
        except Exception as exc:
            print(f"  [relay] Playwright worker error for @{username}: {exc}")
            count = None
        if count is not None:
            with _cache_lock:
                _cache[username] = (count, time.time())
            return count
        print(f"  [relay] Playwright failed for @{username}, trying curl/urllib…")

    # ── 2 & 3. curl / urllib ─────────────────────────────────────────────────
    def _fetch(url, extra_headers=None):
        """Try curl then urllib; return (body, status) or raise."""
        if _CURL:
            return _curl_get(url, cookie_str=_auto_cookie_str,
                             extra_headers=extra_headers)
        body = _http_get(url, extra_headers={
            **(extra_headers or {}),
            "Cookie": _auto_cookie_str,
        })
        return body, 200

    # Three attempts, each less fingerprinted than the last.
    attempts = [
        # 1. Desktop HTML profile page
        ("desktop HTML",  f"https://www.tiktok.com/@{username}",        None),
        # 2. Mobile HTML profile page — separate WAF rules from desktop
        ("mobile HTML",   f"https://m.tiktok.com/@{username}",          None),
        # 3. Internal JSON API — different path, returns JSON directly
        ("JSON API",
         f"https://www.tiktok.com/api/user/detail/?uniqueId={username}&aid=1988&app_name=tiktok_web&device_platform=web_pc",
         {"Accept": "*/*", "Referer": f"https://www.tiktok.com/@{username}"}),
    ]

    html   = None
    status = 0
    for label, url, hdrs in attempts:
        try:
            body, status = _fetch(url, extra_headers=hdrs)
        except Exception as exc:
            print(f"  [relay] {label} error for @{username}: {exc}")
            continue

        if not body:
            print(f"  [relay] {label} empty body for @{username} (HTTP {status})")
            continue
        if status not in (0, 200):
            print(f"  [relay] {label} HTTP {status} for @{username}")
            continue
        if "SlardarWAF" in body or len(body) < 500:
            print(f"  [relay] {label} WAF/challenge ({len(body)} chars) for @{username}")
            continue

        # Looks like real content — stop here.
        html = body
        break

    if not html:
        print(f"  [relay] All fetch attempts failed for @{username}")
        if not _PLAYWRIGHT_AVAILABLE:
            print(f"  [relay] Install Playwright for best results:")
            print(f"  [relay]   pip install playwright && playwright install chromium")
        return None

    # Match follower count from either source:
    #   HTML page  → "followerCount":N inside SIGI_STATE / __INIT_PROPS__
    #   JSON API   → {"userInfo":{"stats":{"followerCount":N}}}
    patterns = [
        r'"followerCount"\s*:\s*(\d+)',      # HTML page / JSON API stats block
        r'"follower_count"\s*:\s*(\d+)',     # older page variant
        r'"fans"\s*:\s*(\d+)',               # some localised variants
    ]
    for pat in patterns:
        m = re.search(pat, html)
        if m:
            count = int(m.group(1))
            print(f"  [relay] curl/urllib: @{username} → {count:,} followers  ✓")
            with _cache_lock:
                _cache[username] = (count, time.time())
            return count

    # Nothing matched but the page looks real — structure may have changed.
    print(f"  [relay] followerCount not found in TikTok response for @{username}")
    print(f"  [relay] Response length: {len(html)} chars")
    print(f"  [relay] First 500 chars: {html[:500]!r}")
    return None


def _parse_abbrev_count(text):
    """
    Convert YouTube's abbreviated subscriber string to an integer.

    Examples:
        "1.23K subscribers" -> 1230
        "123K subscribers"  -> 123000
        "1.2M subscribers"  -> 1200000
        "1.23B subscribers" -> 1230000000
        "500 subscribers"   -> 500
        "1,234"             -> 1234   (some locales use commas)
    Returns None if the string cannot be parsed.
    """
    if not text:
        return None
    m = re.match(r"([\d,.]+)\s*([KkMmBb]?)", text.strip())
    if not m:
        return None
    num_str = m.group(1).replace(",", "")
    suffix  = m.group(2).upper()
    try:
        num = float(num_str)
    except ValueError:
        return None
    multipliers = {"K": 1_000, "M": 1_000_000, "B": 1_000_000_000}
    return int(num * multipliers.get(suffix, 1))


def fetch_yt_subscribers(channel_id):
    """
    Return the subscriber count for a YouTube channel ID, or None on failure.
    Scrapes the public channel page (no API key required).
    Results are cached for CACHE_TTL seconds.

    Fetch order:
        1. Playwright/Chromium — real browser; avoids consent-wall blocks.
        2. urllib              — pure-Python; works on most networks.
    """
    with _cache_lock:
        entry = _yt_cache.get(channel_id)
        if entry and time.time() - entry[1] < CACHE_TTL:
            return entry[0]

    def _store(count):
        with _cache_lock:
            _yt_cache[channel_id] = (count, time.time())
        return count

    # ── 1. Playwright ────────────────────────────────────────────────────────
    if _PLAYWRIGHT_AVAILABLE:
        try:
            count = _pw_call(_pw_fetch_youtube, channel_id)
        except Exception as exc:
            print(f"  [relay] Playwright worker error for YouTube {channel_id}: {exc}")
            count = None
        if count is not None:
            return _store(count)
        print(f"  [relay] Playwright failed for YouTube {channel_id}, trying urllib…")

    # ── 2. urllib ────────────────────────────────────────────────────────────
    url = _yt_url(channel_id)
    try:
        html = _http_get(url, timeout=20)
    except Exception as exc:
        print(f"  [relay] YouTube fetch error for {channel_id}: {exc}")
        return None

    sub_text = None
    for pat, grp in (
        # 2024+ metadataParts format: "content":"N subscribers"
        (r'"content"\s*:\s*"([\d,.]+\.?\d*\s*[KkMmBb]?\s*subscriber[^"]*)"', 1),
        # Legacy: simpleText nested inside accessibility block
        (r'"subscriberCountText"(.{0,600}?)"simpleText"\s*:\s*"([^"]+)"', 2),
        # Legacy flat format: {"simpleText":"N subscribers"}
        (r'"subscriberCountText"\s*:\s*\{"simpleText"\s*:\s*"([^"]+)"', 1),
        # Raw numeric string (no suffix)
        (r'"subscriberCount"\s*:\s*"(\d+)"', 1),
    ):
        m = re.search(pat, html, re.DOTALL)
        if m:
            sub_text = m.group(grp)
            break

    if not sub_text:
        print(f"  [relay] subscriberCountText not found in YouTube response "
              f"for {channel_id}")
        print(f"  [relay] (YouTube may be serving a bot-challenge page — try again later)")
        return None

    count = _parse_abbrev_count(sub_text)
    if count is None:
        print(f"  [relay] Could not parse subscriber count string: '{sub_text}'")
        return None

    print(f"  [relay] urllib: {channel_id} → {count:,} subscribers  ✓")
    return _store(count)


class RelayHandler(BaseHTTPRequestHandler):
    """Minimal HTTP handler — /tiktok, /youtube, /health."""

    def do_GET(self):
        parsed = urlparse(self.path)
        qs     = parse_qs(parsed.query)

        if parsed.path == "/instagram":
            user = (qs.get("user") or [None])[0]
            if not user:
                self._send(400, b'{"error":"missing ?user= parameter"}')
                return
            count = fetch_instagram_followers(user)
            if count is None:
                self._send(503, b'{"error":"Instagram fetch failed - see relay console"}')
            else:
                body = json.dumps({"followers": count}).encode()
                self._send(200, body)

        elif parsed.path == "/tiktok":
            user = (qs.get("user") or [None])[0]
            if not user:
                self._send(400, b'{"error":"missing ?user= parameter"}')
                return
            count = fetch_followers(user)
            if count is None:
                # Return 503 so the ESP32 logs a warning instead of storing 0.
                self._send(503, b'{"error":"TikTok fetch failed - see relay console"}')
            else:
                body = json.dumps({"followers": count}).encode()
                self._send(200, body)

        elif parsed.path == "/youtube":
            channel = (qs.get("channel") or [None])[0]
            if not channel:
                self._send(400, b'{"error":"missing ?channel= parameter"}')
                return
            count = fetch_yt_subscribers(channel)
            if count is None:
                self._send(503, b'{"error":"YouTube fetch failed - see relay console"}')
            else:
                body = json.dumps({"subscribers": count}).encode()
                self._send(200, body)

        elif parsed.path == "/health":
            self._send(200, b"OK")

        else:
            self._send(404, b"Not found")

    def _send(self, code, body):
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    # Silence the default per-request access log line.
    def log_message(self, fmt, *args):  # noqa: ARG002
        pass


if __name__ == "__main__":
    # ThreadingHTTPServer (not plain HTTPServer): a Playwright fetch can take
    # up to ~30s, and a single-threaded server would block every other
    # request — including /health — for that whole duration. _pw_lock already
    # serialises actual browser use across threads, so this just lets
    # /health (and requests for a different, cached platform) respond
    # immediately instead of queuing behind a slow fetch.
    server = ThreadingHTTPServer(("0.0.0.0", PORT), RelayHandler)
    ip = local_ip()

    print("=" * 60)
    print(f"  Nextube social relay  v{__version__}  ->  http://{ip}:{PORT}")
    print("=" * 60)
    print(f'  Enter  "{ip}"  as the relay host')
    print("  in the Nextube web UI (Settings → Social Media Counters).")
    print()

    # Fetch engine status
    if _PLAYWRIGHT_AVAILABLE:
        # Trigger lazy init now so startup banner is accurate.  Routed
        # through _pw_call (not called directly) so the browser launch — and
        # so Playwright's greenlet thread affinity — lands on the dedicated
        # worker thread from the very first call, not this (main) thread.
        try:
            ok = _pw_call(_pw_ensure)
        except Exception as exc:
            print(f"  [relay] Playwright worker init error: {exc}")
            ok = False
        if ok:
            if _STEALTH_AVAILABLE:
                print("  Social Media fetcher : Playwright/Chromium  ✓  (stealth ✓)")
            else:
                print("  Social Media fetcher : Playwright/Chromium  ✓  (stealth DISABLED)")
                print(f"                         Reason: {_stealth_import_err}")
        else:
            print("  Social Media fetcher : Playwright installed but browser")
            print("                         failed to start — run:")
            print("                           playwright install chromium")
            if _CURL:
                print(f"                         Falling back to curl ({_CURL})")
            else:
                print("                         Falling back to urllib")
    else:
        if _CURL:
            print(f"  TikTok fetch  : curl ({_CURL})  ← correct TLS fingerprint")
        else:
            print(f"  TikTok fetch  : urllib (curl not found — TikTok may 403)")
        print()
        print("  Tip: install Playwright for best bot-detection bypass:")
        print("    pip install playwright && playwright install chromium")

    print()
    print("  Routes:")
    print(f"    /instagram?user=<username> ->  {{\"followers\": N}}")
    print(f"    /tiktok?user=<username>    ->  {{\"followers\": N}}")
    print(f"    /youtube?channel=<id>      ->  {{\"subscribers\": N}}")
    print(f"    /health                    ->  OK")
    print()
    print("  Results are cached for 5 minutes.")
    print("  Press Ctrl-C to stop.")
    print()

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nRelay stopped.")
        # On Windows, closing Chromium's pipe raises BrokenPipeError (WinError
        # 232) inside asyncio's proactor.  That exception is attached to an
        # asyncio Future which nothing ever retrieves, so asyncio's default
        # exception handler logs a "Future exception was never retrieved"
        # traceback via the "asyncio" logger — via Future.__del__, which
        # fires whenever the garbage collector happens to reclaim that
        # Future, not necessarily before _pw_pw.stop() below returns.
        # Redirecting stderr only around stop() (the previous approach) is
        # therefore unreliable — the GC pass, and the resulting log line,
        # can land after the redirect's `with` block has already exited.
        # Silencing the logger itself has no such timing dependency: it's
        # about to be process-exit anyway, so there's no need to restore it.
        import logging
        logging.getLogger("asyncio").setLevel(logging.CRITICAL)
        # Clean up the Playwright browser if it was started.  Routed through
        # _pw_call, same as every other Playwright call — closing from this
        # (main) thread instead of the dedicated worker thread would hit the
        # same "Cannot switch to a different thread" greenlet error.
        if _pw_browser is not None:
            try:
                _pw_call(_pw_browser.close, timeout=10)
            except Exception:
                pass
        if _pw_pw is not None:
            try:
                _pw_call(_pw_pw.stop, timeout=10)
            except Exception:
                pass
