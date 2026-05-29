use anyhow::{bail, Context, Result};
use base64::{engine::general_purpose::STANDARD as B64, Engine};
use std::io::{self, BufRead, BufReader, Read, Write};
use std::net::TcpStream;
use std::sync::Arc;
use std::time::Duration;
use tungstenite::{Message, WebSocket};

const SOCKET_TIMEOUT: Duration = Duration::from_secs(30);
const MAX_LINE: usize = 1024;

// ─── Protocol ─────────────────────────────────────────────────────────────────

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Protocol { Http, Ws }

// ─── Transport ────────────────────────────────────────────────────────────────

/// Wraps either a plain TCP stream or a TLS stream, implementing Read + Write.
enum Transport {
    Plain(TcpStream),
    Tls(rustls::StreamOwned<rustls::ClientConnection, TcpStream>),
}

impl Read for Transport {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        match self {
            Transport::Plain(s) => s.read(buf),
            Transport::Tls(s)   => s.read(buf),
        }
    }
}

impl Write for Transport {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        match self {
            Transport::Plain(s) => s.write(buf),
            Transport::Tls(s)   => s.write(buf),
        }
    }
    fn flush(&mut self) -> io::Result<()> {
        match self {
            Transport::Plain(s) => s.flush(),
            Transport::Tls(s)   => s.flush(),
        }
    }
}

// ─── ConnInner ────────────────────────────────────────────────────────────────

enum ConnInner {
    Raw(BufReader<Transport>),
    Ws(WebSocket<Transport>),
}

// ─── RmbtConn ─────────────────────────────────────────────────────────────────

/// A single connection to an RMBT test server, after HTTP upgrade and greeting.
pub struct RmbtConn {
    inner:              ConnInner,
    pub chunk_size:     usize,
    pub chunk_size_min: usize,
    pub chunk_size_max: usize,
}

impl RmbtConn {
    /// Connect, perform the appropriate upgrade (HTTP or WebSocket), then stop
    /// before the greeting.  Call `greeting()` next.
    pub fn connect(
        host:          &str,
        port:          u16,
        use_tls:       bool,
        no_tls_verify: bool,
        protocol:      Protocol,
    ) -> Result<Self> {
        let addr = format!("{host}:{port}");
        let tcp  = TcpStream::connect(&addr)
            .with_context(|| format!("cannot connect to {addr}"))?;
        tcp.set_read_timeout(Some(SOCKET_TIMEOUT))?;
        tcp.set_write_timeout(Some(SOCKET_TIMEOUT))?;
        tcp.set_nodelay(true)?;

        let mut transport = if use_tls {
            build_tls(tcp, host, no_tls_verify)?
        } else {
            Transport::Plain(tcp)
        };

        let inner = match protocol {
            Protocol::Http => {
                http_upgrade(&mut transport, host)?;
                ConnInner::Raw(BufReader::new(transport))
            }
            Protocol::Ws => {
                ws_upgrade(&mut transport, host)?;
                ConnInner::Ws(WebSocket::from_raw_socket(
                    transport,
                    tungstenite::protocol::Role::Client,
                    None,
                ))
            }
        };

        Ok(Self { inner, chunk_size: 4096, chunk_size_min: 1024, chunk_size_max: 4 * 1024 * 1024 })
    }

    /// Perform the RMBT greeting:
    /// receive version line + `ACCEPT TOKEN QUIT`, send `TOKEN`, receive `OK` +
    /// `CHUNKSIZE`.  Sets `self.chunk_size` from the server's announcement.
    pub fn greeting(&mut self, token: &str) -> Result<()> {
        let version = self.read_line()?;
        if !version.trim_start_matches(|c: char| c == '\0' || c.is_whitespace()).starts_with("RMBTv") {
            bail!("unexpected greeting: {version}");
        }

        let accept = self.read_line()?;
        if !accept.contains("TOKEN") {
            bail!("server did not offer TOKEN: {accept}");
        }

        self.write_line(&format!("TOKEN {token}"))?;

        let ok = self.read_line()?;
        if ok != "OK" {
            bail!("token rejected by server: {ok}");
        }

        let cs_line = self.read_line()?;
        if let Some(rest) = cs_line.strip_prefix("CHUNKSIZE ") {
            let parts: Vec<usize> = rest.split_whitespace()
                .filter_map(|s| s.parse().ok())
                .collect();
            if !parts.is_empty() { self.chunk_size     = parts[0]; }
            if parts.len() >= 2  { self.chunk_size_min = parts[1]; }
            if parts.len() >= 3  { self.chunk_size_max = parts[2]; }
        }

        Ok(())
    }

    /// Read and discard the `ACCEPT …` line the server sends before each command.
    pub fn read_accept(&mut self) -> Result<String> {
        self.read_line()
    }

    /// Send `QUIT` and wait for `BYE`.
    pub fn quit(&mut self) -> Result<()> {
        let _ = self.read_accept();
        self.write_line("QUIT")?;
        let _ = self.read_line();
        Ok(())
    }

