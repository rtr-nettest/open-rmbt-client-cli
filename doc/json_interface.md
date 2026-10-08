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
7. **Exit code.** `0` on success (including a measurement whose result
   submission failed, see §3.8); non-zero signals a failed test. A non-zero exit
   is preceded by `STATE_CHANGE` `ERROR` (§3.2, §6.1) whenever the client can
   still report it.

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
`PING_RESULT`, `DOWNLOAD_RESULT`, `UPLOAD_RESULT`, `QOS_RESULT`, plus the
additions of this spec `FINAL_RESULT` (§3.7) and `SUBMIT_RESULT` (§3.8).

### 3.1 `UUID_INFO`

Emitted once, as soon as the control server has registered the test.
*(Historic: `ControlServerConnection`.)*

```json
{"type":"UUID_INFO","testUuid":"…","openTestUuid":"O…","testToken":"…","loopUuid":null}
```

| Field | Type | App reads | Notes |
| --- | --- | --- | --- |
| `testUuid` | string | ✅ | The app stores it and later fetches the full result from the control server by this UUID (§7). **Essential.** |
| `openTestUuid` | string | — | Public/open UUID (`O…`). |
| `testToken` | string | — | Test token. |
| `loopUuid` | string \| null | — | Server loop UUID when running in loop mode (§4.1), else `null`. On the first loop iteration this is the UUID the server just minted. |

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
| `phase` | string | — | **Only with `state:"ERROR"`:** the state the run was in when it failed (e.g. `INIT`, `DOWN`). |
| `error` | string | — | **Only with `state:"ERROR"`:** single-line human-readable reason. |

Failure example:

```json
{"type":"STATE_CHANGE","time":1789206991021,"state":"ERROR","phase":"DOWN","error":"all 4 download threads failed"}
```

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

### 3.7 `FINAL_RESULT`

Emitted **once**, after the upload phase and **before** `SUBMITTING_RESULTS`, so
the app has the final numbers even if the result submission fails. Carries the
values the client measured and submits to the control server. Not emitted on a
failed test. *(Addition of this spec; not in the historic fork.)*

```json
{"type":"FINAL_RESULT","time":1789…,"testUuid":"…","openTestUuid":"O…","loopUuid":null,
 "down":244.53,"up":51.47,"pingMedian":6.91,"pingCount":10,
 "downBytes":232068096,"downNs":7012345678,"downThreads":4,
 "upBytes":48282624,"upNs":7004321000,"upThreads":4}
```

| Field | Type / unit | Notes |
| --- | --- | --- |
| `down` | number, **Mbit/s (decimal)** | Final download throughput, `downBytes × 8 ÷ (downNs ÷ 1e9) ÷ 1e6`. Same unit as `DOWNLOAD_RESULT.down`; equals the submitted `test_speed_download` (kbit/s) ÷ 1000 up to rounding. |
| `up` | number, **Mbit/s (decimal)** | Final upload throughput, analogous. |
| `pingMedian` | number, **ms** | Median of the **server-measured** RTTs (`pingServer` of the `PING_RESULT` samples, `pings[].value_server` in the submission); mean of the two middle values for an even count. |
| `pingCount` | integer | Number of ping samples. |
| `downBytes` / `upBytes` | integer, bytes | Total bytes of all threads (`test_bytes_download` / `test_bytes_upload`). |
| `downNs` / `upNs` | integer, ns | Phase duration (`test_nsec_download` / `test_nsec_upload`). |
| `downThreads` / `upThreads` | integer | Threads that completed the phase (threads that dropped out are excluded). |
| `testUuid`, `openTestUuid`, `loopUuid` | string \| null | As in `UUID_INFO`. |

