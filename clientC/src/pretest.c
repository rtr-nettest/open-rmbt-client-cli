#include "pretest.h"
#include "connection.h"
#include "tests.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PRETEST_DURATION_NS 2000000000ULL   /* 2 s */
#define MIN_CHUNK           1024
#define MAX_CHUNK           (4 * 1024 * 1024)

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* Thread-count tables: {min_mbps, threads}, descending. */
static const double DL_THRESH[] = { 100.0, 1.0, 0.0 };
static const int    DL_COUNT[]  = {   5,   3,   1   };
static const double UL_THRESH[] = { 150.0, 80.0, 30.0, 0.0 };
static const int    UL_COUNT[]  = {   5,    3,    2,   1   };

static int threads_for(double mbps, const double *thresh, const int *count, int n)
{
    for (int i = 0; i < n; i++)
        if (mbps >= thresh[i]) return count[i];
    return 1;
}

int run_pretest(const char *addr, uint16_t port,
                int use_tls, int no_tls_verify, int protocol,
                const char *token, int max_threads,
                PretestResult *out)
{
    printf("\nPre-test: measuring baseline throughput...\n");

    RmbtConn *conn = conn_connect(addr, port, use_tls, no_tls_verify, protocol);
    if (!conn) return -1;
    if (conn_greeting(conn, token) < 0) { conn_free(conn); return -1; }

    size_t server_min = conn->chunk_size_min;
    size_t server_max = conn->chunk_size_max;

    uint64_t t_start    = now_ns();
    size_t   cs         = server_min > MIN_CHUNK ? server_min : MIN_CHUNK;
    int      n          = 1;
    uint64_t last_bytes = 0;
    uint64_t last_ns    = 0;  /* TIME from the last (largest) GETCHUNKS batch */
    uint64_t rtt_ns     = 0;  /* TIME of the first (tiny) batch ≈ round-trip time */

    char line[256];

    for (;;) {
        if (now_ns() - t_start >= PRETEST_DURATION_NS) break;

        if (conn_read_line(conn, line, sizeof(line)) < 0) break;
        if (!strstr(line, "GETCHUNKS")) {
            fprintf(stderr, "pre-test: expected ACCEPT with GETCHUNKS, got: %s\n", line);
            conn_free(conn);
            return -1;
        }

        char cmd[64];
        snprintf(cmd, sizeof(cmd), "GETCHUNKS %d %zu", n, cs);
        if (conn_write_line(conn, cmd) < 0) break;

        unsigned char *buf = malloc(cs);
        if (!buf) break;
        for (int i = 0; i < n; i++) {
            if (conn_read_exact(conn, buf, cs) < 0) {
                free(buf);
                goto done;
            }
        }
        free(buf);
        last_bytes = (uint64_t)n * cs;

        if (conn_write_line(conn, "OK") < 0) break;
        if (conn_read_line(conn, line, sizeof(line)) < 0) break;
        {
            uint64_t t;
            if (parse_time_ns(line, &t) == 0) {
                if (rtt_ns == 0) rtt_ns = t;  /* first tiny batch ≈ RTT */
                last_ns = t;
            }
        }

        /* Exponential progression */
        if (n >= 8) {
            cs = cs * 2;
            size_t cap = server_max < MAX_CHUNK ? server_max : MAX_CHUNK;
            if (cs > cap) cs = cap;
            n = 1;
        } else {
            n *= 2;
        }
    }

done:;
    /* TIME = transmission_time + RTT.  Subtract the RTT estimate (first tiny
     * batch, where transmission_time ≈ 0) to get actual throughput. */
    uint64_t transfer_ns = (last_ns > rtt_ns) ? last_ns - rtt_ns : 0;
    double bps;
    if (transfer_ns > 0)
        bps = last_bytes / (transfer_ns / 1e9);
    else if (last_ns > 0)
        bps = last_bytes / (last_ns / 1e9);
    else
        bps = last_bytes / ((now_ns() - t_start) / 1e9);
    double mbps = bps * 8.0 / 1e6;

    /* Target 50 chunks/sec → round to nearest KiB, clamp to [min, max]. */
    size_t ideal   = (size_t)(bps / 50.0);
    size_t rounded = ((ideal + 512) / 1024) * 1024;
    size_t cap     = server_max < MAX_CHUNK ? server_max : MAX_CHUNK;
    size_t floor_v = server_min > MIN_CHUNK ? server_min : MIN_CHUNK;
    size_t chunk_size = rounded < floor_v ? floor_v :
                        rounded > cap     ? cap      : rounded;

    int dl = threads_for(mbps, DL_THRESH, DL_COUNT, 3);
    int ul = threads_for(mbps, UL_THRESH, UL_COUNT, 4);
    if (dl > max_threads) dl = max_threads;
    if (ul > max_threads) ul = max_threads;

    printf("  pre-test: %.1f Mbit/s → chunk=%zu KiB  dl_threads=%d  ul_threads=%d\n",
           mbps, chunk_size / 1024, dl, ul);

    conn_quit(conn);
    conn_free(conn);

    out->chunk_size = chunk_size;
    out->dl_threads = dl;
    out->ul_threads = ul;
    return 0;
}
