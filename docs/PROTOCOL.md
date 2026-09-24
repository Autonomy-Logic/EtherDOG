# EtherDOG protocol

EtherDOG is driven over two channels:

- **Control**: a stream socket carrying one JSON object per line, in both directions. Used for configuration, lifecycle, discovery, status and logs.
- **Data**: datagrams carrying the process image, one frame in each direction per bus cycle.

Protocol version: **1**.

## Endpoints

| Channel | Spec | Notes |
|---|---|---|
| Control | `unix:<path>` | Default `unix:/run/etherdog/etherdog.socket`, on Linux and Windows (MSYS2). Created with mode `0600`; peers are checked by user id (see Access). |
| Control | `tcp:<loopback ip>:<port>` | Only loopback addresses are accepted. Requires a token. |
| Data | `unix:<path>` | Datagram socket. The client binds it before `open_data`. Not available on Windows (MSYS2). |
| Data | `udp:<loopback ip>:<port>` | Only loopback addresses are accepted. |

## Control channel

Each request is a JSON object on one line: `{"command": "<name>", "params": {...}}`. Each reply is a JSON object on one line. A failed request carries an `"error"` string; a successful one usually carries `"status": "success"`. Requests on one connection are answered in order.

### Access

On a unix control socket, EtherDOG asks the operating system who is connecting (`SO_PEERCRED`) and accepts only its own user, root, and any `--allow-uid <uid>`. Other connections get `{"error": "permission denied"}` and are closed. A tcp socket carries no peer identity, so EtherDOG refuses to open one without a token.

### Token

A token is optional on a unix socket and mandatory on tcp. It is passed on the first line of stdin (`--token-stdin`) or in `$ETHERDOG_TOKEN`, never on the command line, where other users can read it. With a token, the first request on every connection must be:

```json
{"command": "hello", "params": {"token": "<token>"}}
```

Any other first request, or a wrong token, gets an error and the connection is closed. `hello` works without a token too, and reports the server:

```json
{"status": "success", "name": "EtherDOG", "version": "0.1.0", "protocol": 1}
```

### Lifecycle

| Command | Params | Effect |
|---|---|---|
| `configure` | `path` (optional) | Load a bus configuration file ([BUSCONFIG.md](BUSCONFIG.md)), replacing the current one. Refused while the bus runs. Without `path`, clears the configuration. The reply lists the masters (`index`, `name`, `interface`, `cycle_time_us`, `task_priority`, `slave_count`). |
| `start` | none | Brings every master to OPERATIONAL. Safe to repeat. The reply gives `started` and `total`. |
| `stop` | none | Drives the outputs to zero, moves the slaves to INIT, closes the data sessions. |
| `shutdown` | none | Stops the bus and exits the process. |

### Process data

| Command | Params | Effect |
|---|---|---|
| `layout` | none | Where each PDO entry sits in the exchanged image. Valid while a master is OPERATIONAL or RECOVERING (`ready: true`). |
| `open_data` | `endpoint` | Opens a data session towards the client's endpoint for every running master. Replaces any previous session. |
| `close_data` | none | Closes all data sessions. The outputs go to the safe state. |

`layout` reply:

```json
{
  "status": "success",
  "masters": [{
    "index": 0, "name": "bus_a", "state": "OPERATIONAL", "ready": true,
    "output_bytes": 1, "input_bytes": 1,
    "entries": [
      {"slave": 1, "pdo": "0x1600", "index": "0x7000", "subindex": 1, "direction": "output",
       "bit_offset": 0, "bit_length": 1, "data_type": "BOOL", "name": "Output"}
    ]
  }]
}
```

- `bit_offset` is relative to the start of the entry's direction region.
- An entry is identified by `(slave, index, subindex)`.
- Padding entries are not listed, but their bits still count towards the offsets.

`open_data` reply:

```json
{"status": "success", "masters": [{"index": 0, "endpoint": "unix:/run/etherdog/etherdog-data-0-42.sock", "session": "8f3a...16 hex digits"}]}
```

### Discovery and status

| Command | Params | Effect |
|---|---|---|
| `scan` | `interface` | Lists the slaves found on an interface. Refused while the bus runs. |
| `test` | `interface`, `position` | Reports the slave at one position. Refused while the bus runs. |
| `list-interfaces` | none | Lists the network adapters. |
| `status` | none | Per master: state, slaves with their AL state, and cycle metrics. |
| `diagnostics` | none | `status`, plus timing, recovery, data-session counters (`frames_rx`, `frames_tx`, `frames_dropped`, `watchdog_trips`) and a hex snapshot of the process image. |
| `logs` | `min_id`, `level`, `max` | Recent log entries from the in-memory ring, oldest first. The reply includes `next_id`, the value to pass as `min_id` next time. |

## Data channel

### Frame

Every frame is one datagram: a 24-byte little-endian header followed by the raw bytes of one direction's region.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `EDOG` |
| 4 | 1 | version (`1`) |
| 5 | 1 | kind: `1` outputs (client to EtherDOG), `2` inputs (EtherDOG to client) |
| 6 | 1 | master index |
| 7 | 1 | flags |
| 8 | 8 | session, from `open_data` |
| 16 | 4 | sequence, incremented by 1 per frame sent, wrapping |
| 20 | 2 | payload length |
| 22 | 2 | inputs: working counter; outputs: 0 |

Flags:
- `0x01` VALID. On inputs: the bus is OPERATIONAL. On outputs: the payload holds valid outputs. A clear flag asks for the safe state (all outputs zero).
- `0x02` WKC_OK (inputs only): the working counter matched the expected value this cycle.

The payload is exactly `input_bytes` (inputs) or `output_bytes` (outputs) from `layout`, at most 4096 bytes.

`include/etherdog_protocol.h` implements the header encoding and decoding.

### Timing

The bus cycle is set by `master.cycle_time_us` in the bus configuration. It is both how often EtherCAT frames go on the wire and how often data is exchanged with the client. Each cycle:

1. EtherDOG takes the newest valid output frame received since the last cycle (older and invalid frames are dropped) and writes it into the process image.
2. EtherDOG exchanges the process image with the slaves.
3. EtherDOG sends one input frame to the client.

A client answers each input frame with one output frame. The outputs it sends are applied at the start of the next cycle.

### Validation and safety

EtherDOG drops an output frame when any of these is wrong:
- magic, version, kind or master index;
- the session;
- the payload length;
- the sender address (it must be the endpoint given to `open_data`);
- the sequence (it must be newer than the last applied frame).

If no valid output frame arrives for 100 ms, EtherDOG writes zeros to the outputs and counts a watchdog trip. Normal outputs resume with the next valid frame.

## Logs

Every log entry goes to stderr and to the in-memory ring that `logs` reads. With `--log-socket <spec>`, EtherDOG also connects to that stream socket as a client and writes one JSON object per line:

```json
{"timestamp": "1790246115", "level": "INFO", "source": "EtherDOG", "message": "[ETHERDOG] [BUS] 1/1 EtherCAT master(s) running"}
```

It reconnects on its own. Entries logged while disconnected are sent after reconnecting, as long as they are still in the ring.
