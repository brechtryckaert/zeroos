#!/usr/bin/env python3
"""A stand-in for meshtasticd's TCP API, for the ZeroOS simulator.

Speaks the real Meshtastic stream protocol (0x94 0xC3 + 16-bit length +
protobuf) on port 4403 using the official generated protobufs, so it also
checks ZeroOS's own protobuf encoder/decoder. It is not a mesh simulator:
it plays a node that has heard a few neighbours, injects some traffic and
acknowledges what you send.

Usage:
    python -m venv venv && venv/bin/pip install meshtastic
    venv/bin/python tools/sim/meshtastic_sim.py [--port 4403]
"""
import argparse
import asyncio
import random
import struct
import time

from meshtastic.protobuf import (channel_pb2, config_pb2, mesh_pb2, portnums_pb2)

BROADCAST = 0xFFFFFFFF
ME = 0x5A3C19E0

NEIGHBOURS = [
    # num, long name, short, snr, hops, minutes since heard
    (0x8E21A4C7, "Ridge Hiker", "RDGE", 6.5, 0, 1),
    (0x4B10F203, "Harbor Base", "HRBR", 2.0, 1, 4),
    (0x9C7740D1, "Solar Repeater 3", "SR3", -4.25, 2, 12),
]


def frame(msg) -> bytes:
    data = msg.SerializeToString()
    return bytes([0x94, 0xC3]) + struct.pack(">H", len(data)) + data


