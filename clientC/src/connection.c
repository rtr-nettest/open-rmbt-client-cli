#include "connection.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>

#define SOCKET_TIMEOUT_SECS 30
#define MAX_LINE            1024

/* ── Low-level I/O helpers ──────────────────────────────────────────────────── */

static ssize_t raw_read(RmbtConn *c, void *buf, size_t len)
{
    if (c->ssl)
        return SSL_read(c->ssl, buf, (int)len);
    return read(c->fd, buf, len);
}

static ssize_t raw_write(RmbtConn *c, const void *buf, size_t len)
{
    if (c->ssl)
        return SSL_write(c->ssl, buf, (int)len);
    return write(c->fd, buf, len);
}

static int write_all(RmbtConn *c, const void *buf, size_t len)
{
    const unsigned char *p = buf;
    while (len) {
        ssize_t n = raw_write(c, p, len);
        if (n <= 0) { perror("write"); return -1; }
        p   += n;
        len -= n;
    }
    return 0;
}

/* ── WebSocket framing ──────────────────────────────────────────────────────── */

#define WS_OP_TEXT   0x1
#define WS_OP_BINARY 0x2
#define WS_OP_CLOSE  0x8
#define WS_OP_PING   0x9
#define WS_OP_PONG   0xA

/* Send a single (client→server, masked) WebSocket frame. */
static int ws_send_frame(RmbtConn *c, int opcode, const unsigned char *payload, size_t plen)
{
    unsigned char header[14];
    int hlen = 0;

    header[hlen++] = 0x80 | (opcode & 0x0F);  /* FIN=1 */

    uint8_t mask_key[4];
    RAND_bytes(mask_key, 4);

    if (plen <= 125) {
        header[hlen++] = 0x80 | (uint8_t)plen;
    } else if (plen <= 65535) {
        header[hlen++] = 0x80 | 126;
        header[hlen++] = (plen >> 8) & 0xFF;
        header[hlen++] = plen & 0xFF;
    } else {
        header[hlen++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--)
            header[hlen++] = (plen >> (i * 8)) & 0xFF;
    }
    header[hlen++] = mask_key[0];
    header[hlen++] = mask_key[1];
    header[hlen++] = mask_key[2];
    header[hlen++] = mask_key[3];

    if (write_all(c, header, hlen) < 0) return -1;

    /* Mask and send payload in chunks to avoid a large allocation. */
    unsigned char chunk[4096];
    size_t sent = 0;
    while (sent < plen) {
        size_t batch = plen - sent;
        if (batch > sizeof(chunk)) batch = sizeof(chunk);
        for (size_t i = 0; i < batch; i++)
            chunk[i] = payload[sent + i] ^ mask_key[(sent + i) & 3];
        if (write_all(c, chunk, batch) < 0) return -1;
        sent += batch;
    }
    return 0;
}

/* Read one complete WebSocket frame.  Handles fragmentation and control frames.
 * Binary payload is written to *out_buf (caller must free).
 * Returns opcode on success, -1 on error. */
static int ws_recv_frame(RmbtConn *c, unsigned char **out_buf, size_t *out_len)
{
    *out_buf = NULL;
    *out_len = 0;

    /* Accumulate fragments. */
    unsigned char *acc = NULL;
    size_t acc_len = 0;
    int final_op = -1;

    for (;;) {
        unsigned char b0, b1;
        if (raw_read(c, &b0, 1) != 1) goto err;
        if (raw_read(c, &b1, 1) != 1) goto err;

        int fin    = (b0 >> 7) & 1;
        int opcode = b0 & 0x0F;
        int masked = (b1 >> 7) & 1;
        uint64_t plen = b1 & 0x7F;

        if (plen == 126) {
            unsigned char ext[2];
            if (raw_read(c, ext, 2) != 2) goto err;
            plen = ((uint64_t)ext[0] << 8) | ext[1];
        } else if (plen == 127) {
            unsigned char ext[8];
            if (raw_read(c, ext, 8) != 8) goto err;
            plen = 0;
            for (int i = 0; i < 8; i++) plen = (plen << 8) | ext[i];
        }

        uint8_t mkey[4] = {0};
        if (masked) {
            if (raw_read(c, mkey, 4) != 4) goto err;
        }

        unsigned char *payload = NULL;
        if (plen > 0) {
            payload = malloc(plen);
            if (!payload) goto err;
            size_t got = 0;
            while (got < plen) {
                ssize_t n = raw_read(c, payload + got, plen - got);
                if (n <= 0) { free(payload); goto err; }
                got += n;
            }
            if (masked) {
                for (size_t i = 0; i < plen; i++)
                    payload[i] ^= mkey[i & 3];
            }
        }

        /* Handle control frames inline. */
        if (opcode == WS_OP_PING) {
            ws_send_frame(c, WS_OP_PONG, payload, plen);
            free(payload);
            continue;
        }
        if (opcode == WS_OP_CLOSE) {
            free(payload);
            goto err;
        }

        /* Data frame or continuation. */
        if (opcode != 0) final_op = opcode; /* first or unfragmented frame */

        if (plen > 0) {
            acc = realloc(acc, acc_len + plen);
            if (!acc) { free(payload); goto err; }
            memcpy(acc + acc_len, payload, plen);
            acc_len += plen;
            free(payload);
        }

        if (fin) break;
    }

    *out_buf = acc;
    *out_len = acc_len;
    return final_op;

err:
    free(acc);
    return -1;
}

