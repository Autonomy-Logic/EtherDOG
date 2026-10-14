# Bus configuration

A bus configuration file describes one or more EtherCAT masters. It is loaded with `--config <file>` at startup or with the `configure` command. The root is a JSON array; each entry with `"protocol": "ETHERCAT"` becomes one master. Other entries are ignored. At most 4 masters.

Every field below is optional unless marked **required**. The defaults are shown.

```json
[
  {
    "name": "bus_a",
    "protocol": "ETHERCAT",
    "config": {
      "master": { ... },
      "slaves": [ { ... } ],
      "diagnostics": { ... }
    }
  }
]
```

`name` (default `"master"`) identifies the master in `status`, `layout` and the logs.

## master

| Field | Default | Meaning |
|---|---|---|
| `interface` | `"eth0"` | Network interface. A Windows NPF device path on Windows. |
| `cycle_time_us` | `1000` | Bus cycle: how often frames go on the wire, and how often data is exchanged with the client. |
| `receive_timeout_us` | `2000` | Time to wait for the frame to return (minimum 200). |
| `watchdog_timeout_cycles` | `3` | Accepted for compatibility; not used. |
| `task_priority` | `90` | SCHED_FIFO priority of the bus thread (1-99). |
| `safe_close` | `true` | Accepted for compatibility; not used. Outputs are always zeroed on stop and exit. |
| `log_level` | `"info"` | Accepted for compatibility; the process level is set with `--log-level`. |

## slaves[]

| Field | Default | Meaning |
|---|---|---|
| `position` | **required** | 1-based position on the bus. |
| `name` | `""` | Display name. |
| `type` | `"coupler"` | Free-form. |
| `vendor_id` | **required** | Hex string, non-zero. Checked against the bus when `startup_checks.check_vendor_id` is set. |
| `product_code` | **required** | Hex string, non-zero. Checked when `startup_checks.check_product_code` is set. |
| `revision` | `"0x0"` | Hex string. Informational. |
| `rx_pdos`, `tx_pdos` | `[]` | Output (RxPDO) and input (TxPDO) PDOs, in the order the slave maps them. A PDO index may appear once per slave across both lists. |
| `mandatory_pdos` | `[]` | Hex PDO indices the device requires to be assigned. The configuration is refused when one is missing from `rx_pdos`/`tx_pdos`. |
| `modular` | absent | Slot rules and modules of an ETG.5001 modular coupler; see below. |
| `channels` | `[]` | Named I/O points. Each one references a PDO entry. |
| `sdo_configurations` | `[]` | SDO writes applied in PRE-OP. |
| `config` | see below | Per-slave settings. |

A PDO:

```json
{ "index": "0x1A00", "name": "Channel 1", "fixed": true,
  "entries": [ { "index": "0x6000", "subindex": 1, "bit_length": 1, "name": "Input", "data_type": "BOOL" } ] }
```

- Hex values (`vendor_id`, `product_code`, `revision`, indices, idents) are `0x` or `#x` followed by 1 to 8 hex digits.
- `fixed` (default `false`): the mapping is fixed in the device. On a slave that is not a
  modular coupler, EtherDOG reads the mapping object in PRE-OP before mapping and aborts the
  start when the device maps different entries. A mapping it cannot read is logged and skipped.

- An entry with index `0x0000` (or data type `PAD`) is padding. It takes up bits but is not published.
- Data types: `BOOL`, `INT8`/`SINT`, `UINT8`/`USINT`, `INT16`/`INT`, `UINT16`/`UINT`, `INT32`/`DINT`, `UINT32`/`UDINT`, `INT64`/`LINT`, `UINT64`/`ULINT`, `REAL`/`REAL32`/`FLOAT`, `LREAL`/`REAL64`/`DOUBLE`, `PAD`.

A channel:

```json
{ "index": 0, "name": "Input 1", "type": "digital_input", "bit_length": 1,
  "pdo_index": "0x1A00", "pdo_entry_index": "0x6000", "pdo_entry_subindex": 1 }
```

An SDO:

```json
{ "index": "0x8000", "subindex": 1, "value": 3, "data_type": "UINT16", "name": "Filter" }
```

Two optional additions:

- `value_bytes` carries a raw byte-string payload instead of a numeric `value`. It accepts
  a plain string, taken as its UTF-8 bytes (no trailing NUL), or a `"0x..."` hex form
  decoded to bytes. When present, `value` and the numeric range check are not consulted;
  the payload wire-type is the bytes themselves. Needed for ETG.5001 module `InitCmd`
  writes like `0x80n0:03 = "UR20-4DI-P"` that no numeric type can represent.

  `data_type` for a byte-string write is `VISIBLE_STRING`, `OCTET_STRING` or `STRING`.
  Any other `data_type` paired with a `value_bytes` payload is also accepted and treated
  as a byte-string write, so a vendor-specific type name does not have to be renamed
  before upload.
- `complete_access`: `true` sends the SDO with CoE Complete Access (one PDU updates every
  sub-index of the object). Required for `0xF030` on couplers that advertise
  CompleteAccess. Defaults to `false`.