class Session:
    def __init__(self, reader, writer, verbose):
        self.r, self.w, self.verbose = reader, writer, verbose
        self.next_id = random.randint(1, 1 << 30)
        self.alive = True

    def log(self, *a):
        if self.verbose:
            print("[mt-sim]", *a, flush=True)

    def send(self, fr):
        self.next_id += 1
        fr.id = self.next_id
        self.w.write(frame(fr))

    def text_packet(self, frm, to, channel, text):
        fr = mesh_pb2.FromRadio()
        p = fr.packet
        setattr(p, "from", frm)
        p.to = to
        p.channel = channel
        p.id = random.randint(1, 0xFFFFFFFF)
        p.rx_time = int(time.time())
        p.rx_snr = random.uniform(-3, 9)
        p.hop_limit = 3
        p.hop_start = 3
        p.decoded.portnum = portnums_pb2.TEXT_MESSAGE_APP
        p.decoded.payload = text.encode()
        return fr

    def routing_ack(self, frm, request_id, error=0):
        fr = mesh_pb2.FromRadio()
        p = fr.packet
        setattr(p, "from", frm)
        p.to = ME
        p.id = random.randint(1, 0xFFFFFFFF)
        p.rx_time = int(time.time())
        p.decoded.portnum = portnums_pb2.ROUTING_APP
        p.decoded.request_id = request_id
        r = mesh_pb2.Routing()
        r.error_reason = error
        p.decoded.payload = r.SerializeToString()
        return fr

    async def send_config(self, nonce):
        fr = mesh_pb2.FromRadio()
        fr.my_info.my_node_num = ME
        self.send(fr)

        def node(num, long, short, snr, hops, mins):
            fr = mesh_pb2.FromRadio()
            n = fr.node_info
            n.num = num
            n.user.id = f"!{num:08x}"
            n.user.long_name = long
            n.user.short_name = short
            n.user.hw_model = mesh_pb2.HardwareModel.PORTDUINO
            if num != ME:
                n.snr = snr
                n.last_heard = int(time.time()) - mins * 60
                n.hops_away = hops
            self.send(fr)

        node(ME, "ZeroOS Cardputer", "ZERO", 0, 0, 0)
        for nb in NEIGHBOURS:
            node(*nb)

        fr = mesh_pb2.FromRadio()
        lora = fr.config.lora
        lora.use_preset = True
        lora.modem_preset = config_pb2.Config.LoRaConfig.LONG_FAST
        lora.region = config_pb2.Config.LoRaConfig.EU_868
        lora.tx_enabled = True
        lora.hop_limit = 3
        self.send(fr)

        for idx, name, role in ((0, "", channel_pb2.Channel.PRIMARY),
                                (1, "Hiking", channel_pb2.Channel.SECONDARY),
                                (2, "", channel_pb2.Channel.DISABLED)):
            fr = mesh_pb2.FromRadio()
            fr.channel.index = idx
            fr.channel.role = role
            fr.channel.settings.name = name
            fr.channel.settings.psk = b"\x01"
            self.send(fr)

        fr = mesh_pb2.FromRadio()
        fr.config_complete_id = nonce
        self.send(fr)
        await self.w.drain()
        self.log("config sent")
        asyncio.create_task(self.script())

    async def script(self):
        steps = [
            (1.5, NEIGHBOURS[1][0], BROADCAST, 0, "Morning all, mesh check from the harbour."),
            (2.0, NEIGHBOURS[0][0], BROADCAST, 0, "Loud and clear from the ridge trail."),
            (1.5, NEIGHBOURS[0][0], ME, 0, "Hi! Is that the new Cardputer node?"),
            (2.5, NEIGHBOURS[0][0], BROADCAST, 1, "Trail is muddy past the second bridge."),
        ]
        for delay, frm, to, ch, text in steps:
            await asyncio.sleep(delay)
            if not self.alive:
                return
            self.send(self.text_packet(frm, to, ch, text))
            await self.w.drain()
        i = 0
        chatter = ["Battery at 60 percent, solar holding up.", "Wind picking up, heading down."]
        while self.alive:
            await asyncio.sleep(25)
            self.send(self.text_packet(NEIGHBOURS[1][0], BROADCAST, 0, chatter[i % 2]))
            await self.w.drain()
            i += 1

    async def handle_packet(self, p):
        if p.decoded.portnum != portnums_pb2.TEXT_MESSAGE_APP:
            return
        text = p.decoded.payload.decode(errors="replace")
        self.log(f"tx id={p.id:#x} to={p.to:#x} ch={p.channel} want_ack={p.want_ack}: {text!r}")
        await asyncio.sleep(0.8)
        if p.to == BROADCAST:
            # implicit ACK: we heard a neighbour rebroadcast it
            self.send(self.routing_ack(ME, p.id))
            await self.w.drain()
            return
        known = {n[0] for n in NEIGHBOURS}
        if p.to not in known:
            self.send(self.routing_ack(ME, p.id, mesh_pb2.Routing.MAX_RETRANSMIT))
            await self.w.drain()
            return
        self.send(self.routing_ack(p.to, p.id))
        await self.w.drain()
        await asyncio.sleep(1.2)
        self.send(self.text_packet(p.to, ME, 0, f"Copy that: {text}"))
        await self.w.drain()

    async def run(self):
        buf = b""
        try:
            while True:
                data = await self.r.read(4096)
                if not data:
                    break
                buf += data
                while True:
                    i = buf.find(b"\x94\xc3")
                    if i < 0:
                        buf = buf[-1:]
                        break
                    if len(buf) < i + 4:
                        break
                    n = struct.unpack(">H", buf[i + 2:i + 4])[0]
                    if len(buf) < i + 4 + n:
                        break
                    msg = mesh_pb2.ToRadio()
                    msg.ParseFromString(buf[i + 4:i + 4 + n])
                    buf = buf[i + 4 + n:]
                    kind = msg.WhichOneof("payload_variant")
                    if kind == "want_config_id":
                        await self.send_config(msg.want_config_id)
                    elif kind == "packet":
                        asyncio.create_task(self.handle_packet(msg.packet))
                    elif kind == "heartbeat":
                        self.log("heartbeat")
                    elif kind == "disconnect":
                        return
        finally:
            self.alive = False
            self.w.close()


async def main(port, verbose):
    async def on_conn(r, w):
        print("[mt-sim] client connected", flush=True)
        await Session(r, w, verbose).run()
        print("[mt-sim] client gone", flush=True)

    server = await asyncio.start_server(on_conn, "127.0.0.1", port)
    print(f"[mt-sim] meshtasticd stand-in on 127.0.0.1:{port}", flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=4403)
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()
    try:
        asyncio.run(main(a.port, not a.quiet))
    except KeyboardInterrupt:
        pass
