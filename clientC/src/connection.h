#pragma once
#include <stdint.h>
#include <stddef.h>
#include <openssl/ssl.h>

#define PROTO_HTTP 0
#define PROTO_WS   1

typedef struct {
    int      fd;
    SSL     *ssl;
    SSL_CTX *ssl_ctx;
    int      protocol;      /* PROTO_HTTP or PROTO_WS */
    size_t   chunk_size;
    size_t   chunk_size_min;
    size_t   chunk_size_max;
    /* single-byte read-ahead buffer for line reading */
    unsigned char peek;
    int           peek_valid;
} RmbtConn;

/* Connect + HTTP/WS upgrade.  Returns NULL on error (message printed to stderr). */
RmbtConn *conn_connect(const char *host, uint16_t port,
                       int use_tls, int no_tls_verify, int protocol);

/* RMBT greeting: read version, send TOKEN, read OK + CHUNKSIZE. */
int conn_greeting(RmbtConn *c, const char *token);

/* Read one \n-terminated line.  Strips \r\n.  Returns 0 on success, -1 on error. */
int conn_read_line(RmbtConn *c, char *buf, size_t maxlen);

/* Write a line followed by \n and flush. */
int conn_write_line(RmbtConn *c, const char *line);

/* Read exactly len bytes into buf. */
int conn_read_exact(RmbtConn *c, unsigned char *buf, size_t len);

/* Write len bytes from buf (no flush — used for upload chunks). */
int conn_write_bytes(RmbtConn *c, const unsigned char *buf, size_t len);

/* Flush any buffered output. */
int conn_flush(RmbtConn *c);

/* Read and discard the ACCEPT line, then send QUIT, wait for BYE. */
int conn_quit(RmbtConn *c);

void conn_free(RmbtConn *c);
