#include "tests.h"
#include "connection.h"
#include "gui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SAMPLE_INTERVAL_NS 40000000ULL   /* 40 ms */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

int parse_time_ns(const char *line, uint64_t *out)
{
    /* "TIME <ns>" */
    const char *p = line;
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    if (!*p) return -1;
    char *end;
    unsigned long long v = strtoull(p, &end, 10);
    if (end == p) return -1;
    *out = (uint64_t)v;
    return 0;
}

/* ── Ping ────────────────────────────────────────────────────────────────────── */

int run_ping(RmbtConn *conn, double duration_secs,
             uint32_t min_pings, uint32_t max_pings,
             PingResult *results, int max_results)
{
    uint64_t phase_start = now_ns();
    uint64_t deadline    = phase_start + (uint64_t)(duration_secs * 1e9);
    int      count       = 0;
    char     line[256];

    while (count < max_results) {
        if ((uint32_t)count >= max_pings) break;
        if (now_ns() >= deadline && (uint32_t)count >= min_pings) break;

        if (conn_read_line(conn, line, sizeof(line)) < 0) return -1;
        if (!strstr(line, "PING")) {
            fprintf(stderr, "Expected ACCEPT with PING, got: %s\n", line);
            return -1;
        }

        uint64_t t0      = now_ns();
        uint64_t time_ns = t0 - phase_start;
        if (conn_write_line(conn, "PING") < 0) return -1;

        if (conn_read_line(conn, line, sizeof(line)) < 0) return -1;
        uint64_t client_ns = now_ns() - t0;

        if (strcmp(line, "PONG") != 0) {
            fprintf(stderr, "Expected PONG, got: %s\n", line);
            return -1;
        }
        if (conn_write_line(conn, "OK") < 0) return -1;

        if (conn_read_line(conn, line, sizeof(line)) < 0) return -1;
        uint64_t server_ns;
        if (parse_time_ns(line, &server_ns) < 0) return -1;

        printf("  ping  client=%.3fms  server=%.3fms\n",
               client_ns / 1e6, server_ns / 1e6);
        gui_ping_result(client_ns, server_ns, time_ns);

        results[count].client_ns = client_ns;
        results[count].server_ns = server_ns;
        results[count].time_ns   = time_ns;
        count++;
    }
    return count;
}

/* ── Download ────────────────────────────────────────────────────────────────── */

int run_download(RmbtConn *conn, uint32_t duration_secs,
                 size_t chunk_size, int thread_id, TransferResult *out)
{
    char line[256];
    if (conn_read_line(conn, line, sizeof(line)) < 0) return -1;
    if (!strstr(line, "GETTIME")) {
        fprintf(stderr, "Expected ACCEPT with GETTIME, got: %s\n", line);
        return -1;
    }

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "GETTIME %u %zu", duration_secs, chunk_size);
    if (conn_write_line(conn, cmd) < 0) return -1;

    /* Read in 16 KiB blocks so the 40 ms sample interval fires regardless of
     * how large chunk_size is (can reach 1-2 MB on fast connections). */
#define DL_READ_BLOCK (16u * 1024u)
    size_t rblk = chunk_size < DL_READ_BLOCK ? chunk_size : DL_READ_BLOCK;
    unsigned char *buf = malloc(rblk);
    if (!buf) return -1;

    /* Pre-allocate: 25 samples/s  +  anchor(1)  +  final(1)  +  headroom(4) */
    int max_samples = (int)(duration_secs * 25) + 6;
    SpeedSample *samples = malloc((size_t)max_samples * sizeof(SpeedSample));
    if (!samples) { free(buf); return -1; }
    int num_samples = 0;

    uint64_t t0          = now_ns();
    uint64_t total       = 0;
    uint64_t last_sample = t0;

    /* Anchor at origin so speed graphs start at t = 0. */
    samples[num_samples].bytes   = 0;
    samples[num_samples].time_ns = 0;
    num_samples++;

    size_t in_chunk = chunk_size;

    for (;;) {
        size_t want = in_chunk < rblk ? in_chunk : rblk;
        if (conn_read_exact(conn, buf, want) < 0) {
            free(buf); free(samples);
            return -1;
        }
        total    += want;
        in_chunk -= want;
        gui_add_progress(want);

        uint64_t now = now_ns();
        if (now - last_sample >= SAMPLE_INTERVAL_NS) {
            if (num_samples < max_samples - 1) {   /* reserve 1 slot for final entry */
                samples[num_samples].bytes   = total;
                samples[num_samples].time_ns = now - t0;
                num_samples++;
            }
            last_sample = now;
        }

        if (in_chunk == 0) {
            if (buf[want - 1] == 0xFF) { free(buf); break; }
            in_chunk = chunk_size;
        }
    }

    if (conn_write_line(conn, "OK") < 0) { free(samples); return -1; }

    if (conn_read_line(conn, line, sizeof(line)) < 0) { free(samples); return -1; }
    uint64_t elapsed_ns;
    if (parse_time_ns(line, &elapsed_ns) < 0) { free(samples); return -1; }

    /* Final entry uses the server-reported elapsed time. */
    samples[num_samples].bytes   = total;
    samples[num_samples].time_ns = elapsed_ns;
    num_samples++;

    printf("  dl[%2d]  %.2f Mbit/s  (%llu bytes in %.3fs, client %.3fs)\n",
           thread_id,
           (double)total * 8.0 / (elapsed_ns / 1e9) / 1e6,
           (unsigned long long)total,
           elapsed_ns / 1e9,
           (now_ns() - t0) / 1e9);

    out->bytes       = total;
    out->elapsed_ns  = elapsed_ns;
    out->thread_id   = thread_id;
    out->samples     = samples;
    out->num_samples = num_samples;
    return 0;
}

