# rmbt-client (Java)

RMBT network measurement client written in Java. Performs ping, download, and upload phases against an RMBT measurement server and submits results to the control server.

## Requirements

- Java 25
- Maven 3.6+


## Build

```sh
cd java-client
mvn package -q
```

This produces a self-contained fat JAR at `target/rmbt-client-0.9.0.jar` via the Maven Shade plugin.

## Usage

```sh
java -jar target/rmbt-client-0.9.0.jar --host https://measure.example.com
```

Run with a specific thread count and duration:

```sh
java -jar target/rmbt-client-0.9.0.jar --host https://measure.example.com --threads 4 --duration 10
```

Skip TLS verification against a local test server:

```sh
java -jar target/rmbt-client-0.9.0.jar --host https://localhost:8080 --no-tls-verify
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
5. Download: multi-threaded GETTIME, all threads start simultaneously via `CyclicBarrier`
6. Upload: multi-threaded PUTNORESULT
7. POST `/RMBTControlServer/result`

Supports both **RMBThttp** (plain HTTP upgrade) and **RMBTws** (WebSocket) variants.  
TLS via the JDK's built-in `SSLContext`; control server HTTPS via `java.net.http.HttpClient`.
