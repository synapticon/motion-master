"""Shared helpers for the Motion Master Python examples.

Loads config.toml and builds an MMClient for the HTTP API. The host is kept
portless in config so both the HTTP API and the monitoring WebSocket can be
derived from it.
"""

from __future__ import annotations

import sys
import tomllib
from pathlib import Path

from mm_client import MMClient

HERE = Path(__file__).resolve().parent


def load_config():
    """Load config.toml."""
    path = HERE / "config.toml"
    if not path.exists():
        sys.exit(f"config.toml not found -- copy the template first:\n    cp {HERE / 'config.example.toml'} {path}")
    with open(path, "rb") as f:
        return tomllib.load(f)


def http_url(cfg):
    """HTTPS base URL for the request/response API, e.g. https://host:61447."""
    return f"https://{cfg['host']}:{cfg['http_port']}"


def ws_url(cfg):
    """WSS URL for the monitoring WebSocket, e.g. wss://host:62281."""
    return f"wss://{cfg['host']}:{cfg['ws_port']}"


def make_client(cfg=None):
    """Build an MMClient wired to both the HTTP API and the monitoring WebSocket."""
    cfg = cfg or load_config()
    return MMClient(http_url=http_url(cfg), ws_url=ws_url(cfg), verify=cfg.get("verify", False))