/* ── Upload ──────────────────────────────────────────────────────────────────── */

int run_upload(RmbtConn *conn, uint32_t duration_secs,
               size_t chunk_size, int thread_id,
               int intermediate, TransferResult *out)
{
    char line[256];
    if (conn_read_line(conn, line, sizeof(line)) < 0) return -1;
    if (!strstr(line, "PUT")) {
        fprintf(stderr, "Expected ACCEPT with PUT/PUTNORESULT, got: %s\n", line);
        return -1;
    }

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "PUTNORESULT %zu", chunk_size);
    if (conn_write_line(conn, cmd) < 0) return -1;

    if (conn_read_line(conn, line, sizeof(line)) < 0) return -1;
    if (strcmp(line, "OK") != 0) {
        fprintf(stderr, "Expected OK after PUTNORESULT, got: %s\n", line);
        return -1;
    }

    unsigned char *chunk = malloc(chunk_size);
    if (!chunk) return -1;

    /* Fill chunk with pseudo-random data. */
    for (size_t i = 0; i < chunk_size; i++)
        chunk[i] = (unsigned char)(i & 0xFF);

    /* Pre-allocate: 25 samples/s  +  anchor(1)  +  final(1)  +  headroom(4) */
    int max_samples = (int)(duration_secs * 25) + 6;
    SpeedSample *samples = malloc((size_t)max_samples * sizeof(SpeedSample));
    if (!samples) { free(chunk); return -1; }
    int num_samples = 0;

    uint64_t deadline         = now_ns() + (uint64_t)duration_secs * 1000000000ULL;
    uint64_t t0               = now_ns();
    uint64_t total            = 0;
    uint64_t last_sample      = t0;
    uint64_t last_sample_bytes = 0;

    /* Anchor at origin so speed graphs start at t = 0. */
    samples[num_samples].bytes   = 0;
    samples[num_samples].time_ns = 0;
    num_samples++;

    /* Write in 16 KiB blocks so the 40 ms sample interval fires even on
     * asymmetric connections where chunk_size can reach 1-2 MB. */
#define UL_WRITE_BLOCK (16u * 1024u)
    size_t wblk = chunk_size < UL_WRITE_BLOCK ? chunk_size : UL_WRITE_BLOCK;

    for (;;) {
        int terminal = now_ns() >= deadline;
        chunk[chunk_size - 1] = terminal ? 0xFF : 0x00;

        size_t sent = 0;
        while (sent < chunk_size) {
            size_t want = (chunk_size - sent) < wblk ? (chunk_size - sent) : wblk;
            if (conn_write_bytes(conn, chunk + sent, want) < 0) {
                free(chunk); free(samples);
                return -1;
            }
            sent  += want;
            total += want;
            gui_add_progress(want);

            if (!terminal) {
                uint64_t now = now_ns();
                if (now - last_sample >= SAMPLE_INTERVAL_NS) {
                    if (num_samples < max_samples - 1) {
                        samples[num_samples].bytes   = total;
                        samples[num_samples].time_ns = now - t0;
                        num_samples++;
                    }
                    if (intermediate) {
                        double   dt = (now - last_sample) / 1e9;
                        uint64_t db = total - last_sample_bytes;
                        if (dt > 0.0)
                            printf("  ul[%2d] +%.2f Mbit/s\n",
                                   thread_id, (double)db * 8.0 / dt / 1e6);
                    }
                    last_sample       = now;
                    last_sample_bytes = total;
                }
            }
        }

        if (terminal) break;
    }
    free(chunk);

    conn_flush(conn);

    if (conn_read_line(conn, line, sizeof(line)) < 0) { free(samples); return -1; }
    uint64_t elapsed_ns;
    if (parse_time_ns(line, &elapsed_ns) < 0) { free(samples); return -1; }

    /* Replace the last sample's timestamp with the server-reported elapsed time. */
    if (num_samples > 0) {
        samples[num_samples - 1].time_ns = elapsed_ns;
    } else {
        samples[num_samples].bytes   = total;
        samples[num_samples].time_ns = elapsed_ns;
        num_samples++;
    }

    printf("  ul[%2d]  %.2f Mbit/s  (%llu bytes in %.3fs, client %.3fs)\n",
           thread_id,
           (double)total * 8.0 / (elapsed_ns / 1e9) / 1e6,
           (unsigned long long)total,
           elapsed_ns / 1e9,
           (now_ns() - t0) / 1e9);

    out->bytes       = total;
    out->elapsed_ns  = elapsed_ns;
    out->thread_id   = thread_id;
    out->samples     = samples;
    out->num_samples = num_samples;
    return 0;
}