/* ── Line I/O (protocol-aware) ──────────────────────────────────────────────── */

int conn_read_line(RmbtConn *c, char *buf, size_t maxlen)
{
    if (c->protocol == PROTO_WS) {
        unsigned char *payload;
        size_t plen;
        int op = ws_recv_frame(c, &payload, &plen);
        if (op < 0) return -1;
        size_t copy = plen < maxlen - 1 ? plen : maxlen - 1;
        if (payload) memcpy(buf, payload, copy);
        buf[copy] = '\0';
        free(payload);
        /* Strip trailing \r\n */
        size_t l = strlen(buf);
        while (l > 0 && (buf[l-1] == '\n' || buf[l-1] == '\r')) buf[--l] = '\0';
        return 0;
    }

    /* HTTP (raw): byte-by-byte line read with 1-byte peek buffer. */
    size_t pos = 0;
    for (;;) {
        unsigned char ch;
        ssize_t n;
        if (c->peek_valid) {
            ch = c->peek;
            c->peek_valid = 0;
        } else {
            n = raw_read(c, &ch, 1);
            if (n <= 0) return -1;
        }
        if (ch == '\n') break;
        if (ch == '\r') continue;
        if (pos < maxlen - 1) buf[pos++] = ch;
    }
    buf[pos] = '\0';
    return 0;
}

int conn_write_line(RmbtConn *c, const char *line)
{
    if (c->protocol == PROTO_WS) {
        size_t len = strlen(line);
        /* Include the trailing \n the server expects. */
        char *tmp = malloc(len + 2);
        if (!tmp) return -1;
        memcpy(tmp, line, len);
        tmp[len]   = '\n';
        tmp[len+1] = '\0';
        int r = ws_send_frame(c, WS_OP_TEXT, (unsigned char *)tmp, len + 1);
        free(tmp);
        return r;
    }
    /* HTTP (raw): combine line + newline into a single write_all so OpenSSL
     * creates one TLS record; two separate writes produce two records and
     * Nagle holds the second until the first is ACKed (~40 ms). */
    size_t len = strlen(line);
    char tmp[MAX_LINE + 1];
    if (len >= MAX_LINE) return -1;
    memcpy(tmp, line, len);
    tmp[len] = '\n';
    return write_all(c, tmp, len + 1);
}

int conn_read_exact(RmbtConn *c, unsigned char *buf, size_t len)
{
    if (c->protocol == PROTO_WS) {
        size_t filled = 0;
        while (filled < len) {
            unsigned char *payload;
            size_t plen;
            int op = ws_recv_frame(c, &payload, &plen);
            if (op < 0) return -1;
            if (op == WS_OP_BINARY || op == WS_OP_TEXT) {
                size_t copy = plen;
                if (filled + copy > len) copy = len - filled;
                if (payload) memcpy(buf + filled, payload, copy);
                filled += copy;
            }
            free(payload);
        }
        return 0;
    }

    /* Raw: read exact bytes, but must not use the line peek buffer mid-chunk. */
    size_t got = 0;
    while (got < len) {
        ssize_t n = raw_read(c, buf + got, len - got);
        if (n <= 0) return -1;
        got += n;
    }
    return 0;
}