    // ── Low-level I/O ────────────────────────────────────────────────────────

    pub fn read_line(&mut self) -> Result<String> {
        match &mut self.inner {
            ConnInner::Raw(r) => {
                let mut line = String::new();
                let n = r.read_line(&mut line)?;
                if n == 0 {
                    bail!("connection closed unexpectedly");
                }
                if line.len() > MAX_LINE {
                    bail!("protocol line too long ({} bytes)", line.len());
                }
                if line.ends_with('\n') { line.pop(); }
                if line.ends_with('\r') { line.pop(); }
                Ok(line)
            }
            ConnInner::Ws(ws) => loop {
                match ws.read().map_err(|e| anyhow::anyhow!("ws read: {e}"))? {
                    Message::Text(t) => {
                        let mut s = t.trim_end_matches('\n').trim_end_matches('\r').to_string();
                        if s.len() > MAX_LINE { bail!("protocol line too long ({} bytes)", s.len()); }
                        // trim trailing \n that server appends (e.g. "OK\n")
                        while s.ends_with('\n') || s.ends_with('\r') { s.pop(); }
                        return Ok(s);
                    }
                    Message::Binary(b) => {
                        let mut s = String::from_utf8_lossy(&b).into_owned();
                        while s.ends_with('\n') || s.ends_with('\r') { s.pop(); }
                        return Ok(s);
                    }
                    Message::Ping(d) => { ws.send(Message::Pong(d)).ok(); }
                    Message::Close(_) => bail!("WebSocket closed by server"),
                    _ => continue,
                }
            },
        }
    }

    pub fn write_line(&mut self, s: &str) -> Result<()> {
        match &mut self.inner {
            ConnInner::Raw(r) => {
                let w = r.get_mut();
                // Single write_all so rustls creates one TLS record for the
                // whole line; splitting into two calls can produce two records
                // and Nagle holds the second until the first is ACKed (~40ms).
                let mut buf = Vec::with_capacity(s.len() + 1);
                buf.extend_from_slice(s.as_bytes());
                buf.push(b'\n');
                w.write_all(&buf)?;
                w.flush()?;
            }
            ConnInner::Ws(ws) => {
                // Send as a Text frame; include the newline to match raw behaviour.
                ws.send(Message::Text(format!("{s}\n").into()))
                    .map_err(|e| anyhow::anyhow!("ws send: {e}"))?;
            }
        }
        Ok(())
    }

    /// Read exactly `buf.len()` bytes (used for fixed-size download chunks).
    pub fn read_exact(&mut self, buf: &mut [u8]) -> Result<()> {
        match &mut self.inner {
            ConnInner::Raw(r) => r.read_exact(buf).map_err(Into::into),
            ConnInner::Ws(ws) => {
                let mut filled = 0;
                while filled < buf.len() {
                    match ws.read().map_err(|e| anyhow::anyhow!("ws read: {e}"))? {
                        Message::Binary(b) => {
                            let n = b.len().min(buf.len() - filled);
                            buf[filled..filled + n].copy_from_slice(&b[..n]);
                            filled += n;
                        }
                        Message::Ping(d) => { ws.send(Message::Pong(d)).ok(); }
                        Message::Close(_) => bail!("WebSocket closed during download"),
                        _ => continue,
                    }
                }
                Ok(())
            }
        }
    }

    /// Write raw bytes without flushing (used for upload chunks).
    pub fn write_bytes(&mut self, data: &[u8]) -> Result<()> {
        match &mut self.inner {
            ConnInner::Raw(r) => r.get_mut().write_all(data).map_err(Into::into),
            ConnInner::Ws(ws) => {
                ws.send(Message::Binary(data.to_vec().into()))
                    .map_err(|e| anyhow::anyhow!("ws send: {e}"))
            }
        }
    }

    /// Flush any buffered data in the transport layer (needed after upload).
    pub fn flush(&mut self) -> Result<()> {
        match &mut self.inner {
            ConnInner::Raw(r) => r.get_mut().flush().map_err(Into::into),
            ConnInner::Ws(ws) => ws.get_mut().flush().map_err(Into::into),
        }
    }
}

// ─── HTTP upgrade (plain RMBT) ────────────────────────────────────────────────

/// Send `GET /rmbt HTTP/1.1` with `Upgrade: RMBT` and wait for HTTP 101.
///
/// Reads byte-by-byte until `\r\n\r\n` to avoid buffering bytes that belong
/// to the RMBT protocol stream that follows.
fn http_upgrade(transport: &mut Transport, host: &str) -> Result<()> {
    let req = format!(
        "GET /rmbt HTTP/1.1\r\n\
         Host: {host}\r\n\
         Connection: Upgrade\r\n\
         Upgrade: RMBT\r\n\
         RMBT-Version: 1.3.5\r\n\
         \r\n"
    );
    transport.write_all(req.as_bytes())?;
    transport.flush()?;

    let headers = read_http_headers(transport)?;
    let status  = headers.lines().next().unwrap_or("");
    if !status.contains("101") {
        bail!("expected HTTP 101 for RMBT upgrade, got: {status}");
    }
    Ok(())
}

