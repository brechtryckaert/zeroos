#!/usr/bin/env python3
"""MeshCore test mesh for the ZeroOS simulator.

Runs three real pyMC/openHop companion nodes on an in-memory "ether" instead
of a radio. Node A ("ZeroOS") exposes the standard MeshCore companion frame
protocol on TCP, exactly like the openHop repeater's companion does on the
device, so the ZeroOS MeshCore app can connect to it unchanged.

The other two nodes are scripted:
  * "Ridge Hiker"  - adverts, sends A a direct message, echoes DMs back
  * "Harbor Base"  - adverts and chats on the Public channel

Usage:
    python -m venv venv && venv/bin/pip install pymc-core
    venv/bin/python tools/sim/meshcore_sim.py [--port 5000] [--quiet]
"""
import argparse
import asyncio
import hashlib
import logging
import random
import time

from openhop_core.companion.companion_radio import CompanionRadio
from openhop_core.companion.frame_server import CompanionFrameServer
from openhop_core.protocol import LocalIdentity

PUBLIC_SECRET = bytes.fromhex("8b3387e9c5cdea6ac9e5edbaa115cd72")


class Ether:
    """Every radio hears every other radio, with a little delay and noise."""

    def __init__(self):
        self.radios = []

    def attach(self, radio):
        self.radios.append(radio)


class EtherRadio:
    def __init__(self, ether, name, snr):
        self.ether = ether
        self.name = name
        self.snr = snr
        self.rx_callback = None
        ether.attach(self)

    # --- LoRaRadio surface used by the dispatcher ---
    def begin(self):
        return True

    def set_rx_callback(self, cb):
        self.rx_callback = cb

    async def send(self, data: bytes):
        for other in self.ether.radios:
            if other is self or other.rx_callback is None:
                continue
            asyncio.get_running_loop().call_later(
                0.15 + random.random() * 0.1, self._deliver, other, bytes(data))
        return {}

    @staticmethod
    def _deliver(other, data):
        snr = other.snr + random.uniform(-1.5, 1.5)
        try:
            other.rx_callback(data, int(-60 - (10 - snr) * 3), snr)
        except TypeError:
            other.rx_callback(data)

    async def wait_for_rx(self):
        await asyncio.sleep(3600)

    def sleep(self):
        pass

    def get_last_rssi(self):
        return -75

    def get_last_snr(self):
        return self.snr


def make_node(ether, name, snr):
    # Fixed keys per name, so contacts stay the same node across sim restarts.
    seed = hashlib.sha256(b"zeroos-sim:" + name.encode()).digest()
    node = CompanionRadio(EtherRadio(ether, name, snr), LocalIdentity(seed=seed), node_name=name)
    if node.get_channel(0) is None:
        node.set_channel(0, "Public", PUBLIC_SECRET)
    return node


async def main(port, quiet):
    logging.basicConfig(level=logging.WARNING if quiet else logging.INFO,
                        format="%(asctime)s %(name)s: %(message)s")
    log = logging.getLogger("sim")
    ether = Ether()
    a = make_node(ether, "ZeroOS", 9.0)
    b = make_node(ether, "Ridge Hiker", 6.5)
    c = make_node(ether, "Harbor Base", 2.0)
    for n in (a, b, c):
        await n.start()

    server = CompanionFrameServer(a, a.get_public_key()[:1].hex(), port=port, bind_address="127.0.0.1",
                                  device_model="ZeroOS sim", client_idle_timeout_sec=None)
    await server.start()
    log.warning("MeshCore sim: companion for 'ZeroOS' on 127.0.0.1:%d", port)

    # Ridge Hiker echoes direct messages.
    async def on_dm(event):
        text = getattr(event, "text", "")
        log.warning("Ridge Hiker got DM: %r", text)
        await asyncio.sleep(1.0)
        await b.send_text_message(a.get_public_key(), f"Copy that: {text}", wait_for_ack=False)

    b.on_message_event(on_dm)

    # Everyone introduces themselves so contacts get created.
    for n in (b, c, a):
        await n.advertise(flood=True)
        await asyncio.sleep(0.6)
    await asyncio.sleep(1.0)

    await c.send_channel_message(0, "Morning all, mesh check from the harbour. Anyone up on the ridge?")
    await asyncio.sleep(2.0)
    await b.send_channel_message(0, "Loud and clear, 9 km out on the ridge trail.")
    await asyncio.sleep(1.5)
    await b.send_text_message(a.get_public_key(), "Hi! Saw your advert. Is this the Cardputer?",
                              wait_for_ack=False)

    chatter = [
        "Wind picking up here, heading back down.",
        "Battery at 60 percent, solar is doing its job.",
        "Weather looks good for tomorrow.",
    ]
    i = 0
    while True:
        await asyncio.sleep(20)
        await c.send_channel_message(0, chatter[i % len(chatter)])
        i += 1


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=5000)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()
    try:
        asyncio.run(main(args.port, args.quiet))
    except KeyboardInterrupt:
        pass