The final values differ from the last interim `DOWNLOAD_RESULT`/`UPLOAD_RESULT`
(the interim monitor's clock also covers connection set-up), so consumers MUST
use `FINAL_RESULT`, not the last interim message, as the result of the run.

### 3.8 `SUBMIT_RESULT`

Emitted once, after the result POST to the control server (§7), between
`SUBMITTING_RESULTS` and `END`. A failed submission does **not** fail the run:
the client still emits `END` and exits `0`. *(Addition of this spec.)*

```json
{"type":"SUBMIT_RESULT","time":1789…,"success":true,"httpStatus":200,"error":null}
{"type":"SUBMIT_RESULT","time":1789…,"success":false,"httpStatus":503,"error":"result submission returned HTTP 503"}
```

| Field | Type | Notes |
| --- | --- | --- |
| `success` | boolean | `true` if the control server accepted the result (HTTP < 400 and an empty `error` array in its response). |
| `httpStatus` | integer \| null | HTTP status of the response; `null` if no response was received (e.g. connection failure). |
| `error` | string \| null | Reason on failure (transport error, HTTP status, or the server's `error` array); `null` on success. |

When `success` is `false`, the result cannot be fetched from the control server
by `testUuid`; the app should show the `FINAL_RESULT` values and mark them as not
uploaded.

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
| `-set-version` | `4.1.0` | Wrapping app version; reported as result `device`, prefixed `App: ` (→ `"App: 4.1.0"`). |
| `-v` | *(flag)* | Enable the JSON interface (§1.4). |
| `-u` | `<uuid>` | Client UUID; omitted on first run → obtained from the server. |

Loop mode (sent only when looping):

| Flag | Value | Meaning |
| --- | --- | --- |
| `--user-loop-mode` | *(flag)* | Enable loop mode. |
| `--user-loop-mode-max-delay` | minutes | Max delay between iterations. |
| `--user-loop-mode-test-counter` | integer | 0-based iteration counter. |
| `--user-loop-mode-uuid` | `<uuid>` | Server loop UUID; **omitted on the first iteration** (server mints it — never client-generated). Accepted with or without the `L` prefix (§4.1). |

### 4.1 Loop UUID round-trip

The client runs **one** measurement per invocation; the caller (desktop app)
re-spawns it per iteration. Loop membership is established at registration: when
`--user-loop-mode` is set, the client adds a `loopmode_info` object to the
`testRequest`, which is what tells the control server to group the iterations.

```jsonc
{
  "measurement_type": "LOOP_ACTIVE",   // "REGULAR" when not looping
  "loopmode": true,                     // omitted when not looping
  "loopmode_info": {
    "max_delay":    0,                  // --user-loop-mode-max-delay (minutes)
    "max_movement": 0,                  // always 0: no signal/GPS measurement here
    "max_tests":    0,                  // 0 = unbounded/unknown
    "test_counter": 0,                  // --user-loop-mode-test-counter
    "loop_uuid":    null                // null on iteration 0 → server mints one
  }
}
```

Round-trip (matches the historic Android/rmbt-client engine). **The loop UUID is
minted by the control server and MUST NOT be generated by the client or its
caller** — mirroring the Android model, where the client-local loop id never
leaves the device and only the server-minted UUID travels on the wire:

1. **First iteration** — `--user-loop-mode-uuid` **must be omitted** (passing an
   empty value or the literal `null` is treated the same), so `loop_uuid` is sent
   as JSON `null`. The server mints a loop UUID (`"L…"`), returns it as
   `loop_uuid` in the response, and the client surfaces it (`UUID_INFO.loopUuid`,
   plus a `Loop UUID: …` line on stdout).
2. **The caller persists that UUID** and passes it as `--user-loop-mode-uuid`
   on every subsequent iteration (with an incremented `--user-loop-mode-test-counter`).
3. The server returns the `loop_uuid` on each response, grouping the tests.

**The `L` prefix:** the control server mints the UUID with a leading `L` and
returns the `L`-prefixed form in every response (`loop_uuid`). On the wire in the
request, the bare form (no `L`) is expected. Clients therefore **strip a single
leading `L`** from the `--user-loop-mode-uuid` argument, so the caller may pass
the value back either as received (`L…`) or bare — both are accepted and the bare
form is sent. The DB groups by the bare UUID, so either works.

A regular (non-loop) test sends `measurement_type: "REGULAR"` and **no**
`loopmode_info`/`loop_uuid`. The per-test result submission (§7) does not
re-send the loop UUID — membership is already recorded server-side at
registration time via each test's `loop_uuid`.

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

* **Ping RTT (`pingClient`, `pingServer`, `pingMedian`): milliseconds** (floating
  point). The summary value (`FINAL_RESULT.pingMedian`, and the "Ping (median)"
  line of the human-readable output) is the median of the **server-measured**
  RTTs.
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

### 6.1 Failed tests

A test fails when any step up to and including the upload phase cannot be
completed (control server unreachable or rejecting the request, measurement
server unreachable, token rejected, protocol error, all threads of a phase
failing, …). The client then:

1. emits `STATE_CHANGE` with `state:"ERROR"`, `phase` (the state it was in) and
   `error` (§3.2);
2. emits **no** `FINAL_RESULT` and no `END`;
3. prints the `ENDING TEST.` sentinel (§2);
4. exits non-zero.

A failure of the result submission alone is **not** a failed test (§3.8). If
some threads of a phase drop out but at least one completes, the test succeeds
with fewer threads (`FINAL_RESULT.downThreads`/`upThreads`).

How a consumer determines the outcome:

| Outcome | Observed |
| --- | --- |
| Success | `FINAL_RESULT` → `SUBMIT_RESULT` (`success:true`) → `STATE_CHANGE:END`, exit `0` |
| Success, result not uploaded | as above, but `SUBMIT_RESULT` with `success:false` |
| Failed | `STATE_CHANGE:ERROR` (with `phase`, `error`), exit ≠ `0` |
| Crashed / killed | process ends without `END` or `ERROR` → treat as failed |

---

## 7. Result submission (control server)

The client MUST submit the complete result to the control server itself (the
existing `/result` POST — historic `RMBTClient.sendResult()`) and reports the
outcome as `SUBMIT_RESULT` (§3.8). The JSON stream carries live progress plus the
locally measured summary (`FINAL_RESULT`, §3.7), so the app can show the result
immediately and without a further network round trip. The control server's
result, retrieved **by `testUuid`** (from `UUID_INFO`), remains authoritative and
carries additional data (e.g. provider, location).

*(Historic: the fork's JSON stream carried only live progress; the app had to
fetch every result from the control server.)*

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
{"type":"FINAL_RESULT","down":…,"up":…,"pingMedian":…,…}
{"type":"STATE_CHANGE","time":…,"state":"SUBMITTING_RESULTS"}
{"type":"SUBMIT_RESULT","success":true,"httpStatus":200,"error":null}
{"type":"STATE_CHANGE","time":…,"state":"END"}
ENDING TEST.
(exit 0)
```

Failed test (e.g. measurement server unreachable during the pre-test):

```
STARTING TEST.
{"type":"STATE_CHANGE","time":…,"state":"INIT"}
{"type":"UUID_INFO",…}
{"type":"STATE_CHANGE","time":…,"state":"INIT_DOWN"}
{"type":"STATE_CHANGE","time":…,"state":"ERROR","phase":"INIT_DOWN","error":"…"}
ENDING TEST.
(exit 1)
```

---

## 9. Deviations from the historic fork

Intentional differences between this spec and the JSON-enabled fork:

| Topic | Historic fork | This spec |
| --- | --- | --- |
| Lifecycle signal | Plain-text `ENDING TEST.` is what the app keys on | `STATE_CHANGE:"END"` is authoritative; `STARTING/ENDING TEST.` kept only as a compat shim (§2) |
| Throughput unit | Producer already emits decimal Mbit/s (÷1 000 000); the **desktop consumer** mis-scales with 1024² | Clients emit decimal Mbit/s (÷1 000 000); the 1024-based scaling is a historic consumer bug and MUST NOT be reproduced (§5) |
| QoS | `QOS_RESULT` emitted during QoS | Optional; emit only if QoS is implemented (§3.6) |
| Final result | Not in the stream; app fetches it from the control server | `FINAL_RESULT` with down/up/ping median emitted before submission (§3.7) |
| Submission outcome | Not reported | `SUBMIT_RESULT` (§3.8) |
| Failure reporting | `ERROR` state only | `STATE_CHANGE:ERROR` carries `phase` and `error`; `ENDING TEST.` still printed; exit ≠ 0 (§6.1) |

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

**These clients.** `clientJava`, `clientC`, and `clientRust` implement §1–§7
behind `-v`, including the additions `FINAL_RESULT`, `SUBMIT_RESULT` and the
extended `ERROR` reporting.

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
- [ ] Emit `FINAL_RESULT` (§3.7) after the upload phase, before
      `SUBMITTING_RESULTS`; ping as server-RTT median.
- [ ] Submit the full result to the control server (already implemented) so the
      app can fetch it by `testUuid`, and report the outcome as `SUBMIT_RESULT`
      (§3.8).
- [ ] On failure emit `STATE_CHANGE` `ERROR` with `phase` and `error`, print
      `ENDING TEST.`, exit non-zero (§6.1).
- [ ] In loop mode, send `loopmode_info` in the `testRequest` and surface the
      returned `loop_uuid` (§4.1).
- [ ] Exit `0` on success, non-zero on error.