int conn_write_bytes(RmbtConn *c, const unsigned char *buf, size_t len)
{
    if (c->protocol == PROTO_WS)
        return ws_send_frame(c, WS_OP_BINARY, buf, len);
    return write_all(c, buf, len);
}

int conn_flush(RmbtConn *c)
{
    /* OpenSSL and raw sockets have no application-level flush needed here. */
    (void)c;
    return 0;
}

/* ── HTTP headers reader ─────────────────────────────────────────────────────── */

static int read_http_headers(RmbtConn *c, char *out, size_t maxlen)
{
    size_t pos = 0;
    unsigned char prev[3] = {0};
    for (;;) {
        unsigned char ch;
        if (raw_read(c, &ch, 1) != 1) return -1;
        if (pos < maxlen - 1) out[pos++] = ch;
        /* Shift tail */
        prev[0] = prev[1]; prev[1] = prev[2]; prev[2] = ch;
        /* Detect \r\n\r\n */
        if (pos >= 4 &&
            out[pos-4] == '\r' && out[pos-3] == '\n' &&
            out[pos-2] == '\r' && out[pos-1] == '\n') break;
        if (pos >= 8192) { fprintf(stderr, "HTTP headers too large\n"); return -1; }
    }
    out[pos] = '\0';
    (void)prev;
    return 0;
}

/* ── HTTP upgrade ────────────────────────────────────────────────────────────── */

static int http_upgrade(RmbtConn *c, const char *host)
{
    char req[512];
    snprintf(req, sizeof(req),
        "GET /rmbt HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: RMBT\r\n"
        "RMBT-Version: 1.3.5\r\n"
        "\r\n", host);
    if (write_all(c, req, strlen(req)) < 0) return -1;

    char headers[8192];
    if (read_http_headers(c, headers, sizeof(headers)) < 0) return -1;
    if (!strstr(headers, "101")) {
        char first[128] = "";
        sscanf(headers, "%127[^\r\n]", first);
        fprintf(stderr, "Expected HTTP 101 for RMBT upgrade, got: %s\n", first);
        return -1;
    }
    return 0;
}

/* ── WebSocket upgrade ───────────────────────────────────────────────────────── */

static void base64_encode(const unsigned char *in, size_t len, char *out)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0, j = 0;
    while (i < len) {
        uint32_t octet_a = i < len ? in[i++] : 0;
        uint32_t octet_b = i < len ? in[i++] : 0;
        uint32_t octet_c = i < len ? in[i++] : 0;
        uint32_t triple  = (octet_a << 16) | (octet_b << 8) | octet_c;
        out[j++] = tbl[(triple >> 18) & 0x3F];
        out[j++] = tbl[(triple >> 12) & 0x3F];
        out[j++] = tbl[(triple >>  6) & 0x3F];
        out[j++] = tbl[ triple        & 0x3F];
    }
    static const int mod_table[] = {0, 2, 1};
    for (int k = 0; k < mod_table[len % 3]; k++) out[j - 1 - k] = '=';
    out[j] = '\0';
}

