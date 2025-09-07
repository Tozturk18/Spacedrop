from __future__ import annotations
# =========================
# Spacedrop Project
# Server Host
# server.py
#
# Created by Ozgur Tuna Ozturk on 06/09/2025.
# Version: 1.0 (iPhone to Mac, one-way)
#
# Description:
#   FastAPI application handling file uploads and clipboard updates
#   from iPhone devices over Tailscale VPN (one-way: iPhone -> Mac).
#
#   Features:
#   - Custom icon in notifications (via terminal-notifier when available)
#   - Accept/Decline confirmation dialog for foreign/unknown UserIDs
#   - Confirmation policy:
#       * Skip only for PERSONAL mode + sender == personal_user_id
#       * Otherwise, prompt (even in EVERYONE) if REQUIREconfirm_ON_FOREIGN=1
#   - NEW: AirDrop-like behavior for links:
#       * If POSTed 'text' is an http(s) URL → open immediately on Mac
#       * If uploaded file is .txt/.url/.webloc or simple .html with a URL → open
#       * Otherwise save file to Downloads (previous behavior)
# License: MIT
# =========================

# Imports
# =========================
import os
from datetime import datetime
from typing import Optional, Dict, Any
from urllib.parse import urlparse

from fastapi import FastAPI, UploadFile, File, HTTPException, Request, Form
from fastapi.responses import JSONResponse
from dotenv import load_dotenv

from helpers.shell_helper import (
    notify,
    confirm,
    expand,
    open_url,
    is_http_url,
    extract_url_from_html,
    unique_path,
)
from helpers.userid_helper import sender_userid
from helpers.config_helper import load_or_init_config, parse_modes
# =========================


# -------- load environment --------
load_dotenv()  # reads .env in current working dir
ENV: Dict[str, str] = dict(os.environ)

PORT = int(ENV.get("PORT", "8787"))
APP_TITLE = ENV.get("APP_TITLE", "Spacedrop Server (one-way)")

DOWNLOADS_DIR = expand(ENV.get("DOWNLOADS_DIR", "~/Downloads"))
CONF_DIR      = expand(ENV.get("CONF_DIR", "~/.config/Spacedrop"))
CONF_PATH     = expand(ENV.get("CONF_PATH", "~/.config/Spacedrop/config.json"))
VALID_MODES   = parse_modes(ENV.get("VALID_MODES", "EVERYONE,CONTACTS_ONLY,OFF,PERSONAL"))

# Custom icon + confirmation settings
ICON_PATH = expand(ENV.get("ICON_PATH", "~/Spacedrop/icon.png"))
REQUIREconfirm_ON_FOREIGN = ENV.get("REQUIREconfirm_ON_FOREIGN", "1") not in ("0","false","False","no","No")
APPROVAL_TIMEOUT = int(ENV.get("APPROVAL_TIMEOUT", "20"))

app = FastAPI(title=APP_TITLE)

# -------- config --------
CONFIG: dict[str, Any] = load_or_init_config(ENV, ENV.get("VALID_MODES", ""))


def _allowed(uid: Optional[int]) -> bool:
    """Check if the user is allowed to send based on mode & IDs."""
    mode = CONFIG["mode"]
    personal_uid = CONFIG["personal_user_id"]
    contacts = set(CONFIG["contacts_user_ids"])

    if mode == "OFF":
        return False
    if mode == "EVERYONE":
        return True
    if uid is None:
        return False
    if mode == "CONTACTS_ONLY":
        return (uid == personal_uid) or (uid in contacts)
    if mode == "PERSONAL":  # hidden mode
        return uid == personal_uid
    return False


