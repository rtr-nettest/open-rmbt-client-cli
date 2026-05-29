package at.rtr.rmbt.client;

import javax.net.ssl.*;
import java.io.*;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.security.SecureRandom;
import java.security.cert.X509Certificate;
import java.util.Base64;

/**
 * A single RMBT test-server connection after HTTP upgrade and greeting.
 * Supports both RMBThttp (plain HTTP upgrade) and RMBTws (WebSocket) framing.
 */
final class RmbtConn implements Closeable {

    static final int PROTO_HTTP = 0;
    static final int PROTO_WS   = 1;

    private static final int SOCKET_TIMEOUT_MS = 30_000;
    private static final int MAX_LINE          = 1024;

    private final Socket        socket;
    private final OutputStream  out;
    private final InputStream   in;
    private final int           protocol;

    int chunkSize    = 4096;
    int chunkSizeMin = 1024;
    int chunkSizeMax = 4 * 1024 * 1024;

    // ── Connect + upgrade ──────────────────────────────────────────────────────

    static RmbtConn connect(String host, int port, boolean useTls,
                            boolean noTlsVerify, int protocol) throws IOException {
        Socket socket;
        if (useTls) {
            SSLContext ctx = buildSslContext(noTlsVerify);
            SSLSocket sslSocket = (SSLSocket) ctx.getSocketFactory()
                    .createSocket(host, port);
            sslSocket.setEnabledProtocols(new String[]{"TLSv1.2", "TLSv1.3"});
            sslSocket.startHandshake();
            socket = sslSocket;
        } else {
            socket = new Socket();
            socket.connect(new InetSocketAddress(host, port), SOCKET_TIMEOUT_MS);
        }
        socket.setSoTimeout(SOCKET_TIMEOUT_MS);
        socket.setTcpNoDelay(true);

        RmbtConn conn = new RmbtConn(socket, protocol);
        if (protocol == PROTO_WS) {
            conn.wsUpgrade(host);
        } else {
            conn.httpUpgrade(host);
        }
        return conn;
    }

    private RmbtConn(Socket socket, int protocol) throws IOException {
        this.socket   = socket;
        this.out      = new BufferedOutputStream(socket.getOutputStream(), 65536);
        this.in       = socket.getInputStream();
        this.protocol = protocol;
    }

    // ── Greeting ───────────────────────────────────────────────────────────────

    void greeting(String token) throws IOException {
        /* Some server implementations prefix the greeting with \x00 — strip it. */
        String version = readLine().replaceAll("^[\\x00\\s]+", "");
        if (!version.startsWith("RMBTv"))
            throw new IOException("Unexpected greeting: " + version);

        String accept = readLine();
        if (!accept.contains("TOKEN"))
            throw new IOException("Server did not offer TOKEN: " + accept);

        writeLine("TOKEN " + token);

        String ok = readLine();
        if (!"OK".equals(ok))
            throw new IOException("Token rejected: " + ok);

        String csLine = readLine();
        if (csLine.startsWith("CHUNKSIZE")) {
            String[] parts = csLine.substring(9).trim().split("\\s+");
            if (parts.length >= 1) chunkSize     = Integer.parseInt(parts[0]);
            if (parts.length >= 2) chunkSizeMin  = Integer.parseInt(parts[1]);
            if (parts.length >= 3) chunkSizeMax  = Integer.parseInt(parts[2]);
        }
    }

    /** Read and discard the ACCEPT line, then send QUIT. */
    void quit() {
        try { readLine(); writeLine("QUIT"); readLine(); } catch (IOException ignored) {}
    }

    // ── Line I/O ───────────────────────────────────────────────────────────────

    String readLine() throws IOException {
        if (protocol == PROTO_WS) return wsReadTextFrame();

        // Raw: byte-by-byte until \n
        byte[] buf = new byte[MAX_LINE];
        int pos = 0;
        for (;;) {
            int b = in.read();
            if (b < 0) throw new EOFException("Connection closed");
            if (b == '\n') break;
            if (b == '\r') continue;
            if (pos < MAX_LINE - 1) buf[pos++] = (byte) b;
        }
        return new String(buf, 0, pos, StandardCharsets.UTF_8);
    }