// ─── WebSocket upgrade ────────────────────────────────────────────────────────

/// Perform the RFC 6455 client-side opening handshake and wait for HTTP 101.
///
/// Reads byte-by-byte until `\r\n\r\n` so that no bytes are consumed from the
/// WebSocket data stream before handing the transport to tungstenite.
fn ws_upgrade(transport: &mut Transport, host: &str) -> Result<()> {
    let mut key_bytes = [0u8; 16];
    fastrand::fill(&mut key_bytes);
    let key = B64.encode(key_bytes);

    let req = format!(
        "GET /rmbt HTTP/1.1\r\n\
         Host: {host}\r\n\
         Connection: Upgrade\r\n\
         Upgrade: websocket\r\n\
         Sec-WebSocket-Version: 13\r\n\
         Sec-WebSocket-Key: {key}\r\n\
         \r\n"
    );
    transport.write_all(req.as_bytes())?;
    transport.flush()?;

    let headers = read_http_headers(transport)?;
    let status  = headers.lines().next().unwrap_or("");
    if !status.contains("101") {
        bail!("expected HTTP 101 for WebSocket upgrade, got: {status}");
    }
    Ok(())
}

/// Read HTTP response headers byte-by-byte until the blank line (`\r\n\r\n`).
fn read_http_headers(transport: &mut Transport) -> Result<String> {
    let mut buf = Vec::with_capacity(512);
    let mut tmp = [0u8; 1];
    loop {
        transport.read_exact(&mut tmp)?;
        buf.push(tmp[0]);
        if buf.ends_with(b"\r\n\r\n") { break; }
        if buf.len() > 8192 {
            bail!("HTTP response headers too large (> 8 KiB)");
        }
    }
    Ok(String::from_utf8_lossy(&buf).into_owned())
}

// ─── TLS setup ────────────────────────────────────────────────────────────────

fn build_tls(tcp: TcpStream, host: &str, no_verify: bool) -> Result<Transport> {
    use rustls::ClientConfig;
    use rustls_pki_types::ServerName;

    let config: ClientConfig = if no_verify {
        ClientConfig::builder()
            .dangerous()
            .with_custom_certificate_verifier(Arc::new(NoVerify))
            .with_no_client_auth()
    } else {
        let root_store = rustls::RootCertStore {
            roots: webpki_roots::TLS_SERVER_ROOTS.to_vec(),
        };
        ClientConfig::builder()
            .with_root_certificates(root_store)
            .with_no_client_auth()
    };

    let server_name = ServerName::try_from(host.to_string())
        .context("invalid hostname for TLS SNI")?;

    let conn = rustls::ClientConnection::new(Arc::new(config), server_name)?;
    Ok(Transport::Tls(rustls::StreamOwned::new(conn, tcp)))
}

// ─── No-op certificate verifier (--no-tls-verify) ────────────────────────────

#[derive(Debug)]
struct NoVerify;

impl rustls::client::danger::ServerCertVerifier for NoVerify {
    fn verify_server_cert(
        &self,
        _end_entity:   &rustls_pki_types::CertificateDer<'_>,
        _intermediates: &[rustls_pki_types::CertificateDer<'_>],
        _server_name:  &rustls_pki_types::ServerName<'_>,
        _ocsp:         &[u8],
        _now:          rustls_pki_types::UnixTime,
    ) -> std::result::Result<rustls::client::danger::ServerCertVerified, rustls::Error> {
        Ok(rustls::client::danger::ServerCertVerified::assertion())
    }

    fn verify_tls12_signature(
        &self,
        _msg:  &[u8],
        _cert: &rustls_pki_types::CertificateDer<'_>,
        _sig:  &rustls::DigitallySignedStruct,
    ) -> std::result::Result<rustls::client::danger::HandshakeSignatureValid, rustls::Error> {
        Ok(rustls::client::danger::HandshakeSignatureValid::assertion())
    }

    fn verify_tls13_signature(
        &self,
        _msg:  &[u8],
        _cert: &rustls_pki_types::CertificateDer<'_>,
        _sig:  &rustls::DigitallySignedStruct,
    ) -> std::result::Result<rustls::client::danger::HandshakeSignatureValid, rustls::Error> {
        Ok(rustls::client::danger::HandshakeSignatureValid::assertion())
    }

    fn supported_verify_schemes(&self) -> Vec<rustls::SignatureScheme> {
        use rustls::SignatureScheme::*;
        vec![
            RSA_PKCS1_SHA256, RSA_PKCS1_SHA384, RSA_PKCS1_SHA512,
            ECDSA_NISTP256_SHA256, ECDSA_NISTP384_SHA384, ECDSA_NISTP521_SHA512,
            RSA_PSS_SHA256, RSA_PSS_SHA384, RSA_PSS_SHA512,
            ED25519,
        ]
    }
}