def _maybeconfirm(uid: Optional[int]) -> bool:
    """
        Confirmation policy (with CONFIRM_ON_SELF toggle):
          • If REQUIREconfirm_ON_FOREIGN=0: never prompt.
          • If uid == personal_user_id and CONFIRM_ON_SELF=0: skip prompt (auto-accept) in all modes.
          • Else, if mode == PERSONAL and uid == personal_user_id: skip prompt (auto-accept).
          • Otherwise: prompt (EVERYONE / CONTACTS_ONLY / PERSONAL-from-others).
    """
    if not REQUIREconfirm_ON_FOREIGN:
        return True

    personal_uid = int(CONFIG.get("personal_user_id", 0) or 0)
    mode = CONFIG.get("mode", "PERSONAL").upper()

    # allow skipping confirmation for your own devices in all modes (default)
    confirm_on_self = os.getenv("CONFIRM_ON_SELF", "0") not in ("0", "false", "False", "no", "No")
    if uid is not None and int(uid) == personal_uid and not confirm_on_self:
        return True

    if mode == "PERSONAL" and uid is not None and int(uid) == personal_uid:
        return True

    msg = "Unknown sender (no UserID). Accept incoming item?" if uid is None else f"Incoming item from UserID {uid}. Accept?"
    return confirm("Spacedrop", msg, ICON_PATH, APPROVAL_TIMEOUT)


# -------- API --------
@app.get("/health")
def health():
    """Check server health/configuration."""
    return {
        "ok": True,
        "mode": CONFIG["mode"],
        "personal_user_id": CONFIG["personal_user_id"],
        "contacts_user_ids": CONFIG["contacts_user_ids"],
        "downloads_dir": DOWNLOADS_DIR,
        "config_path": CONF_PATH,
        "app_title": APP_TITLE,
        "requireconfirm_on_foreign": REQUIREconfirm_ON_FOREIGN,
        "approval_timeout": APPROVAL_TIMEOUT,
    }


@app.get("/debug/whoami")
def debug_whoami(request: Request):
    """Identify the calling peer by Tailscale UserID."""
    src_ip = request.client.host
    uid = sender_userid(src_ip)
    return {"src_ip": src_ip, "user_id": uid}


@app.post("/admin/reload-config")
def reload_config():
    """Reload configuration from disk."""
    global CONFIG
    CONFIG = load_or_init_config(ENV, ENV.get("VALID_MODES", ""))
    return {"ok": True, "mode": CONFIG["mode"], "personal_user_id": CONFIG["personal_user_id"]}


# ---- File drop OR link open (AirDrop-like) ----
@app.post("/drop")
async def drop(
    request: Request,
    text: str | None = Form(default=None),
    file: UploadFile | None = File(default=None),
):
    """
    Accept either:
      - form field 'text' (could be a URL or plain text)
      - a file upload (could be a real file, a .txt with a URL, a .webloc/.url, or a simple .html page)
    Behavior:
      - If we detect an http(s) URL, open it immediately and return ok.
      - Else, save the file to Downloads (original behavior).
    """
    src_ip = request.client.host
    uid = sender_userid(src_ip)

    if not _allowed(uid):
        raise HTTPException(status_code=401, detail=f"Sender not allowed (UserID={uid}) in mode {CONFIG['mode']}")

    if not _maybeconfirm(uid):
        raise HTTPException(status_code=403, detail="Declined by user (confirmation dialog)")

    # Case A: explicit text field (Shortcut sends shared URL as text)
    if text:
        url = is_http_url(text)
        if url:
            opened = open_url(url)
            host = urlparse(url).netloc or url
            notify("Opening link", host, icon_path=ICON_PATH)
            return JSONResponse({"ok": True, "action": "opened_url", "url": url, "opened": bool(opened)})

    # Case B: file upload (e.g., .webloc, .url, .txt with URL, or basic .html)
    if file:
        name = (file.filename or "").lower()

        data = await file.read()  # need content to inspect; file sizes from share sheets are tiny
        # Try URL wrappers first
        if name.endswith((".txt", ".url", ".webloc")):
            candidate = data.decode("utf-8", "ignore").strip()
            url = is_http_url(candidate)
            if url:
                opened = open_url(url)
                host = urlparse(url).netloc or url
                notify("Opening link", host, icon_path=ICON_PATH)
                return JSONResponse({"ok": True, "action": "opened_url", "url": url, "opened": bool(opened)})

        if name.endswith((".html", ".htm")):
            url = extract_url_from_html(data.decode("utf-8", "ignore"))
            if url:
                opened = open_url(url)
                host = urlparse(url).netloc or url
                notify("Opening link", host, icon_path=ICON_PATH)
                return JSONResponse({"ok": True, "action": "opened_url", "url": url, "opened": bool(opened)})

        # Otherwise treat it as a normal file (save to Downloads)
        os.makedirs(DOWNLOADS_DIR, exist_ok=True)
        base = os.path.basename(file.filename or "untitled")
        out_path = unique_path(DOWNLOADS_DIR, base)
        with open(out_path, "wb") as f:
            f.write(data)


        notify("Saved to Downloads", base, icon_path=ICON_PATH)
        return JSONResponse({"ok": True, "saved_as": out_path, "mode": CONFIG["mode"], "user_id": uid})

    # Nothing provided
    raise HTTPException(status_code=400, detail="No content provided")