    void writeLine(String s) throws IOException {
        if (protocol == PROTO_WS) { wsSendText(s + "\n"); return; }
        byte[] bytes = (s + "\n").getBytes(StandardCharsets.UTF_8);
        out.write(bytes);
        out.flush();
    }

    void readExact(byte[] buf) throws IOException {
        if (protocol == PROTO_WS) { wsReadBinaryInto(buf); return; }
        int got = 0;
        while (got < buf.length) {
            int n = in.read(buf, got, buf.length - got);
            if (n < 0) throw new EOFException("Connection closed during read");
            got += n;
        }
    }

    void writeBytes(byte[] data) throws IOException {
        if (protocol == PROTO_WS) { wsSendBinary(data); return; }
        out.write(data);
    }

    void flush() throws IOException {
        out.flush();
    }

    @Override
    public void close() {
        try { socket.close(); } catch (IOException ignored) {}
    }

    // ── HTTP upgrade ───────────────────────────────────────────────────────────

    private void httpUpgrade(String host) throws IOException {
        String req = "GET /rmbt HTTP/1.1\r\n" +
                     "Host: " + host + "\r\n" +
                     "Connection: Upgrade\r\n" +
                     "Upgrade: RMBT\r\n" +
                     "RMBT-Version: 1.3.5\r\n" +
                     "\r\n";
        out.write(req.getBytes(StandardCharsets.US_ASCII));
        out.flush();
        String status = readHttpHeaders();
        if (!status.contains("101"))
            throw new IOException("Expected HTTP 101 for RMBT upgrade, got: " + status.lines().findFirst().orElse("?"));
    }

    // ── WebSocket upgrade ──────────────────────────────────────────────────────

    private void wsUpgrade(String host) throws IOException {
        byte[] keyBytes = new byte[16];
        new SecureRandom().nextBytes(keyBytes);
        String key = Base64.getEncoder().encodeToString(keyBytes);

        String req = "GET /rmbt HTTP/1.1\r\n" +
                     "Host: " + host + "\r\n" +
                     "Connection: Upgrade\r\n" +
                     "Upgrade: websocket\r\n" +
                     "Sec-WebSocket-Version: 13\r\n" +
                     "Sec-WebSocket-Key: " + key + "\r\n" +
                     "\r\n";
        out.write(req.getBytes(StandardCharsets.US_ASCII));
        out.flush();
        String status = readHttpHeaders();
        if (!status.contains("101"))
            throw new IOException("Expected HTTP 101 for WS upgrade, got: " + status.lines().findFirst().orElse("?"));
    }

    private String readHttpHeaders() throws IOException {
        ByteArrayOutputStream buf = new ByteArrayOutputStream(512);
        int[] tail = new int[4];
        for (;;) {
            int b = in.read();
            if (b < 0) throw new EOFException("Connection closed reading HTTP headers");
            buf.write(b);
            tail[0] = tail[1]; tail[1] = tail[2]; tail[2] = tail[3]; tail[3] = b;
            if (tail[0] == '\r' && tail[1] == '\n' && tail[2] == '\r' && tail[3] == '\n') break;
            if (buf.size() > 8192) throw new IOException("HTTP headers too large");
        }
        return buf.toString(StandardCharsets.US_ASCII);
    }

    // ── WebSocket framing ──────────────────────────────────────────────────────

    private static final int WS_TEXT   = 0x1;
    private static final int WS_BINARY = 0x2;
    private static final int WS_PING   = 0x9;
    private static final int WS_PONG   = 0xA;
    private static final int WS_CLOSE  = 0x8;

