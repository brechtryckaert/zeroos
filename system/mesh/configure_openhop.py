#!/usr/bin/env python3
"""Point an openHop (pyMC) MeshCore repeater at the Cap LoRa-1262 and give
ZeroOS a companion to talk to.

Edits /etc/openhop_repeater/config.yaml in place (a .bak copy is kept):
  * radio_type sx1262 on spidev0.1 with the CardputerZero cap pins
  * DIO2 RF switch, 1.8 V TCXO on DIO3
  * a radio preset (default: EU/UK Narrow, 869.618 MHz SF8 BW62.5 CR8)
  * repeater mode "monitor": a handheld should not repeat by default
  * one companion identity "zeroos" on 127.0.0.1:5000 (key generated once)

Run with the repeater's own Python so PyYAML is available:
  sudo /opt/openhop_repeater/venv/bin/python configure_openhop.py --name ZeroOS
"""
import argparse
import os
import secrets
import shutil
import socket

import yaml

PRESETS = {
    # name: (frequency Hz, bandwidth Hz, SF, CR) — from MeshCore's suggested presets
    "eu-narrow": (869618000, 62500, 8, 8),
    "eu-legacy": (869525000, 250000, 11, 5),
    "nl": (869618000, 62500, 7, 5),
    "us": (910525000, 62500, 7, 5),
    "anz": (915800000, 250000, 10, 5),
}

CAP_PINS = {
    "bus_id": 0,
    "cs_id": 1,          # SPI0 CE1 = GPIO7 = cap NSS
    "cs_pin": -1,        # hardware chip select
    "reset_pin": 26,
    "busy_pin": 22,
    "irq_pin": 23,       # DIO1
    "txen_pin": -1,
    "rxen_pin": -1,
    "en_pin": -1,
    "txled_pin": -1,
    "rxled_pin": -1,
    "use_dio3_tcxo": True,
    "dio3_tcxo_voltage": 1.8,
    "use_dio2_rf": True,
    "is_waveshare": False,
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default="/etc/openhop_repeater/config.yaml")
    ap.add_argument("--name", default=socket.gethostname())
    ap.add_argument("--preset", choices=sorted(PRESETS), default="eu-narrow")
    ap.add_argument("--tx-power", type=int, default=22)
    ap.add_argument("--mode", choices=["monitor", "forward", "no_tx"], default="monitor")
    ap.add_argument("--port", type=int, default=5000)
    a = ap.parse_args()

    with open(a.config) as f:
        cfg = yaml.safe_load(f) or {}
    shutil.copy2(a.config, a.config + ".bak")

    rep = cfg.setdefault("repeater", {}) or {}
    cfg["repeater"] = rep
    rep["node_name"] = f"{a.name}-rpt"
    rep["mode"] = a.mode

    cfg["radio_type"] = "sx1262"
    freq, bw, sf, cr = PRESETS[a.preset]
    radio = cfg.setdefault("radio", {}) or {}
    cfg["radio"] = radio
    radio.update(frequency=freq, bandwidth=bw, spreading_factor=sf, coding_rate=cr, tx_power=a.tx_power)
    sx = cfg.setdefault("sx1262", {}) or {}
    cfg["sx1262"] = sx
    sx.update(CAP_PINS)

    ids = cfg.setdefault("identities", {}) or {}
    cfg["identities"] = ids
    companions = ids.get("companions") or []
    mine = next((c for c in companions if c.get("name") == "zeroos"), None)
    if mine is None:
        mine = {"name": "zeroos", "identity_key": secrets.token_hex(32), "settings": {}}
        companions.append(mine)
    mine.setdefault("identity_key", secrets.token_hex(32))
    mine["settings"] = {
        **(mine.get("settings") or {}),
        "node_name": a.name,
        "tcp_port": a.port,
        "bind_address": "127.0.0.1",   # only ZeroOS on this device
        "tcp_timeout": 0,              # ZeroOS keeps one long-lived connection
    }
    ids["companions"] = companions

    tmp = a.config + ".tmp"
    with open(tmp, "w") as f:
        yaml.safe_dump(cfg, f, sort_keys=False, allow_unicode=True)
    os.replace(tmp, a.config)
    print(f"configured {a.config}: {a.preset} {freq/1e6:.3f} MHz SF{sf} BW{bw/1000:g} CR{cr}, "
          f"mode {a.mode}, companion '{a.name}' on 127.0.0.1:{a.port}")


if __name__ == "__main__":
    main()
