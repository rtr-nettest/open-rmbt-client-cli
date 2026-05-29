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
| `--help` | Print help |

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