```json
{ "index": "0x8010", "subindex": 3, "value_bytes": "UR20-4DI-P",
  "data_type": "VISIBLE_STRING", "name": "Module name init" }
```

### slaves[].modular

```json
{ "slot_index_increment": 16,
  "slots": [ { "min_instances": 0, "max_instances": 63, "module_classes": ["Di", "Do"], "module_idents": [] } ],
  "modules": [ { "slot": 1, "ident": "0x00091F84", "class": "Di" } ] }
```

| Field | Meaning |
|---|---|
| `slot_index_increment` | Object index step from one slot to the next (ESI `SlotIndexIncrement`), 0 to 4095. With 0, module states are not read. |
| `slots[]` | The coupler's `<Slot>` definitions, in order. `min_instances` to `max_instances` (at least 1) physical slots each. A slot accepts a module whose ident is in `module_idents` or whose class is in `module_classes`; with both empty it accepts any module. |
| `modules[]` | The configured modules, in rising slot order, one per slot. `slot` is 1-based, `ident` is the module ident (hex), `class` its module class (optional). |

Physical slots 1 to the last configured one fill the slot definitions in order: a definition
takes slots until it holds `max_instances`, or, once it holds `min_instances`, until the next
module is one it does not accept. An empty slot counts towards the current definition. The
configuration is refused when a module lands in a definition that does not accept it, when the
slots run past the last definition, or when a definition ends with fewer than `min_instances`.

The module list (`0xF030`), module `InitCmd`s and PDO assignment are ordinary
`sdo_configurations`; `modular` only describes the station so EtherDOG can check it and
read the modules' diagnostics.

### slaves[].config

| Field | Default |
|---|---|
| `startup_checks.check_vendor_id` / `check_product_code` | `true` / `true` |
| `addressing.ethercat_address` | `0` (auto) |
| `timeouts.sdo_timeout_ms` / `init_to_preop_timeout_ms` / `safeop_to_op_timeout_ms` | `1000` / `3000` / `10000` |
| `watchdog.sm_watchdog_enabled` / `sm_watchdog_ms` | `true` / `100` |
| `watchdog.pdi_watchdog_enabled` / `pdi_watchdog_ms` | `false` / `100` |
| `distributed_clocks.enabled`, `sync_unit_cycle_us`, `sync0_enabled`, `sync0_cycle_us`, `sync0_shift_us`, `sync1_enabled`, `sync1_cycle_us`, `sync1_shift_us` | `false` / `0` |
| `strict_sdo` | `true`: a failed SDO write aborts the start |

## diagnostics

`log_connections` (`true`), `log_data_access` (`false`), `log_errors` (`true`), `max_log_entries` (`10000`) and `status_update_interval_ms` (`500`) are accepted for compatibility.

## Limits

Per-master and per-slave buffers are heap-allocated at config load, so the only remaining
limits are the ones the protocols or EtherDOG's own addressing model enforce. A configuration
that cannot be allocated, or that fails a check above, is refused as a whole; nothing is
truncated.

| Item | Limit | Reason |
|---|---|---|
| Masters | 4 | Top-level cap; one bus per NIC in practice. |
| Slaves per master | hardware memory | Allocated per project at upload. |
| PDOs per slave, per direction | hardware memory | Allocated per project at upload. |
| Entries per PDO | hardware memory | Allocated per project at upload. |
| Channels per slave | hardware memory | Allocated per project at upload. |
| SDO configurations per slave | hardware memory | Allocated per project at upload. |
| Process image per direction | 65483 bytes | One data-channel datagram: the IPv4 UDP payload limit (65507) minus the 24-byte frame header. |

The IOmap is allocated at start from the configured PDOs. After the startup SDOs, and before
SOEM maps the process data, it grows to what the slaves report through the same sources SOEM
reads (CoE PDO assignment, SoE, SII), plus a per-slave alignment slack
(`ECAT_IOMAP_SLACK_PER_SLAVE`) and a floor (`ECAT_IOMAP_MIN_BYTES`). Allocation failures
surface as out-of-memory errors. The data-channel frame buffers are allocated from the layout
at start and freed at stop.

Names (masters, slaves, PDOs, entries) longer than 63 characters are shortened to 63.

## Protocol capabilities

The `hello` control-socket response advertises the EtherCAT feature set so a newer
editor can refuse to compile a project that targets this EtherDOG with features it does
not support. Current features:

| Name | Meaning |
|---|---|
| `ethercat.dynamic_alloc` | Buffers sized from the configuration; `modular`, `mandatory_pdos` and `fixed` are checked. |
| `ethercat.sdo_byte_string` | `sdo_configurations[].value_bytes` is honoured. |
| `ethercat.sdo_complete_access` | `sdo_configurations[].complete_access` is honoured. |
| `ethercat.scan_modules` | The `scan-modules` command reads `0xF050` on a slave ([PROTOCOL.md](PROTOCOL.md)). |
| `ethercat.diagnostics_slave` | The `diagnostics-slave` command reads a modular coupler's status ([PROTOCOL.md](PROTOCOL.md)). |

## Example

[examples/ek1818_busconfig.json](../examples/ek1818_busconfig.json) is a Beckhoff EK1818: 8 digital inputs and 4 digital outputs.
