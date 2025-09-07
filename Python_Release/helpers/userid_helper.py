from __future__ import annotations
# =========================
# Spacedrop Project
# Helpers (UserID)
# helpers/userid_helper.py
#
# Description:
#   Resolve Tailscale UserIDs from IP addresses.
# License: MIT
# =========================

# Imports
# =========================
import json
import ipaddress
from typing import Optional, Dict, Any, List
from helpers.shell_helper import run
# =========================


# --- userid_from_whois_json() ---
def userid_from_whois_json(ip: str) -> Optional[int]:
    """
        Preferred method to get the UserID from:
        `tailscale whois --json <ip>`
        - UserProfile.ID  (authoritative)
        - fallback Node.User
    """
    try:
        out = run(["tailscale", "whois", "--json", ip])
        obj = json.loads(out)
        uid = (obj.get("UserProfile", {}).get("ID") or obj.get("Node", {}).get("User"))
        return int(uid) if uid is not None else None
    except Exception:
        return None


# --- userid_from_status_by_ip() ---
def userid_from_status_by_ip(ip: str) -> Optional[int]:
    """
        Fallback: map sender IP → node in `tailscale status --json`,
        then read the node's UserID.
    """
    try:
        out = run(["tailscale", "status", "--json"])
        st = json.loads(out)
        peers: List[Dict[str, Any]] = list((st.get("Peer") or {}).values())
        self_node: Dict[str, Any] = st.get("Self") or {}
        for node in peers + ([self_node] if self_node else []):
            for a in node.get("TailscaleIPs", []):
                try:
                    if ipaddress.ip_address(ip) == ipaddress.ip_address(a):
                        uid = node.get("UserID")
                        return int(uid) if uid is not None else None
                except Exception:
                    continue
    except Exception:
        pass
    return None


# --- sender_userid() ---
def sender_userid(src_ip: str) -> Optional[int]:
    """
        Resolve sender's Tailscale UserID using whois JSON first,
        then fall back to status JSON.
    """
    return userid_from_whois_json(src_ip) or userid_from_status_by_ip(src_ip)
