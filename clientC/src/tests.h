#pragma once
#include <stdint.h>
#include "connection.h"

typedef struct {
    uint64_t bytes;
    uint64_t time_ns;
} SpeedSample;

typedef struct {
    uint64_t    bytes;
    uint64_t    elapsed_ns;
    int         thread_id;
    /* Intermediate speed samples collected every 40 ms during the phase.
     * The last entry always carries the server-reported elapsed time.
     * Allocated by run_download / run_upload; caller must free(). */
    SpeedSample *samples;
    int          num_samples;
} TransferResult;

typedef struct {
    uint64_t client_ns;
    uint64_t server_ns;
    uint64_t time_ns;
} PingResult;

/* Returns number of pings collected, or -1 on error. */
int run_ping(RmbtConn *conn, double duration_secs,
             uint32_t min_pings, uint32_t max_pings,
             PingResult *results, int max_results);

int run_download(RmbtConn *conn, uint32_t duration_secs,
                 size_t chunk_size, int thread_id, TransferResult *out);

int run_upload(RmbtConn *conn, uint32_t duration_secs,
               size_t chunk_size, int thread_id,
               int intermediate, TransferResult *out);

/* Parse "TIME <ns>" line. Returns -1 on error. */
int parse_time_ns(const char *line, uint64_t *out);
