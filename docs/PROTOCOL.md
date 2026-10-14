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
{"status": "success", "name": "EtherDOG", "version": "1.0.0", "protocol": 1,
 "features": ["ethercat.dynamic_alloc", "ethercat.sdo_byte_string", "ethercat.sdo_complete_access",
              "ethercat.scan_modules", "ethercat.diagnostics_slave"]}
```

`features` names what this build supports ([BUSCONFIG.md](BUSCONFIG.md#protocol-capabilities)); clients ignore names they do not know.

### Lifecycle

| Command | Params | Effect |
|---|---|---|
| `configure` | `path` (optional) | Load a bus configuration file ([BUSCONFIG.md](BUSCONFIG.md)), replacing the current one. Refused while the bus runs. Without `path`, clears the configuration. The reply lists the masters (`index`, `name`, `interface`, `cycle_time_us`, `task_priority`, `slave_count`). |
| `start` | none | Brings every master to OPERATIONAL. Safe to repeat. The reply gives `started` and `total`. |
| `stop` | none | Drives the outputs to zero, moves the slaves to INIT, closes the data sessions. |
| `shutdown` | none | Stops the bus (outputs to zero, slaves to INIT) and exits the process. |

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
| `status` | none | Per master: state, slaves with their AL state, and cycle metrics (see below). |
| `diagnostics` | none | `status`, plus timing (including `overruns`, cycles that ended past the next deadline), recovery, the cyclic AL status poll (`al_poll`: `status`, `responding`, `replies`, `misses`, `faults`), data-session counters (`frames_rx`, `frames_tx`, `frames_dropped`, `watchdog_trips`) and a hex snapshot of the process image. |
| `scan-modules` | `master`, `slave` | The detected module list (`0xF050`) of a modular coupler, in slot order from slot 1. A running master reads it through its own context; a stopped one opens its configured interface for the read, refused while another master runs on that interface. |
| `diagnostics-slave` | `master`, `slave` | Status of a running modular coupler (`0xF100`) and of each configured module. The slave must have a `modular` section. |
| `logs` | `min_id`, `level`, `max` | Recent log entries from the in-memory ring, oldest first. The reply includes `next_id`, the value to pass as `min_id` next time. |

`master` is the master index from `configure`; `slave` is the 1-based bus position.

`scan-modules` reply:

```json
{"status": "success", "master": 0, "slave": 2, "idents": ["0x00091F84", "0x040115C4"]}
```

`diagnostics-slave` reply:

```json
{"status": "success", "master": 0, "slave": 2,
 "coupler": {"status_word": 64, "poll_at_ms": 1760000000000},
 "modules": [{"slot": 1, "ident": "0x00091F84", "status_byte": 0}, {"slot": 2, "ident": "0x040115C4", "status_byte": null}]}
```

- `status_word` holds the 16 status bits of `0xF100` (read with Complete Access).
- `status_byte` is sub-index 1 of the module's object `0xA000 + (slot - 1) * slot_index_increment`.
- `null` means the value could not be read; the whole read is bounded to about 2 s.
- Both commands use the mailbox. While the monitor recovers slaves they wait up to 1 s, then
  reply with the error `bus recovering, retry`.

Errors from both commands are `{"status": "error", "message": "..."}`.

### Cycle metrics

`status` returns these keys in each master's `metrics` object; `diagnostics` returns the timing ones in `timing`.

| Key | Meaning |
|---|---|
| `cycle_count` | Bus cycles run since the master started. |
| `wkc_error_count` | Cycles whose working counter was below the expected value. |
| `noframe_count` | Cycles whose process-data frame did not come back. |
| `avg_cycle_us`, `min_cycle_us`, `max_cycle_us` | Time of the process-data exchange (send and receive). |
| `min_exchange_us`, `max_exchange_us` | Same values as `min_cycle_us` and `max_cycle_us`. |
| `avg_period_us`, `min_period_us`, `max_period_us` | Time between the starts of consecutive cycles. |
| `avg_latency_us`, `min_latency_us`, `max_latency_us` | How late each cycle woke after its deadline. |
| `consecutive_wkc_errors` | Working-counter errors in a row; `ECAT_WKC_ERROR_THRESHOLD` (3) in OPERATIONAL starts recovery. |
| `recovery_attempts` | Attempts in the current recovery; back to 0 once every slave is OPERATIONAL again. |
| `overruns` | Cycles that ended past the next deadline. The bus never skips a cycle; an overrun only delays the next one. |

`exchange_skips` is no longer reported: the bus cycle no longer waits on the monitor, so it cannot be skipped.

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

The payload is exactly `input_bytes` (inputs) or `output_bytes` (outputs) from `layout`, at most 65483 bytes (`EDOG_FRAME_MAX_PAYLOAD`).

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

## Outputs on exit

EtherDOG drives the outputs to zero whenever it leaves the bus: on `stop`, on `shutdown`, and when it receives SIGINT, SIGTERM, SIGHUP or SIGQUIT. The zero frame is resent until a slave answers (up to 3 times), then the slaves are moved to INIT. If the process is killed outright (SIGKILL, a crash), it cannot do this; the slaves' sync manager watchdog then takes the outputs to their safe state (`watchdog.sm_watchdog_ms` in the bus configuration, 100 ms by default).

## Logs

Every log entry goes to stderr and to the in-memory ring that `logs` reads. With `--log-socket <spec>`, EtherDOG also connects to that stream socket as a client and writes one JSON object per line:

```json
{"timestamp": "1790246115", "level": "INFO", "source": "EtherDOG", "message": "[ETHERDOG] [BUS] 1/1 EtherCAT master(s) running"}
```

It reconnects on its own. Entries logged while disconnected are sent after reconnecting, as long as they are still in the ring.
