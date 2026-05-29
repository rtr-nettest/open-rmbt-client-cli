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
cd client
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
