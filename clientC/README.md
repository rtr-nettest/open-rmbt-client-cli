# rmbt-client (C)

RMBT network measurement client written in C. Performs ping, download, and upload phases against an RMBT measurement server and submits results to the control server.

## Requirements

- GCC or Clang
- Make
- OpenSSL development headers
- libcurl development headers

Install dependencies:

```sh
# Debian / Ubuntu
apt install gcc make libssl-dev libcurl4-openssl-dev

# Alpine
apk add gcc make musl-dev openssl-dev curl-dev

# RHEL / Fedora
dnf install gcc make openssl-devel libcurl-devel

# macOS (Homebrew)
brew install openssl curl
```

## Build

```sh
cd c-client
make
```

The binary is written to `build/rmbt-client`.

For a debug build:

```sh
make DEBUG=1
```

## Usage

```sh
./build/rmbt-client --host https://measure.example.com
```

Run with a specific thread count and duration:

```sh
./build/rmbt-client --host https://measure.example.com --threads 4 --duration 10
```

Skip TLS verification against a local test server:

```sh
./build/rmbt-client --host https://localhost:8080 --no-tls-verify
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
| `--debug` | Print control server request/response JSON |
| `--intermediate` | Print upload throughput every 40 ms per thread |
| `--set-version VER` | Wrapping app version; reported as the `device` field prefixed with `App: ` (e.g. `--set-version 4.1.0` → `device` = `"App: 4.1.0"`). Also accepted as `-set-version` (single dash) for desktop compatibility. |
| `-v`, `--verbose` | Emit machine-readable JSON progress messages on stdout (`UUID_INFO`, `STATE_CHANGE`, …). See [`../doc/json_interface.md`](../doc/json_interface.md). |
| `--user-loop-mode` | Mark this run as one iteration of a loop: sends a `loopmode_info` block so the control server groups the iterations. See [Loop mode](#loop-mode). |
| `--user-loop-mode-max-delay MIN` | Max waiting time between loop iterations, in minutes (`loopmode_info.max_delay`) |
| `--user-loop-mode-test-counter N` | 0-based index of this iteration within the loop (`loopmode_info.test_counter`) |
| `--user-loop-mode-uuid UUID` | Server loop UUID to echo on the **second and later** iterations. **Omit it on the first iteration** — see [Loop mode](#loop-mode). Accepted with or without the server's `L` prefix. |
| `--help` | Print help |

### Loop mode

In *loop mode* the client runs **one** measurement per invocation; the caller
(e.g. the desktop app) re-spawns it once per iteration. `--user-loop-mode` adds a
`loopmode_info` block to the test request so the control server groups all
iterations into a single *loop*.

**The loop UUID is minted by the control server — never by the client.** This
follows the model documented for the Android client: a client-local loop id is
never put on the wire; only the server-minted loop UUID is. Concretely:

1. **First iteration:** do **not** pass `--user-loop-mode-uuid` (and do not
   invent one). The client sends `loop_uuid: null`; the server mints the loop
   UUID and returns it. The client surfaces it as a `Loop UUID: <uuid>` line on
   stdout and, with `-v`, as the `loopUuid` field of the `UUID_INFO` message.
2. **Second and later iterations:** pass that server-returned value back via
   `--user-loop-mode-uuid`, incrementing `--user-loop-mode-test-counter` each
   time. The value is accepted **with or without** the server's `L` prefix — a
   single leading `L` is stripped before it is sent on the wire.

See [`../doc/json_interface.md`](../doc/json_interface.md) §4.1 for the full
round-trip specification.

## Protocol

1. POST `/RMBTControlServer/settings` → register client, receive UUID
2. POST `/RMBTControlServer/testRequest` → receive token, server address, thread count
3. Pre-test: 2-second single-thread GETCHUNKS download to determine chunk size and thread counts
4. Ping: 1 s / 10–100 pings
5. Download: multi-threaded GETTIME, all threads start simultaneously via `pthread_barrier_t`
6. Upload: multi-threaded PUTNORESULT
7. POST `/RMBTControlServer/result`

Supports both **RMBThttp** (plain HTTP upgrade) and **RMBTws** (WebSocket) variants.  
TLS via OpenSSL; control server HTTPS via libcurl.
