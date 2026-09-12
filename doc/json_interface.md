# RMBT CLI — JSON progress interface (specification)

This document **specifies** the machine-readable control interface that the CLI
clients in this repository (`clientJava`, `clientC`, `clientRust`) MUST implement
to be drop-in compatible with the
[`open-rmbt-desktop`](https://github.com/rtr-nettest/open-rmbt-desktop) Electron
application.

The contract is derived from the historic **JSON-enabled fork** of the RTR RMBT
Java client, which the desktop app
currently drives via a bundled `RMBTClient-all.jar`. Each requirement below cites
the historic code it comes from, and is cross-checked against the desktop
consumer. Requirement keywords (MUST / SHOULD / MAY) are used in the RFC 2119
sense.

## Reference code bases

| Repo | Role | Key files |
| --- | --- | --- |
| **JSON-enabled fork** (`open-rmbt`) | Normative producer — defines this contract | `RMBTClient/src/main/java/at/rtr/rmbt/client/{RMBTClientRunner,RMBTClient,RMBTTest}.java`, `helper/{ControlServerConnection,DebugStates,TestStatus,Globals}.java`, `QualityOfServiceTest.java` |
| `open-rmbt-desktop` | Consumer — what the app actually reads | `src/measurement/services/rmbt-client-java.service.ts` |
| `open-rmbt-history-2012` | Ancestor — original 2012 client, no JSON interface | `RMBTClient/…/RMBTClientRunner.java` |

Where this spec deliberately diverges from the JSON-enabled fork (see
[§9](#9-deviations-from-the-historic-fork)), the historic behaviour is called out
so the divergence is intentional and traceable.

---

## 1. Transport & activation

1. The client runs as a child process spawned by the app
   (`child_process.spawn`). It communicates **out** to the app over **stdout**
   only. There is no stdin channel and no post-launch back-channel; the app stops
   a run by killing the process.
2. Progress messages are **one JSON object per line**, UTF-8, terminated by
   `\n`, with no embedded newlines and no pretty-printing (historically
   `System.out.println(json)` with org.json's compact form).
3. The app buffers stdout and splits on `\n`. A line **starting with `{`** is
   parsed as a protocol message; any other line is treated as a human log line
   (ignored except for liveness, §1.6).
4. **Activation flag `-v`.** JSON output MUST be emitted only when `-v`
   (`--verbose`, historically "Log data for gui") is passed. Without it the
   client prints only human-readable logs.
   *(Historic: `options.has("v")` → `Globals.DEBUG_CLI_GUI`, which gates every
   JSON emission in `RMBTClient`, `RMBTTest`, `ControlServerConnection`,
   `QualityOfServiceTest`.)*
5. `--log` is a **separate** flag enabling verbose human debug text
   (`Globals.DEBUG_CLI`); it MUST NOT, by itself, enable the JSON protocol.
6. **Liveness.** The app aborts a run after `ALLOWED_INACTIVITY_MS` (default
   **10 000 ms**) with no stdout line. Clients MUST emit at least one message
   (progress or state) within any 10 s window during a phase.
7. **Exit code.** `0` on success; non-zero signals an error to the app.

---

## 2. Process lifecycle & sentinels

The historic fork brackets each run with two **plain-text** lines (printed
unconditionally, i.e. even without `-v`):

```
STARTING TEST.
… (messages) …
ENDING TEST.
```

The desktop consumer keys its wrap-up **solely** on the literal line
`ENDING TEST.` (`line.startsWith("ENDING TEST.")`).

**This spec's lifecycle rule (adopted improvement):**

* Clients MUST make `STATE_CHANGE` the **authoritative** lifecycle signal — in
  particular they MUST emit `STATE_CHANGE` with `state:"END"` (§3.2) as the last
  protocol message of a successful run, then exit `0`. Consumers SHOULD rely on
  `STATE_CHANGE` states rather than parsing free text.
* For **backward compatibility** with the current desktop consumer, clients MUST
  still print the bare sentinel lines `STARTING TEST.` (before the run) and
  `ENDING TEST.` (after the run), each on its own line with no prefix, exact
  case. These are a compatibility shim, not the primary contract, and MAY be
  retired once consumers migrate to `STATE_CHANGE`.

Rationale: the plain-text sentinel is fragile (any log prefix or case change
breaks the consumer). Promoting `STATE_CHANGE` to the source of truth removes
that fragility while the sentinel keeps today's app working.

---

## 3. Client → app messages (output)

Every message is a one-line JSON object with a string `type` field (historically
a `DebugStates` enum name). Consumers dispatch on `type` and ignore unknown types
and unknown fields; the fork emits more fields than the app reads. The **App
reads** column marks the fields the desktop consumer actually consumes — those
are load-bearing; the rest SHOULD be emitted for fidelity but are not required
for the current app.

`DebugStates` (message `type` values): `UUID_INFO`, `STATE_CHANGE`,
`PING_RESULT`, `DOWNLOAD_RESULT`, `UPLOAD_RESULT`, `QOS_RESULT`.

### 3.1 `UUID_INFO`

Emitted once, as soon as the control server has registered the test.
*(Historic: `ControlServerConnection`.)*

```json
{"type":"UUID_INFO","testUuid":"…","openTestUuid":"O…","testToken":"…"}
```

| Field | Type | App reads | Notes |
| --- | --- | --- | --- |
| `testUuid` | string | ✅ | The app stores it and later fetches the full result from the control server by this UUID (§7). **Essential.** |
| `openTestUuid` | string | — | Public/open UUID (`O…`). |
| `testToken` | string | — | Test token. |

### 3.2 `STATE_CHANGE`

Emitted on every phase transition (and on error).
*(Historic: `RMBTClient.setStatus()`.)*

```json
{"type":"STATE_CHANGE","time":1789206987208,"state":"PING"}
```

| Field | Type | App reads | Notes |
| --- | --- | --- | --- |
| `state` | string (enum) | ✅ | New phase; see §6. |
| `time` | number (ms) | — | Wall-clock ms. |

### 3.3 `PING_RESULT`

Emitted per ping sample during `PING`. *(Historic: `RMBTTest.pingLog()`.)*

```json
{"type":"PING_RESULT","time":1789…,"phase":"PING","testOpenUuid":"…",
 "pingTimeNs":45502500,"pingTimeNsStart":…,"pingClient":6.7,"pingServer":6.9,
 "startTimeMs":1789…,"status":"PING"}
```

| Field | Type / unit | App reads | Notes |
| --- | --- | --- | --- |
| `pingClient` | number, **ms** | ✅ | Client-measured RTT (see §5). |
| `pingServer` | number, **ms** | ✅ | Server-measured RTT. |
| `pingTimeNs` | number, **ns** | ✅ | Sample time **relative to test start** (`pingTimeNs − startTimeNsPublic`). |
| `pingTimeNsStart` | number, ns | — | Absolute test-start ns. |
| `time`, `phase`, `testOpenUuid`, `startTimeMs`, `status` | — | — | Context. |

### 3.4 `DOWNLOAD_RESULT`

Emitted periodically during `DOWN`. *(Historic: `RMBTTest.calculateValuesChart()`.)*

```json
{"type":"DOWNLOAD_RESULT","time":1789…,"phase":"DOWN","down":244.5,"up":-1,
 "bytes":232068096,"testOpenUuid":"…","progress":0.83,"startTimeMs":1789…,"status":"DOWN"}
```

| Field | Type / unit | App reads | Notes |
| --- | --- | --- | --- |
| `down` | number, **Mbit/s (decimal)** | ✅ | Interim download throughput (see §5). |
| `bytes` | number, bytes | ✅ | Cumulative bytes so far. |
| `up` | number | — | Always `-1` in this message. |
| `progress` | number 0..1 | — | Phase progress. |
| `time`, `phase`, `testOpenUuid`, `startTimeMs`, `status` | — | — | Context. |

### 3.5 `UPLOAD_RESULT`

Emitted periodically during `UP`; mirror of `DOWNLOAD_RESULT` with `up` carrying
the throughput and `down:-1`.

```json
{"type":"UPLOAD_RESULT","time":1789…,"phase":"UP","up":51.5,"down":-1,
 "bytes":48282624,"testOpenUuid":"…","progress":0.71,"startTimeMs":1789…,"status":"UP"}
```

| Field | Type / unit | App reads | Notes |
| --- | --- | --- | --- |
| `up` | number, **Mbit/s (decimal)** | ✅ | Interim upload throughput. |
| `bytes` | number, bytes | ✅ | Cumulative bytes so far. |
| `down` | number | — | Always `-1`. |

### 3.6 `QOS_RESULT`

Emitted during QoS testing. The desktop consumer **ignores** this type. Clients
MAY emit it only if they implement QoS. *(Historic: `QualityOfServiceTest.qosLog()`.)*

```json
{"type":"QOS_RESULT","time":1789…,"phase":"QOS","qos_result":"<string>"}
```

---

## 4. App → client (command-line arguments)

The historic fork parses these with `joptsimple`. Multi-character options are
declared as long options but the app invokes them with a **single leading dash**
(e.g. `-set-version`, `-v`); clients MUST accept the single-dash spellings the app
sends. Argument vector the app sends, in order:

| Flag (as sent) | Example | Meaning |
| --- | --- | --- |
| `-h` | `c01.netztest.at` | Control-server host. |
| `-p` | `443` | Control-server port. |
| `--platform` | `Darwin` | Platform label (result `plattform`). |
| `--os` | `Darwin, 24.1.0` | OS string. |
| `--model` | `Desktop_arm64` | Device model. |
| `--osver` | `24.1.0` | OS version (result `os_version`). |
| `--type` | `DESKTOP` | Client type. |
| `--nettype` | `98` | Network type code (result `network_type`; default **98** = LAN). |
| `-set-version` | `4.1.0` | Reported client software version. |
| `-v` | *(flag)* | Enable the JSON interface (§1.4). |
| `-u` | `<uuid>` | Client UUID; omitted on first run → obtained from the server. |

Loop mode (sent only when looping):

| Flag | Value | Meaning |
| --- | --- | --- |
| `--user-loop-mode` | *(flag)* | Enable loop mode. |
| `--user-loop-mode-max-delay` | minutes | Max delay between iterations. |
| `--user-loop-mode-test-counter` | integer | Iteration counter. |
| `--user-loop-mode-uuid` | `<uuid>` | Loop UUID. |

Additional flags the historic fork accepts (not sent by the desktop; clients MAY
support): `--token`, `-s/--ssl`, `--ssl-no-verify`, `--ssl-verify`,
`-t/--threads`, `-d/--duration`, `-n/--ndt`, `--ndt-host`, `-q/--qos`,
`--server-type`, `-g/--gui` *(declared but unused for gating)*, `--log`,
`-l/--loop`, `-i/--interval`, `--qmon`, `--json-result`.

---

## 5. Units & conventions

* **Throughput (`down`, `up`): decimal megabits per second.**
  Clients MUST compute `Mbit/s = bytes × 8 ÷ seconds ÷ 1 000 000` — the SI factor
  **1 000 000** (base 1000).
  *(Historic producer: `RMBTTest` computes `(sum*8/seconds)/1_000_000`, i.e. true
  decimal Mbit/s.)*

  > **Historic error in a consumer component.** The `open-rmbt-desktop` app
  > reconstructs an internal bit/s value from these fields using the **binary**
  > factor `1024 × 1024 = 1 048 576` and then divides by `1 000 000`, a
  > ~**4.86 %** discrepancy (`1 048 576 / 1 000 000`). This affects only a
  > dead/unused code path in that app (final numbers come from the control
  > server, §7). Clients MUST emit **true decimal Mbit/s (÷1 000 000)** and MUST
  > NOT pre-scale to compensate for that historic 1024-based bug.

* **Ping RTT (`pingClient`, `pingServer`): milliseconds** (floating point).
* **Ping sample time (`pingTimeNs`): nanoseconds**, relative to test start.
  *(The ms/ns asymmetry is historic and load-bearing — the app multiplies the ms
  values by `1e6` and uses `pingTimeNs` verbatim.)*
* **`bytes`:** cumulative bytes transferred in the phase so far.
* **`network_type`:** RMBT numeric code; `98` (LAN) is the desktop default.

---

## 6. Measurement states (`STATE_CHANGE.state`)

Enum values (historic `helper/TestStatus.java` in the JSON-enabled fork):

```
NOT_STARTED, WAIT, INIT, PING, INIT_DOWN, DOWN, INIT_UP, UP,
SUBMITTING_RESULTS, SPEEDTEST_END, QOS_TEST_RUNNING,
SUBMITTING_QOS_RESULTS, QOS_END, END, ERROR, ABORTED
```

The fork's own source comment marks `NOT_STARTED, INIT_DOWN, SUBMITTING_RESULTS,
SUBMITTING_QOS_RESULTS` as **added** relative to the 2012 ancestor. Enum
declaration order is not the runtime order. Required happy-path progression:

```
INIT → INIT_DOWN → PING → DOWN → INIT_UP → UP → SUBMITTING_RESULTS → END
```

Clients MUST emit at least this progression. The desktop consumer acts on
`PING, INIT_DOWN, DOWN, INIT_UP, UP` (phase-start timestamps) and accepts the
rest without UI effect. `ERROR` / `ABORTED` MUST be emitted on failure/abort.

---

## 7. Result submission (control server)

The JSON stream carries only **live progress**, never the final result set. The
client MUST submit the complete result to the control server itself (the existing
`/result` POST — historic `RMBTClient.sendResult()`). The app then retrieves the
authoritative result from the control server **by `testUuid`** (from
`UUID_INFO`). Consequently `UUID_INFO` is the single indispensable message.

---

## 8. Reference sequence (happy path, `-v` set)

```
=============== RMBTClient <rev> ===============
STARTING TEST.
{"type":"STATE_CHANGE","time":…,"state":"INIT"}
{"type":"UUID_INFO","testUuid":"…","openTestUuid":"O…","testToken":"…"}
{"type":"STATE_CHANGE","time":…,"state":"INIT_DOWN"}
{"type":"STATE_CHANGE","time":…,"state":"PING"}
{"type":"PING_RESULT",…}                     (×N)
{"type":"STATE_CHANGE","time":…,"state":"DOWN"}
{"type":"DOWNLOAD_RESULT",…}                 (×N)
{"type":"STATE_CHANGE","time":…,"state":"INIT_UP"}
{"type":"STATE_CHANGE","time":…,"state":"UP"}
{"type":"UPLOAD_RESULT",…}                   (×N)
{"type":"STATE_CHANGE","time":…,"state":"SUBMITTING_RESULTS"}
{"type":"STATE_CHANGE","time":…,"state":"END"}
ENDING TEST.
(exit 0)
```

---

## 9. Deviations from the historic fork

Intentional differences between this spec and the JSON-enabled fork:

| Topic | Historic fork | This spec |
| --- | --- | --- |
| Lifecycle signal | Plain-text `ENDING TEST.` is what the app keys on | `STATE_CHANGE:"END"` is authoritative; `STARTING/ENDING TEST.` kept only as a compat shim (§2) |
| Throughput unit | Producer already emits decimal Mbit/s (÷1 000 000); the **desktop consumer** mis-scales with 1024² | Clients emit decimal Mbit/s (÷1 000 000); the 1024-based scaling is a historic consumer bug and MUST NOT be reproduced (§5) |
| QoS | `QOS_RESULT` emitted during QoS | Optional; emit only if QoS is implemented (§3.6) |

---

## 10. Lineage & implementation status

**Ancestor (2012).** `open-rmbt-history-2012/RMBTClient` has none of this
interface: its CLI accepts only `-u/--uuid`, `--token`, `-h/--host`, `-p/--port`,
`-s/--ssl`, `--ssl-no-verify/--ssl-verify`, `-t/--threads`, `-d/--duration`,
`-n/--ndt`, `--ndt-host`, `-q/--qos`, `--server-type`; it prints human text only
and exposes progress to embedders via a polling API (`getStatus()`,
`getIntermediateResult()`, `getTestUuid()`, `setOutputCallback()`), not stdout
JSON. `network_type` was hardcoded `97` (CLI) / `98` (applet).

**JSON-enabled fork.** Added `Globals.DEBUG_CLI_GUI` (gated by `-v`), the
`DebugStates` message types, the extended `TestStatus`, the extended CLI, and
`network_type` default `98` — i.e. exactly the contract specified above.

**These clients.** `clientJava`, `clientC`, and `clientRust` currently emit only
human-readable progress. Implementing §1–§7 (behind `-v`) is net-new work in all
three.

### Implementation checklist (per client)

- [ ] Accept the argv in §4 (single-dash spellings), at minimum `-h`, `-p`, `-u`,
      `-v`, `--nettype`, `--type`, `--platform`, `--os`, `--osver`, `--model`,
      `-set-version`, and the four `--user-loop-mode*` flags.
- [ ] When `-v` is set, emit single-line JSON messages (§3) with the correct
      units (§5); otherwise stay silent on the JSON channel.
- [ ] Emit `UUID_INFO` as soon as the test UUID is known.
- [ ] Emit `STATE_CHANGE` on every transition (§6), ending with `state:"END"`.
- [ ] Emit `PING_RESULT` / `DOWNLOAD_RESULT` / `UPLOAD_RESULT` at intervals
      ≤ 10 s apart during their phases.
- [ ] Print the `STARTING TEST.` / `ENDING TEST.` compat sentinels (§2).
- [ ] Submit the full result to the control server (already implemented) so the
      app can fetch it by `testUuid`.
- [ ] Exit `0` on success, non-zero on error.
