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
| `safe_close` | `true` | Zero the outputs and wait for INIT on stop. |
| `log_level` | `"info"` | Accepted for compatibility; the process level is set with `--log-level`. |

## slaves[]

| Field | Default | Meaning |
|---|---|---|
| `position` | **required** | 1-based position on the bus. |
| `name` | `""` | Display name. |
| `type` | `"coupler"` | Free-form. |
| `vendor_id` | `"0x0"` | Hex string. Checked against the bus when `startup_checks.check_vendor_id` is set. |
| `product_code` | `"0x0"` | Hex string. Checked when `startup_checks.check_product_code` is set. |
| `revision` | `"0x0"` | Hex string. Informational. |
| `rx_pdos`, `tx_pdos` | `[]` | Output (RxPDO) and input (TxPDO) PDOs, in the order the slave maps them. |
| `channels` | `[]` | Named I/O points. Each one references a PDO entry. |
| `sdo_configurations` | `[]` | SDO writes applied in PRE-OP. |
| `config` | see below | Per-slave settings. |

A PDO:

```json
{ "index": "0x1A00", "name": "Channel 1",
  "entries": [ { "index": "0x6000", "subindex": 1, "bit_length": 1, "name": "Input", "data_type": "BOOL" } ] }
```

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

## Example

[examples/ek1818_busconfig.json](../examples/ek1818_busconfig.json) is a Beckhoff EK1818: 8 digital inputs and 4 digital outputs.