    private void wsSendFrame(int opcode, byte[] payload) throws IOException {
        int plen = payload.length;
        ByteArrayOutputStream frame = new ByteArrayOutputStream(plen + 14);

        frame.write(0x80 | (opcode & 0x0F));    // FIN + opcode
        byte[] mask = new byte[4];
        new SecureRandom().nextBytes(mask);

        if (plen <= 125) {
            frame.write(0x80 | plen);
        } else if (plen <= 65535) {
            frame.write(0x80 | 126);
            frame.write((plen >> 8) & 0xFF);
            frame.write(plen & 0xFF);
        } else {
            frame.write(0x80 | 127);
            for (int i = 7; i >= 0; i--) frame.write((plen >> (i * 8)) & 0xFF);
        }
        frame.write(mask);

        byte[] masked = new byte[plen];
        for (int i = 0; i < plen; i++) masked[i] = (byte)(payload[i] ^ mask[i & 3]);
        frame.write(masked);

        out.write(frame.toByteArray());
        out.flush();
    }

    private void wsSendText(String s) throws IOException {
        wsSendFrame(WS_TEXT, s.getBytes(StandardCharsets.UTF_8));
    }

    private void wsSendBinary(byte[] data) throws IOException {
        wsSendFrame(WS_BINARY, data);
    }

    /** Read one complete WS message (handles fragmentation and pong replies). */
    private byte[] wsReadFrame() throws IOException {
        ByteArrayOutputStream acc = new ByteArrayOutputStream();

        for (;;) {
            int b0 = in.read(), b1 = in.read();
            if (b0 < 0 || b1 < 0) throw new EOFException("WS connection closed");

            boolean fin    = (b0 & 0x80) != 0;
            int     opcode = b0 & 0x0F;
            boolean masked = (b1 & 0x80) != 0;
            long    plen   = b1 & 0x7F;

            if (plen == 126) {
                plen = ((long)(in.read() & 0xFF) << 8) | (in.read() & 0xFF);
            } else if (plen == 127) {
                plen = 0;
                for (int i = 0; i < 8; i++) plen = (plen << 8) | (in.read() & 0xFF);
            }

            byte[] mkey = new byte[4];
            if (masked) readExactRaw(mkey);

            byte[] payload = new byte[(int) plen];
            readExactRaw(payload);
            if (masked) for (int i = 0; i < payload.length; i++) payload[i] ^= mkey[i & 3];

            if (opcode == WS_PING) { wsSendFrame(WS_PONG, payload); continue; }
            if (opcode == WS_CLOSE) throw new IOException("WebSocket closed by server");

            acc.write(payload);

            if (fin) break;
        }
        return acc.toByteArray();
    }

    private String wsReadTextFrame() throws IOException {
        String s = new String(wsReadFrame(), StandardCharsets.UTF_8);
        // Strip trailing \r\n
        int end = s.length();
        while (end > 0 && (s.charAt(end-1) == '\n' || s.charAt(end-1) == '\r')) end--;
        return s.substring(0, end);
    }

    private void wsReadBinaryInto(byte[] dest) throws IOException {
        int filled = 0;
        while (filled < dest.length) {
            byte[] chunk = wsReadFrame();
            int copy = Math.min(chunk.length, dest.length - filled);
            System.arraycopy(chunk, 0, dest, filled, copy);
            filled += copy;
        }
    }

    private void readExactRaw(byte[] buf) throws IOException {
        int got = 0;
        while (got < buf.length) {
            int n = in.read(buf, got, buf.length - got);
            if (n < 0) throw new EOFException();
            got += n;
        }
    }

    // ── TLS ───────────────────────────────────────────────────────────────────

    private static SSLContext buildSslContext(boolean noVerify) {
        try {
            SSLContext ctx = SSLContext.getInstance("TLS");
            TrustManager[] tms = noVerify
                ? new TrustManager[]{ new X509TrustManager() {
                    public X509Certificate[] getAcceptedIssuers() { return new X509Certificate[0]; }
                    public void checkClientTrusted(X509Certificate[] c, String a) {}
                    public void checkServerTrusted(X509Certificate[] c, String a) {}
                }}
                : null;  // use default JVM trust store
            ctx.init(null, tms, null);
            return ctx;
        } catch (Exception e) {
            throw new RuntimeException("Failed to create SSL context", e);
        }
    }
}
