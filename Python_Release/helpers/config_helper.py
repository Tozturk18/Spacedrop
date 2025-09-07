from __future__ import annotations
# =========================
# Spacedrop Project
# Helpers (Config)
# helpers/config_helper.py
#
# Description:
#   Manage configuration files and environment wiring.
# License: MIT
# =========================

# Imports
# =========================
import os
import json
from typing import Dict, Set, Any

from helpers.shell_helper import tailscale_ip4_self
from helpers.userid_helper import userid_from_whois_json, userid_from_status_by_ip
# =========================


# --- expand() ---
def expand(path: str) -> str:
    """Expand ~ in the given path."""
    return os.path.expanduser(path) if path else path


# --- parse_modes() ---
def parse_modes(env_val: str) -> Set[str]:
    """Parse VALID_MODES env var (comma-separated) into an uppercased set."""
    modes: Set[str] = set()
    for p in (env_val or "").split(","):
        p = p.strip()
        if p:
            modes.add(p.upper())
    return modes


# --- load_or_init_config() ---
def load_or_init_config(env: Dict[str, str], valid_modes_env: str) -> Dict[str, Any]:
    """
        Load ~/.config/Spacedrop/config.json if present; else create default:
          mode="PERSONAL"
          personal_user_id = whois(self IPv4)
          contacts_user_ids = []
        Returns a config dict with normalized values and
        _conf_dir/_conf_path for server reference.
    """
    conf_dir  = expand(env.get("CONF_DIR", "~/.config/Spacedrop"))
    conf_path = expand(env.get("CONF_PATH", "~/.config/Spacedrop/config.json"))
    os.makedirs(conf_dir, exist_ok=True)

    if os.path.isfile(conf_path):
        with open(conf_path, "r") as f:
            cfg = json.load(f)
    else:
        personal_uid = 0
        ip4 = tailscale_ip4_self()
        if ip4:
            uid = userid_from_whois_json(ip4) or userid_from_status_by_ip(ip4)
            if uid:
                personal_uid = uid
        cfg = {
            "mode": "PERSONAL",
            "personal_user_id": personal_uid,
            "contacts_user_ids": []
        }
        with open(conf_path, "w") as f:
            json.dump(cfg, f, indent=2)

    valid_modes = parse_modes(valid_modes_env) or {"EVERYONE","CONTACTS_ONLY","OFF","PERSONAL"}
    mode = str(cfg.get("mode", "PERSONAL")).upper()
    if mode not in valid_modes:
        mode = "PERSONAL"
    cfg["mode"] = mode

    def _as_int(x):
        try: return int(x)
        except Exception: return 0

    cfg["personal_user_id"] = _as_int(cfg.get("personal_user_id"))
    ids = cfg.get("contacts_user_ids") or []
    cfg["contacts_user_ids"] = sorted({ _as_int(v) for v in ids if _as_int(v) })

    cfg["_conf_dir"] = conf_dir
    cfg["_conf_path"] = conf_path
    return cfg
