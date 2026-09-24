# EtherDOG

EtherDOG is an EtherCAT master service. It runs one or more EtherCAT masters from a JSON bus configuration and exchanges process data with a client program over local sockets. The process image travels as one datagram in each direction per bus cycle, and a line-based JSON control channel handles configuration, discovery and status.

Built on [SOEM](https://github.com/OpenEtherCATsociety/SOEM).

## Features

- Several masters, each on its own interface, with a dedicated SCHED_FIFO bus thread and absolute-deadline timing.
- Topology validation, SDO configuration, PDO mapping, optional distributed clocks and per-slave watchdogs.
- Background monitoring with automatic slave recovery.
- Bus scan and slave test for discovery.
- A published process-data layout per master: every PDO entry with its bit offset.
- A per-cycle data exchange over AF_UNIX or loopback UDP, with session ids, sequence checks and an output watchdog (outputs go to zero when the client goes quiet).
- Status and diagnostics with cycle, period and wake-up latency statistics.
- Logs on stderr, in an in-memory ring (the `logs` command), and optionally streamed as JSON lines to a log socket.
- Linux, and Windows through MSYS2 (Npcap required).

## Build

```sh
git clone --recurse-submodules https://github.com/Autonomy-Logic/EtherDOG
cmake -S EtherDOG -B EtherDOG/build -DCMAKE_BUILD_TYPE=Release
cmake --build EtherDOG/build -j
```

This needs CMake 3.28 or newer (for SOEM). Add `-DETHERDOG_BUILD_TESTS=ON` and run `ctest --test-dir EtherDOG/build` for the unit tests.

## Run

```sh
sudo ./build/etherdog --config examples/ek1818_busconfig.json --start
```

| Option | Default |
|---|---|
| `--control <spec>` | `unix:/run/etherdog/etherdog.socket` |
| `--allow-uid <uid>` | none. Unix socket clients must run as EtherDOG's user, root, or one of these. |
| `--token-stdin` | off. Reads a token from stdin (or `$ETHERDOG_TOKEN`); clients must send it. Required for `tcp:`. |
| `--state-dir <dir>` | `/run/etherdog` |
| `--config <file>` | none |
| `--start` | start the bus after loading `--config` |
| `--log-level <level>` | `info` |
| `--log-socket <spec>` | none |

Raw Ethernet access and real-time scheduling need root, or `CAP_NET_RAW`, `CAP_NET_ADMIN` and `CAP_SYS_NICE`.

## Talking to it

- [docs/PROTOCOL.md](docs/PROTOCOL.md): the control commands and the data frames.
- [docs/BUSCONFIG.md](docs/BUSCONFIG.md): the bus configuration format.
- [tools/etherdog_client.py](tools/etherdog_client.py): a reference client in plain Python.

```sh
python3 tools/etherdog_client.py status
python3 tools/etherdog_client.py layout
python3 tools/etherdog_client.py io --seconds 5 --set 1:0x7000:1=blink
```

## License

GPL-3.0-or-later. See [LICENSE](LICENSE). `include/etherdog_protocol.h` and `tools/etherdog_client.py` are MIT.