static int ws_upgrade(RmbtConn *c, const char *host)
{
    unsigned char key_bytes[16];
    RAND_bytes(key_bytes, sizeof(key_bytes));
    char key_b64[25];
    base64_encode(key_bytes, sizeof(key_bytes), key_b64);

    char req[768];
    snprintf(req, sizeof(req),
        "GET /rmbt HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "\r\n", host, key_b64);
    if (write_all(c, req, strlen(req)) < 0) return -1;

    char headers[8192];
    if (read_http_headers(c, headers, sizeof(headers)) < 0) return -1;
    if (!strstr(headers, "101")) {
        char first[128] = "";
        sscanf(headers, "%127[^\r\n]", first);
        fprintf(stderr, "Expected HTTP 101 for WS upgrade, got: %s\n", first);
        return -1;
    }
    return 0;
}

/* ── TLS setup ───────────────────────────────────────────────────────────────── */

static SSL_CTX *make_ssl_ctx(int no_verify)
{
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return NULL;
    if (no_verify) {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    } else {
        SSL_CTX_set_default_verify_paths(ctx);
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    }
    return ctx;
}

/* ── Public API ──────────────────────────────────────────────────────────────── */

RmbtConn *conn_connect(const char *host, uint16_t port,
                       int use_tls, int no_tls_verify, int protocol)
{
    /* Resolve host */
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        fprintf(stderr, "Cannot resolve %s\n", host);
        return NULL;
    }

    int fd = -1;
    for (struct addrinfo *rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return NULL;
    }

    /* Socket timeouts */
    struct timeval tv = { SOCKET_TIMEOUT_SECS, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    /* Disable Nagle's algorithm so small writes (e.g. "PING\n") are sent
     * immediately; two-write sequences otherwise stall ~40 ms due to the
     * server-side delayed-ACK timer. */
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    RmbtConn *c = calloc(1, sizeof(*c));
    if (!c) { close(fd); return NULL; }
    c->fd            = fd;
    c->protocol      = protocol;
    c->chunk_size     = 4096;
    c->chunk_size_min = 1024;
    c->chunk_size_max = 4 * 1024 * 1024;

    if (use_tls) {
        c->ssl_ctx = make_ssl_ctx(no_tls_verify);
        if (!c->ssl_ctx) goto fail;
        c->ssl = SSL_new(c->ssl_ctx);
        if (!c->ssl) goto fail;
        SSL_set_fd(c->ssl, fd);
        SSL_set_tlsext_host_name(c->ssl, host);
        if (SSL_connect(c->ssl) != 1) {
            ERR_print_errors_fp(stderr);
            goto fail;
        }
    }

    /* HTTP or WebSocket upgrade */
    if (protocol == PROTO_WS) {
        if (ws_upgrade(c, host) < 0) goto fail;
    } else {
        if (http_upgrade(c, host) < 0) goto fail;
    }

    return c;

fail:
    conn_free(c);
    return NULL;
}

int conn_greeting(RmbtConn *c, const char *token)
{
    char line[MAX_LINE];

    /* Server sends: RMBTv<version> */
    if (conn_read_line(c, line, sizeof(line)) < 0) return -1;
    /* Skip leading null bytes and whitespace (some server implementations prepend \x00) */
    const char *vp = line;
    while (*vp == '\0' || *vp == ' ' || *vp == '\t') vp++;
    if (strncmp(vp, "RMBTv", 5) != 0) {
        fprintf(stderr, "Unexpected greeting: %s\n", line);
        return -1;
    }

    /* Server sends: ACCEPT TOKEN QUIT */
    if (conn_read_line(c, line, sizeof(line)) < 0) return -1;
    if (!strstr(line, "TOKEN")) {
        fprintf(stderr, "Server did not offer TOKEN: %s\n", line);
        return -1;
    }

    /* Client sends: TOKEN <token> */
    char tok_line[600];
    snprintf(tok_line, sizeof(tok_line), "TOKEN %s", token);
    if (conn_write_line(c, tok_line) < 0) return -1;

    /* Server sends: OK */
    if (conn_read_line(c, line, sizeof(line)) < 0) return -1;
    if (strcmp(line, "OK") != 0) {
        fprintf(stderr, "Token rejected: %s\n", line);
        return -1;
    }

    /* Server sends: CHUNKSIZE <default> [<min> <max>] */
    if (conn_read_line(c, line, sizeof(line)) < 0) return -1;
    if (strncmp(line, "CHUNKSIZE", 9) == 0) {
        size_t a = 0, b = 0, d = 0;
        int n = sscanf(line + 9, " %zu %zu %zu", &a, &b, &d);
        if (n >= 1 && a) c->chunk_size     = a;
        if (n >= 2 && b) c->chunk_size_min = b;
        if (n >= 3 && d) c->chunk_size_max = d;
    }

    return 0;
}

int conn_quit(RmbtConn *c)
{
    char line[MAX_LINE];
    conn_read_line(c, line, sizeof(line)); /* discard ACCEPT line */
    conn_write_line(c, "QUIT");
    conn_read_line(c, line, sizeof(line)); /* discard BYE */
    return 0;
}

void conn_free(RmbtConn *c)
{
    if (!c) return;
    if (c->ssl) {
        SSL_shutdown(c->ssl);
        SSL_free(c->ssl);
    }
    if (c->ssl_ctx) SSL_CTX_free(c->ssl_ctx);
    if (c->fd >= 0)  close(c->fd);
    free(c);
}