# ---- Clipboard: push (phone→Mac) ----
@app.post("/clip/push")
async def clip_push(request: Request,
                    kind: str = Form("text"),
                    text: str = Form(None),
                    image: UploadFile = File(None)):
    """Push clipboard content from the phone to the Mac."""
    src_ip = request.client.host
    uid = sender_userid(src_ip)

    if not _allowed(uid):
        raise HTTPException(status_code=401, detail=f"Sender not allowed (UserID={uid}) in mode {CONFIG['mode']}")

    if not _maybeconfirm(uid):
        raise HTTPException(status_code=403, detail="Declined by user (confirmation dialog)")

    kind = (kind or "text").lower()

    if kind == "text":
        if text is None:
            raise HTTPException(status_code=422, detail="Missing 'text' for kind=text")
        import subprocess
        try:
            subprocess.run(["pbcopy"], input=text, text=True, check=False)
        except Exception as e:
            raise HTTPException(status_code=500, detail=f"Failed to set clipboard text: {e}")
        notify("Clipboard updated", "Text from device", icon_path=ICON_PATH)
        return {"ok": True, "kind": "text"}

    elif kind == "image":
        if image is None:
            raise HTTPException(status_code=422, detail="Missing 'image' for kind=image")
        import tempfile
        try:
            from AppKit import NSPasteboard, NSImage
            from Foundation import NSData
        except Exception:
            raise HTTPException(status_code=500, detail="PyObjC needed for image clipboard (pip install pyobjc)")

        suffix = os.path.splitext(image.filename or "clip.png")[1] or ".png"
        with tempfile.NamedTemporaryFile(suffix=suffix, delete=False) as tf:
            while True:
                chunk = await image.read(1024 * 1024)
                if not chunk:
                    break
                tf.write(chunk)
            tmp_path = tf.name
        try:
            pb = NSPasteboard.generalPasteboard()
            data = NSData.dataWithContentsOfFile_(tmp_path)
            if data is None:
                raise RuntimeError("No image data")
            img = NSImage.alloc().initWithData_(data)
            if img is None:
                raise RuntimeError("Invalid image")
            pb.clearContents()
            ok = pb.writeObjects_([img])
            if not ok:
                raise RuntimeError("Pasteboard write failed")
        except Exception as e:
            try: os.unlink(tmp_path)
            except Exception: pass
            raise HTTPException(status_code=500, detail=f"Failed to set clipboard image: {e}")
        try: os.unlink(tmp_path)
        except Exception: pass

        notify("Clipboard updated", "Image from device", icon_path=ICON_PATH)
        return {"ok": True, "kind": "image"}

    else:
        raise HTTPException(status_code=400, detail="Unsupported kind (use 'text' or 'image')" )


# -------- Entrypoint (uvicorn) --------
if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=PORT)
