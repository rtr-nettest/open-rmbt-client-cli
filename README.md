RMBT Test Client in Rust
========================

This project contains the Rust implementation of the RMBT Test Client for conducting measurements based on
the RMBT protocol.


Usage
-----
```
./rmbt-client --help
RMBT network measurement client

Usage: rmbt-client [OPTIONS] --host <URL>

Options:
      --help             Print this help
  -h, --host <URL>       Control server base URL (e.g. https://measure.example.com)
  -p, --port <PORT>      Override test server port
  -u, --uuid <UUID>      Client UUID (leave empty on first run)
  -t, --threads <N>      Parallel test threads (default: from control server)
  -d, --duration <SECS>  Test duration in seconds (default: from control server)
      --ws               Use WebSocket (RMBTws) framing instead of plain HTTP upgrade
      --http             Use plain HTTP upgrade (RMBThttp) — overrides auto-detection
      --no-tls-verify    Skip TLS certificate verification for test server (insecure)
      --debug            Print raw control server request and response JSON
```

Get in Touch
------------

* [RTR-Netztest](https://www.netztest.at) on the web


License
-------

This source code is licensed under the Apache license found in
the [LICENSE.txt](https://github.com/rtr-nettest/rmbtws/blob/master/LICENSE.txt) file.
The documentation to the project is licensed under the [CC BY-AT 3.0](https://creativecommons.org/licenses/by/3.0/at/deed.de_AT)
license.
