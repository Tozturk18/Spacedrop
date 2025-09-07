from __future__ import annotations
# =========================
# Spacedrop Project
# Helpers (shell)
# helpers/shell_helper.py
#
# Description:
#   Helper functions that run shell commands (e.g., tailscale, osascript).
#   Adds custom-icon notifications and Accept/Decline confirmation dialog.
#   NEW: URL helpers and GUI opener for AirDrop-like link handling.
# License: MIT
# =========================

# Imports
# =========================
import os
import re
import shutil
import subprocess
import ipaddress
from urllib.parse import urlparse
from typing import List, Optional
# =========================


# --- _run() ---
def _run(cmd: List[str]) -> str:
    """
        Run a shell command and return stdout (stripped).
        Raises CalledProcessError on failure.
    """
    return subprocess.check_output(cmd, text=True).strip()


# --- _expand() ---
def _expand(path: str | None) -> str | None:
    """Expand ~ in the given path (or return None if path is None)."""
    return os.path.expanduser(path) if path else path


# --- _notify() ---
def _notify(title: str, text: str, icon_path: Optional[str] = None) -> None:
    """
        macOS notification with optional custom icon.
        Tries terminal-notifier for custom icon; falls back to AppleScript.
        Parameters:
            - title (str): Notification title
            - text (str): Notification body
            - icon_path (Optional[str]): Path to PNG/ICNS icon (optional)
    """
    try:
        tn = shutil.which("terminal-notifier")
        icon = _expand(icon_path) if icon_path else None

        if tn:
            cmd = [tn, "-title", title, "-message", text]
            if icon and os.path.exists(icon):
                cmd += ["-appIcon", icon]
            subprocess.run(cmd, check=False)
            return

        # Fallback: AppleScript (no custom icon support)
        subprocess.run([
            "osascript", "-e",
            f'display notification "{text.replace("\\","\\\\").replace("\"","\\\"")}" with title "{title}"'
        ], check=False)
    except Exception:
        # Non-fatal: notifications should not crash the server
        pass


# --- _confirm() ---
def _confirm(title: str, text: str, icon_path: Optional[str] = None, timeout_sec: int = 20) -> bool:
    """
        Blocking Accept/Decline prompt (AppleScript dialog).
        Returns True on Accept, False on Decline/timeout/error.
        Notes:
          • timeout_sec=0 means "no timeout".
          • Supports custom icon (PNG/ICNS) when provided.
    """
    icon = _expand(icon_path) if icon_path else None

    def esc(s: str) -> str:
        # Escape backslashes and double-quotes for AppleScript string literals
        return s.replace("\\", "\\\\").replace("\"", "\\\"")

    tsec = max(0, int(timeout_sec))
    lines = []
    lines.append('set theButtons to {"Decline","Accept"}')
    lines.append(f'set theTitle to "{esc(title)}"')
    lines.append(f'set theText to "{esc(text)}"')
    lines.append(f'set timeoutSeconds to {tsec}')

    if icon and os.path.exists(icon):
        lines.append(f'set theIcon to POSIX file "{esc(icon)}"')

    # Build a robust dialog call
    lines.append('try')
    if icon and os.path.exists(icon):
        if tsec > 0:
            lines.append('  display dialog theText with title theTitle buttons theButtons default button "Accept" with icon theIcon giving up after timeoutSeconds')
        else:
            lines.append('  display dialog theText with title theTitle buttons theButtons default button "Accept" with icon theIcon')
    else:
        if tsec > 0:
            lines.append('  display dialog theText with title theTitle buttons theButtons default button "Accept" giving up after timeoutSeconds')
        else:
            lines.append('  display dialog theText with title theTitle buttons theButtons default button "Accept"')

    lines.append('  set btn to button returned of result')
    lines.append('  set gu to false')
    lines.append('  try')
    lines.append('    set gu to gave up of result')
    lines.append('  end try')
    lines.append('on error')
    lines.append('  return "decline"')
    lines.append('end try')

    lines.append('if gu then return "decline"')
    lines.append('if btn is "Accept" then return "accept"')
    lines.append('return "decline"')

    script = "\n".join(lines)

    try:
        out = subprocess.check_output(["osascript", "-e", script], text=True).strip()
        return out.lower() == "accept"
    except subprocess.CalledProcessError:
        return False


# --- URL helpers (NEW) ---
_URL_RE = re.compile(r'^(https?://\S+)$', re.IGNORECASE)

def _is_http_url(s: str | None) -> Optional[str]:
    """Return normalized http(s) URL if valid, else None."""
    if not s:
        return None
    s = s.strip()
    if not _URL_RE.match(s):
        return None
    p = urlparse(s)
    if p.scheme in ("http", "https") and p.netloc:
        return s
    return None

def _extract_url_from_html(html_text: str | None) -> Optional[str]:
    """
    Extract a likely destination URL from simple share HTML files.
    Supports:
      - <meta http-equiv="refresh" content="0; url=...">
      - A single <a href="..."> link
    """
    if not html_text:
        return None
    # meta refresh
    m = re.search(r'http-equiv=["\']refresh["\'].*?url=([^"\'> ]+)', html_text, re.I)
    if m:
        url = _is_http_url(m.group(1))
        if url:
            return url
    # anchor tag
    m = re.search(r'<a[^>]+href=["\']([^"\']+)["\']', html_text, re.I)
    if m:
        url = _is_http_url(m.group(1))
        if url:
            return url
    return None

def _open_url(url: str) -> bool:
    """
    Open a URL in the logged-in user's GUI session.
    Tries 'open' first (works if server runs in user session).
    Falls back to launchctl asuser with the current uid.
    Returns True if the 'open' command executed without raising.
    """
    try:
        # Primary path
        r = subprocess.run(["/usr/bin/open", url], check=False)
        if r.returncode == 0:
            return True
    except Exception:
        pass
    # Fallback: attempt launchctl asuser with current uid (best effort)
    try:
        uid = os.getuid()
        r = subprocess.run(["launchctl", "asuser", str(uid), "/usr/bin/open", url], check=False)
        return r.returncode == 0
    except Exception:
        return False


# --- _tailscale_ip4_self() ---
def _tailscale_ip4_self() -> Optional[str]:
    """
        Get the first IPv4 address of the current Tailscale node.
        Returns None if not found.
    """
    try:
        out = _run(["tailscale", "ip", "-4"])
        for line in out.splitlines():
            s = line.strip()
            try:
                ipaddress.IPv4Address(s)
                return s
            except Exception:
                continue
    except Exception:
        pass
    return None

import os

def _unique_path(directory: str, filename: str) -> str:
    """
    Return a non-conflicting path inside `directory` for `filename`.
    If the file exists, appends ' (1)', ' (2)', ... before the extension.

    Example:
        foo.txt -> foo.txt
        foo.txt (already exists) -> foo (1).txt
        foo (1).txt (also exists) -> foo (2).txt
    """
    base, ext = os.path.splitext(filename)
    candidate = os.path.join(directory, filename)
    i = 1
    while os.path.exists(candidate):
        candidate = os.path.join(directory, f"{base} ({i}){ext}")
        i += 1
    return candidate
