#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Autonomy®

"""Reference EtherDOG client (docs/PROTOCOL.md), standard library only.

Examples:
  etherdog_client.py status
  etherdog_client.py scan eth0
  etherdog_client.py configure /path/busconfig.json
  etherdog_client.py start
  etherdog_client.py layout
  etherdog_client.py io --seconds 5 --set 1:0x7000:1=1 --set 1:0x7010:1=blink
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import struct
import sys
import tempfile
import time
from dataclasses import dataclass
from typing import Any

FRAME_HEADER = struct.Struct("<4sBBBBQIHH")
MAGIC = b"EDOG"
PROTOCOL_VERSION = 1
KIND_OUTPUTS = 1
KIND_INPUTS = 2
FLAG_VALID = 0x01
FLAG_WKC_OK = 0x02


def _connect(spec: str) -> socket.socket:
    if spec.startswith("unix:"):
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.connect(spec[len("unix:") :])
        return sock
    if spec.startswith("tcp:"):
        host, _, port = spec[len("tcp:") :].rpartition(":")
        return socket.create_connection((host, int(port)))
    raise ValueError(f"unsupported control endpoint: {spec}")


class ControlClient:
    """One control connection; each call sends a JSON line and reads one JSON line back."""

    def __init__(self, spec: str, token: str | None = None, timeout: float = 30.0) -> None:
        self._sock = _connect(spec)
        self._sock.settimeout(timeout)
        self._buf = b""
        hello = self.call("hello", {"token": token} if token else None)
        if "error" in hello:
            raise PermissionError(hello["error"])
        self.server = hello

    def call(self, command: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
        request: dict[str, Any] = {"command": command}
        if params is not None:
            request["params"] = params
        self._sock.sendall(json.dumps(request).encode() + b"\n")
        while b"\n" not in self._buf:
            chunk = self._sock.recv(65536)
            if not chunk:
                raise ConnectionError("EtherDOG closed the control connection")
            self._buf += chunk
        line, _, self._buf = self._buf.partition(b"\n")
        return json.loads(line)

    def close(self) -> None:
        self._sock.close()


@dataclass
class Entry:
    slave: int
    pdo: str
    index: str
    subindex: int
    direction: str
    bit_offset: int
    bit_length: int
    data_type: str
    name: str

    @property
    def key(self) -> tuple[int, int, int]:
        return (self.slave, int(self.index, 16), self.subindex)


def read_bits(image: bytes, bit_offset: int, bit_length: int) -> int:
    value = 0
    for i in range(bit_length):
        bit = bit_offset + i
        if image[bit // 8] >> (bit % 8) & 1:
            value |= 1 << i
    return value


def write_bits(image: bytearray, bit_offset: int, bit_length: int, value: int) -> None:
    for i in range(bit_length):
        bit = bit_offset + i
        mask = 1 << (bit % 8)
        if value >> i & 1:
            image[bit // 8] |= mask
        else:
            image[bit // 8] &= ~mask & 0xFF


class DataSession:
    """Cyclic exchange with one master: receive input frames, answer each with outputs."""

    def __init__(self, control: ControlClient, master_index: int = 0, use_udp: bool = False) -> None:
        if use_udp or not hasattr(socket, "AF_UNIX"):
            self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self._sock.bind(("127.0.0.1", 0))
            endpoint = f"udp:127.0.0.1:{self._sock.getsockname()[1]}"
            self._path = None
        else:
            self._path = os.path.join(tempfile.gettempdir(), f"edog-client-{os.getpid()}.sock")
            if os.path.exists(self._path):
                os.unlink(self._path)
            self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
            self._sock.bind(self._path)
            endpoint = f"unix:{self._path}"
        self._sock.settimeout(1.0)

        layout = control.call("layout")
        master = next(m for m in layout["masters"] if m["index"] == master_index)
        if not master["ready"]:
            raise RuntimeError(f"master {master_index} is not running ({master['state']})")
        self.entries = [Entry(**e) for e in master["entries"]]
        self.output_bytes = int(master["output_bytes"])
        self.input_bytes = int(master["input_bytes"])

        opened = control.call("open_data", {"endpoint": endpoint})
        if "error" in opened:
            raise RuntimeError(opened["error"])
        info = next(m for m in opened["masters"] if m["index"] == master_index)
        self.master_index = master_index
        self.session = int(info["session"], 16)
        self._server = self._address(info["endpoint"])
        self.outputs = bytearray(self.output_bytes)
        self.inputs = bytes(self.input_bytes)
        self.last_flags = 0
        self.last_wkc = 0
        self._seq = 0

    @staticmethod
    def _address(endpoint: str) -> Any:
        if endpoint.startswith("unix:"):
            return endpoint[len("unix:") :]
        host, _, port = endpoint[len("udp:") :].rpartition(":")
        return (host, int(port))

    def step(self) -> bool:
        """Wait for one input frame, then send the current outputs. False on timeout."""
        try:
            data = self._sock.recv(65536)
        except socket.timeout:
            return False
        magic, ver, kind, master, flags, session, _seq, length, wkc = FRAME_HEADER.unpack_from(data)
        if (
            magic != MAGIC
            or ver != PROTOCOL_VERSION
            or kind != KIND_INPUTS
            or master != self.master_index
            or session != self.session
            or length != len(data) - FRAME_HEADER.size
        ):
            return True
        self.inputs = data[FRAME_HEADER.size :]
        self.last_flags = flags
        self.last_wkc = wkc
        self._seq = (self._seq + 1) & 0xFFFFFFFF
        header = FRAME_HEADER.pack(
            MAGIC, PROTOCOL_VERSION, KIND_OUTPUTS, self.master_index, FLAG_VALID,
            self.session, self._seq, self.output_bytes, 0,
        )
        self._sock.sendto(header + bytes(self.outputs), self._server)
        return True

    def get(self, entry: Entry) -> int:
        image = self.inputs if entry.direction == "input" else bytes(self.outputs)
        return read_bits(image, entry.bit_offset, entry.bit_length)

    def set(self, entry: Entry, value: int) -> None:
        write_bits(self.outputs, entry.bit_offset, entry.bit_length, value)

    def close(self) -> None:
        self._sock.close()
        if self._path and os.path.exists(self._path):
            os.unlink(self._path)


def _find(entries: list[Entry], spec: str) -> Entry:
    slave, index, sub = spec.split(":")
    key = (int(slave), int(index, 16), int(sub, 0))
    for e in entries:
        if e.key == key:
            return e
    raise KeyError(f"no layout entry {spec}")


def _run_io(control: ControlClient, args: argparse.Namespace) -> int:
    session = DataSession(control, args.master, use_udp=args.udp)
    sets: list[tuple[Entry, str]] = []
    for item in args.set or []:
        spec, _, value = item.partition("=")
        sets.append((_find(session.entries, spec), value))

    frames = 0
    deadline = time.monotonic() + args.seconds
    next_print = time.monotonic()
    while time.monotonic() < deadline:
        now = time.monotonic()
        for entry, value in sets:
            level = int(now * 2) & 1 if value == "blink" else int(value, 0)
            session.set(entry, level)
        if session.step():
            frames += 1
        if now >= next_print:
            inputs = {f"{e.slave}:{e.index}:{e.subindex}": session.get(e)
                      for e in session.entries if e.direction == "input"}
            ok = "OP" if session.last_flags & FLAG_VALID else "not OP"
            print(f"[{frames} frames, {ok}, wkc={session.last_wkc}] inputs={inputs}", flush=True)
            next_print = now + 1.0
    control.call("close_data")
    session.close()
    print(f"exchanged {frames} frames in {args.seconds:g} s")
    return 0 if frames > 0 else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument("--control", default=os.environ.get("ETHERDOG_CONTROL", "unix:/run/etherdog/etherdog.socket"))
    parser.add_argument("--token", default=os.environ.get("ETHERDOG_TOKEN"))
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("status", "diagnostics", "start", "stop", "layout", "list-interfaces", "close_data", "shutdown"):
        sub.add_parser(name)
    p = sub.add_parser("scan")
    p.add_argument("interface")
    p = sub.add_parser("test")
    p.add_argument("interface")
    p.add_argument("position", type=int)
    p = sub.add_parser("configure")
    p.add_argument("path", nargs="?")
    p = sub.add_parser("io", help="exchange process data for a while")
    p.add_argument("--master", type=int, default=0)
    p.add_argument("--seconds", type=float, default=5.0)
    p.add_argument("--udp", action="store_true", help="use a loopback UDP data socket")
    p.add_argument("--set", action="append", help="slave:index:subindex=value|blink")
    args = parser.parse_args()

    control = ControlClient(args.control, args.token)
    try:
        if args.command == "io":
            return _run_io(control, args)
        params: dict[str, Any] | None = None
        if args.command in ("scan", "test"):
            params = {"interface": args.interface}
            if args.command == "test":
                params["position"] = args.position
        elif args.command == "configure":
            params = {"path": os.path.abspath(args.path)} if args.path else {}
        result = control.call(args.command, params)
        print(json.dumps(result, indent=2))
        return 1 if "error" in result else 0
    finally:
        control.close()


if __name__ == "__main__":
    sys.exit(main())
