# rmbt-client (Rust)

RMBT network measurement client written in Rust. Performs ping, download, and upload phases against an RMBT measurement server and submits results to the control server.

## Requirements

- Rust 1.70+ (uses the 2021 edition)
- Cargo (included with Rust)

Install Rust via [rustup](https://rustup.rs/):

```sh
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
```

## Build

```sh
cd clientRust
cargo build --release
```

The binary is written to `target/release/rmbt-client`.

For a debug build (faster compile, slower runtime):

```sh
cargo build
# binary at target/debug/rmbt-client
```

## Usage

```sh
./target/release/rmbt-client --host https://measure.example.com
```

Run with a specific thread count and duration:

```sh
./target/release/rmbt-client --host https://measure.example.com --threads 4 --duration 10
```

Skip TLS verification against a local test server:

```sh
./target/release/rmbt-client --host https://localhost:8080 --no-tls-verify
```

### Options

| Flag | Description |
|------|-------------|
| `-h`, `--host URL` | Control server base URL **(required)** |
| `-p`, `--port PORT` | Override test server port |
| `-u`, `--uuid UUID` | Client UUID (uses/creates `~/.rmbt_client_uuid` if omitted) |
| `-t`, `--threads N` | Force thread count for download and upload (overrides pre-test) |
| `-d`, `--duration SECS` | Test duration in seconds (default: from control server) |
| `--ws` | Use WebSocket (RMBTws) framing instead of plain HTTP upgrade |
| `--http` | Use plain HTTP upgrade (RMBThttp) — overrides auto-detection |
| `--no-tls-verify` | Skip TLS certificate verification (insecure) |
| `--debug` | Print raw control server request and response JSON |
| `--intermediate` | Print intermediate upload throughput every 40 ms per thread |
| `--help` | Print help |

### JSON progress interface (desktop/GUI integration)

These options make the client speak the machine-readable line protocol consumed
by [`open-rmbt-desktop`](https://github.com/rtr-nettest/open-rmbt-desktop). The
full contract is specified in [`../doc/json_interface.md`](../doc/json_interface.md).

| Flag | Description |
|------|-------------|
| `-v`, `--verbose` | Emit machine-readable JSON progress messages on stdout (see below). Without it, only human-readable output is printed. |
| `--nettype CODE` | Network-type code reported to the control server (default `98` = LAN) |
| `--type TYPE` | Client type reported to the control server (default `CLI`) |
| `--platform PLATFORM` | Platform label reported to the control server |
| `--os OS` | Operating-system string (accepted for compatibility) |
| `--osver VER` | Operating-system version (accepted for compatibility) |
| `--model MODEL` | Device model reported to the control server |
| `--set-version VER` | Override the reported client software version (`client_version`). Also accepted as `-set-version` (single dash) for desktop compatibility. |
| `--user-loop-mode` | Accepted for compatibility; loop mode is not yet implemented (runs a single test) |
| `--user-loop-mode-max-delay MIN` | Accepted for compatibility (ignored) |
| `--user-loop-mode-test-counter N` | Accepted for compatibility (ignored) |
| `--user-loop-mode-uuid UUID` | Accepted for compatibility (ignored) |

When `-v` is set, the client prints one JSON object per line on stdout, bracketed
by the plain-text sentinels `STARTING TEST.` and `ENDING TEST.`. Message `type`s:

| `type` | Emitted | Key fields |
|--------|---------|------------|
| `UUID_INFO` | once the test is registered | `testUuid`, `openTestUuid`, `testToken` |
| `STATE_CHANGE` | on every phase transition | `state` (`INIT`→`INIT_DOWN`→`PING`→`DOWN`→`INIT_UP`→`UP`→`SUBMITTING_RESULTS`→`END`), `time` |
| `PING_RESULT` | per ping sample | `pingClient`/`pingServer` (**ms**), `pingTimeNs` (**ns**) |
| `DOWNLOAD_RESULT` | ~every 250 ms during `DOWN` | `down` (**decimal Mbit/s**), `bytes` |
| `UPLOAD_RESULT` | ~every 250 ms during `UP` | `up` (**decimal Mbit/s**), `bytes` |

Example:

```sh
./target/release/rmbt-client -h measure.example.com \
    --platform Windows_NT --os "Windows_NT, 10.0" --model Desktop_x64 \
    --osver 10.0 --type DESKTOP --nettype 98 -set-version 2.0.1 -v
```

## Protocol

1. POST `/RMBTControlServer/settings` → register client, receive UUID
2. POST `/RMBTControlServer/testRequest` → receive token, server address, thread count
3. Pre-test: 2-second single-thread GETCHUNKS download to determine chunk size and thread counts
4. Ping: 1 s / 10–100 pings
5. Download: multi-threaded GETTIME, all threads start simultaneously via barrier
6. Upload: multi-threaded PUTNORESULT
7. POST `/RMBTControlServer/result`

Supports both **RMBThttp** (plain HTTP upgrade) and **RMBTws** (WebSocket) variants.  
TLS via rustls (ring backend); control server HTTPS via ureq.
