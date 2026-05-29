#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <openssl/ssl.h>
#include <curl/curl.h>

#include "connection.h"
#include "control.h"
#include "pretest.h"
#include "tests.h"
#include "uuid_store.h"

#define MAX_THREADS 20
#define MAX_PINGS   100

/* ── Thread arguments / results ─────────────────────────────────────────────── */

typedef enum { PHASE_DOWNLOAD, PHASE_UPLOAD } Phase;

typedef struct {
    /* inputs */
    const char   *addr;
    uint16_t      port;
    int           use_tls;
    int           no_tls_verify;
    int           protocol;
    const char   *token;
    uint32_t      duration;
    size_t        chunk_size;
    int           thread_id;
    int           intermediate;
    Phase         phase;
    pthread_barrier_t *barrier;
    /* outputs */
    TransferResult result;
    int            failed;
} ThreadArg;

static void *thread_func(void *arg)
{
    ThreadArg *a = arg;

    RmbtConn *conn = conn_connect(a->addr, a->port, a->use_tls,
                                  a->no_tls_verify, a->protocol);
    if (!conn) {
        pthread_barrier_wait(a->barrier);
        a->failed = 1;
        return NULL;
    }
    if (conn_greeting(conn, a->token) < 0) {
        conn_free(conn);
        pthread_barrier_wait(a->barrier);
        a->failed = 1;
        return NULL;
    }

    pthread_barrier_wait(a->barrier);

    int rc;
    if (a->phase == PHASE_DOWNLOAD) {
        rc = run_download(conn, a->duration, a->chunk_size, a->thread_id, &a->result);
    } else {
        rc = run_upload(conn, a->duration, a->chunk_size, a->thread_id,
                        a->intermediate, &a->result);
    }
    conn_quit(conn);
    conn_free(conn);

    if (rc < 0) a->failed = 1;
    return NULL;
}

/* Run `n` threads for one phase.  Returns number of successful threads. */
static int run_phase(int n, const char *addr, uint16_t port,
                     int use_tls, int no_tls_verify, int protocol,
                     const char *token, uint32_t duration, size_t chunk_size,
                     int intermediate, Phase phase,
                     TransferResult *results, int *result_count)
{
    pthread_t       threads[MAX_THREADS];
    ThreadArg       args[MAX_THREADS];
    pthread_barrier_t bar;

    pthread_barrier_init(&bar, NULL, n);

    for (int i = 0; i < n; i++) {
        args[i].addr          = addr;
        args[i].port          = port;
        args[i].use_tls       = use_tls;
        args[i].no_tls_verify = no_tls_verify;
        args[i].protocol      = protocol;
        args[i].token         = token;
        args[i].duration      = duration;
        args[i].chunk_size    = chunk_size;
        args[i].thread_id     = i;
        args[i].intermediate  = intermediate;
        args[i].phase         = phase;
        args[i].barrier       = &bar;
        args[i].failed        = 0;
        memset(&args[i].result, 0, sizeof(args[i].result));
        pthread_create(&threads[i], NULL, thread_func, &args[i]);
    }

    int ok = 0;
    for (int i = 0; i < n; i++) {
        pthread_join(threads[i], NULL);
        if (!args[i].failed)
            results[ok++] = args[i].result;
        else
            fprintf(stderr, "[thread %d] dropped (skipping)\n", i);
    }

    pthread_barrier_destroy(&bar);
    *result_count = ok;
    return ok;
}

/* ── Helpers ─────────────────────────────────────────────────────────────────── */

static uint64_t now_ms_wall(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static void print_usage(const char *prog)
{
    printf(
        "Usage: %s -h URL [options]\n\n"
        "Options:\n"
        "  -h, --host URL          Control server base URL (required)\n"
        "  -p, --port PORT         Override test server port\n"
        "  -u, --uuid UUID         Client UUID (uses/creates ~/.rmbt_client_uuid if omitted)\n"
        "  -t, --threads N         Force thread count for download and upload\n"
        "  -d, --duration SECS     Test duration in seconds\n"
        "      --ws                Use WebSocket (RMBTws) framing\n"
        "      --http              Use plain HTTP upgrade (RMBThttp, default)\n"
        "      --no-tls-verify     Skip TLS certificate verification\n"
        "      --debug             Print control server JSON\n"
        "      --intermediate      Print upload throughput every 40 ms per thread\n"
        "      --help              Print this help\n",
        prog);
}

/* ── main ────────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
    curl_global_init(CURL_GLOBAL_DEFAULT);

    const char *host         = NULL;
    const char *uuid_cli     = NULL;
    int         port_ovr     = 0;
    int         threads_ovr  = 0;
    int         dur_ovr      = 0;
    int         force_ws     = 0;
    int         force_http   = 0;
    int         no_tls_verify = 0;
    int         debug        = 0;
    int         intermediate = 0;

    static struct option long_opts[] = {
        {"host",          required_argument, NULL, 'h'},
        {"port",          required_argument, NULL, 'p'},
        {"uuid",          required_argument, NULL, 'u'},
        {"threads",       required_argument, NULL, 't'},
        {"duration",      required_argument, NULL, 'd'},
        {"ws",            no_argument,       NULL, 'W'},
        {"http",          no_argument,       NULL, 'H'},
        {"no-tls-verify", no_argument,       NULL, 'n'},
        {"debug",         no_argument,       NULL, 'D'},
        {"intermediate",  no_argument,       NULL, 'i'},
        {"help",          no_argument,       NULL, '?'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "h:p:u:t:d:", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'h': host        = optarg;           break;
        case 'p': port_ovr    = atoi(optarg);     break;
        case 'u': uuid_cli    = optarg;           break;
        case 't': threads_ovr = atoi(optarg);     break;
        case 'd': dur_ovr     = atoi(optarg);     break;
        case 'W': force_ws    = 1;                break;
        case 'H': force_http  = 1;                break;
        case 'n': no_tls_verify = 1;              break;
        case 'D': debug       = 1;                break;
        case 'i': intermediate = 1;               break;
        case '?': print_usage(argv[0]); return 0;
        default:  print_usage(argv[0]); return 1;
        }
    }

    if (!host) {
        fprintf(stderr, "Error: --host is required\n\n");
        print_usage(argv[0]);
        return 1;
    }

    /* prepend https:// if no scheme given */
    static char host_buf[512];
    if (strncmp(host, "http://", 7) != 0 && strncmp(host, "https://", 8) != 0) {
        snprintf(host_buf, sizeof(host_buf), "https://%s", host);
        host = host_buf;
    }

    /*
     * Resolve UUID:
     *  - CLI --uuid: use as-is, skip settings call
     *  - Otherwise: call /settings with any stored UUID; server returns the
     *    authoritative UUID; persist it for future runs
     */
    char uuid_buf[64] = "";
    if (uuid_cli) {
        strncpy(uuid_buf, uuid_cli, sizeof(uuid_buf) - 1);
    } else {
        char stored[64] = "";
        int  has_stored = (uuid_load(stored, sizeof(stored)) == 0);

        if (control_request_settings(host,
                                     has_stored ? stored : NULL,
                                     debug,
                                     uuid_buf, sizeof(uuid_buf)) < 0)
            return 1;

        /* Persist if new or changed */
        if (!has_stored || strcmp(stored, uuid_buf) != 0)
            uuid_save(uuid_buf);
    }

    /* ── Step 1: control server ──────────────────────────────────────────────── */
    printf("Contacting control server: %s\n", host);
    TestParams params;
    if (control_request_test(host, uuid_buf, force_ws, debug, &params) < 0)
        return 1;

    /* Show token preview */
    char token_preview[44] = "";
    snprintf(token_preview, sizeof(token_preview), "%.40s", params.token);
    printf("Token:    %s...\n", token_preview);
    printf("Server:   %s:%u (%s)\n",
           params.server_addr, params.server_port,
           params.encryption ? "TLS" : "plain TCP");

    int protocol;
    if (force_ws)
        protocol = PROTO_WS;
    else if (force_http)
        protocol = PROTO_HTTP;
    else if (strcmp(params.server_type, "RMBTws") == 0)
        protocol = PROTO_WS;
    else
        protocol = PROTO_HTTP;

    printf("Protocol: %s  (server_type: %s)\n",
           protocol == PROTO_WS ? "RMBTws" : "RMBThttp",
           params.server_type[0] ? params.server_type : "unset");

    if (params.wait > 0) {
        printf("Waiting %us before test...\n", params.wait);
        sleep(params.wait);
    }

    uint16_t port     = port_ovr ? (uint16_t)port_ovr : params.server_port;
    uint32_t duration = dur_ovr  ? (uint32_t)dur_ovr  : params.duration;
    int      server_cap = params.num_threads < MAX_THREADS
                          ? (int)params.num_threads : MAX_THREADS;

    /* ── Step 2: pre-test ────────────────────────────────────────────────────── */
    PretestResult pt;
    if (run_pretest(params.server_addr, port, params.encryption, no_tls_verify,
                    protocol, params.token, server_cap, &pt) < 0)
        return 1;

    int dl_threads = threads_ovr ? threads_ovr : pt.dl_threads;
    int ul_threads = threads_ovr ? threads_ovr : pt.ul_threads;
    if (dl_threads < 1) dl_threads = 1;
    if (ul_threads < 1) ul_threads = 1;
    if (dl_threads > server_cap) dl_threads = server_cap;
    if (ul_threads > server_cap) ul_threads = server_cap;
    size_t dl_chunk_size = pt.chunk_size;
    /* Many servers use read() instead of readFully() for upload, so they see
     * partial reads bounded by the TLS record size (~16 KB).  With very large
     * chunks (4 MB = 256 reads/chunk) a false-positive terminal-byte detection
     * exceeds 60% probability per chunk, causing Broken Pipe.  Cap upload at
     * 512 KB where false positives are rare and servers handle them gracefully. */
#define MAX_UL_CHUNK (512 * 1024)
    size_t ul_chunk_size = dl_chunk_size < MAX_UL_CHUNK ? dl_chunk_size : MAX_UL_CHUNK;

    printf("\nTest plan: dl_threads=%d  ul_threads=%d  dl_chunk=%zu KiB  ul_chunk=%zu KiB  duration=%us\n",
           dl_threads, ul_threads, dl_chunk_size / 1024, ul_chunk_size / 1024, duration);

    uint64_t test_begin_ms = now_ms_wall();

    /* ── Step 3: ping ────────────────────────────────────────────────────────── */
    printf("\nPing (1 s, 10-100 pings):\n");
    PingResult ping_results[MAX_PINGS];
    int num_pings = 0;
    {
        RmbtConn *conn = conn_connect(params.server_addr, port,
                                      params.encryption, no_tls_verify, protocol);
        if (!conn) return 1;
        if (conn_greeting(conn, params.token) < 0) { conn_free(conn); return 1; }
        num_pings = run_ping(conn, 1.0, 10, MAX_PINGS, ping_results, MAX_PINGS);
        conn_quit(conn);
        conn_free(conn);
    }
    if (num_pings < 0) { fprintf(stderr, "Ping phase failed\n"); return 1; }

    /* ── Step 4: download ────────────────────────────────────────────────────── */
    printf("\nDownload (%d thread(s), %us):\n", dl_threads, duration);
    TransferResult dl_results[MAX_THREADS];
    int num_dl = 0;
    run_phase(dl_threads, params.server_addr, port, params.encryption,
              no_tls_verify, protocol, params.token,
              duration, dl_chunk_size, 0, PHASE_DOWNLOAD,
              dl_results, &num_dl);
    if (num_dl == 0) { fprintf(stderr, "All download threads failed\n"); return 1; }

    /* ── Step 5: upload ──────────────────────────────────────────────────────── */
    printf("\nUpload (%d thread(s), %us):\n", ul_threads, duration);
    TransferResult ul_results[MAX_THREADS];
    int num_ul = 0;
    run_phase(ul_threads, params.server_addr, port, params.encryption,
              no_tls_verify, protocol, params.token,
              duration, ul_chunk_size, intermediate, PHASE_UPLOAD,
              ul_results, &num_ul);
    if (num_ul == 0) { fprintf(stderr, "All upload threads failed\n"); return 1; }

    /* ── Step 6: aggregate ───────────────────────────────────────────────────── */
    uint64_t dl_bytes = 0, dl_ns = 0;
    for (int i = 0; i < num_dl; i++) {
        dl_bytes += dl_results[i].bytes;
        if (dl_results[i].elapsed_ns > dl_ns) dl_ns = dl_results[i].elapsed_ns;
    }
    uint64_t ul_bytes = 0, ul_ns = 0;
    for (int i = 0; i < num_ul; i++) {
        ul_bytes += ul_results[i].bytes;
        if (ul_results[i].elapsed_ns > ul_ns) ul_ns = ul_results[i].elapsed_ns;
    }
    if (!dl_ns) dl_ns = 1;
    if (!ul_ns) ul_ns = 1;

    double dl_mbps = (double)dl_bytes * 8.0 / (dl_ns / 1e9) / 1e6;
    double ul_mbps = (double)ul_bytes * 8.0 / (ul_ns / 1e9) / 1e6;

    /* Ping stats */
    uint64_t ping_min_client = UINT64_MAX, ping_shortest_server = UINT64_MAX;
    uint64_t client_ns_arr[MAX_PINGS];
    for (int i = 0; i < num_pings; i++) {
        client_ns_arr[i] = ping_results[i].client_ns;
        if (ping_results[i].client_ns < ping_min_client)
            ping_min_client = ping_results[i].client_ns;
        if (ping_results[i].server_ns < ping_shortest_server)
            ping_shortest_server = ping_results[i].server_ns;
    }
    if (ping_min_client    == UINT64_MAX) ping_min_client    = 0;
    if (ping_shortest_server == UINT64_MAX) ping_shortest_server = 0;

    qsort(client_ns_arr, num_pings, sizeof(uint64_t), cmp_u64);
    uint64_t ping_median_client = num_pings ? client_ns_arr[num_pings / 2] : 0;

    printf("\n=== Results ===\n");
    printf("Ping (min):     %7.2f ms  (%d pings)\n",
           ping_min_client / 1e6, num_pings);
    printf("Ping (median):  %7.2f ms\n", ping_median_client / 1e6);
    printf("Download:       %7.2f Mbit/s  (%llu bytes in %.2fs, %d thread(s))\n",
           dl_mbps, (unsigned long long)dl_bytes, dl_ns / 1e9, num_dl);
    printf("Upload:         %7.2f Mbit/s  (%llu bytes in %.2fs, %d thread(s))\n",
           ul_mbps, (unsigned long long)ul_bytes, ul_ns / 1e9, num_ul);

    /* ── Step 7: submit ──────────────────────────────────────────────────────── */
    uint64_t dl_kbps = (uint64_t)((double)dl_bytes * 8e6 / dl_ns);
    uint64_t ul_kbps = (uint64_t)((double)ul_bytes * 8e6 / ul_ns);

    PingItem ping_items[MAX_PINGS];
    for (int i = 0; i < num_pings; i++) {
        ping_items[i].value        = ping_results[i].client_ns;
        ping_items[i].value_server = ping_results[i].server_ns;
        ping_items[i].time_ns      = ping_results[i].time_ns;
    }

    /* Count total samples across all threads. */
    int num_sd = 0;
    for (int i = 0; i < num_dl; i++) num_sd += dl_results[i].num_samples;
    for (int i = 0; i < num_ul; i++) num_sd += ul_results[i].num_samples;

    SpeedItem *sd = calloc((size_t)num_sd, sizeof(SpeedItem));
    if (!sd) return 1;
    int sd_pos = 0;
    for (int i = 0; i < num_dl; i++) {
        for (int j = 0; j < dl_results[i].num_samples; j++) {
            strcpy(sd[sd_pos].direction, "download");
            sd[sd_pos].thread = dl_results[i].thread_id;
            sd[sd_pos].time   = dl_results[i].samples[j].time_ns;
            sd[sd_pos].bytes  = dl_results[i].samples[j].bytes;
            sd_pos++;
        }
    }
    for (int i = 0; i < num_ul; i++) {
        for (int j = 0; j < ul_results[i].num_samples; j++) {
            strcpy(sd[sd_pos].direction, "upload");
            sd[sd_pos].thread = ul_results[i].thread_id;
            sd[sd_pos].time   = ul_results[i].samples[j].time_ns;
            sd[sd_pos].bytes  = ul_results[i].samples[j].bytes;
            sd_pos++;
        }
    }

    TestResultSubmission result;
    memset(&result, 0, sizeof(result));
    strcpy(result.client_language,         "en");
    strcpy(result.client_name,
           protocol == PROTO_WS ? "RMBTws" : "RMBT");
    snprintf(result.client_uuid,            sizeof(result.client_uuid),            "%s", uuid_buf);
    snprintf(result.client_version,         sizeof(result.client_version),         "%s", GIT_VERSION);
    snprintf(result.client_software_version, sizeof(result.client_software_version), "%s", GIT_VERSION);
    strcpy(result.model,                   "Client CLI C");
    result.network_type               = 98;
    strcpy(result.platform,                "CLI");
    strcpy(result.product,                 "rmbt-client-c");
    result.pings                      = ping_items;
    result.num_pings                  = num_pings;
    result.test_bytes_download        = dl_bytes;
    result.test_bytes_upload          = ul_bytes;
    result.test_nsec_download         = dl_ns;
    result.test_nsec_upload           = ul_ns;
    result.test_num_threads           = num_dl;
    result.num_threads_ul             = num_ul;
    result.test_ping_shortest         = ping_shortest_server;
    result.test_speed_download        = dl_kbps;
    result.test_speed_upload          = ul_kbps;
    snprintf(result.test_token, sizeof(result.test_token), "%s", params.token);
    snprintf(result.test_uuid,  sizeof(result.test_uuid),  "%s", params.test_uuid);
    result.time_ms                    = test_begin_ms;
    strcpy(result.timezone,            "UTC");
    strcpy(result.client_type,         "DESKTOP");
    strcpy(result.version_code,        "1");
    result.speed_detail               = sd;
    result.num_speed_detail           = num_sd;
    result.user_server_selection      = 0;
    strcpy(result.test_status,         "0");
    result.test_port_remote           = port;

    if (params.open_test_uuid[0])
        printf("Result:         https://www.netztest.at/share/%s\n", params.open_test_uuid);

    printf("\nSubmitting results to control server...\n");
    control_submit_result(host, &result, debug);

    for (int i = 0; i < num_dl; i++) free(dl_results[i].samples);
    for (int i = 0; i < num_ul; i++) free(ul_results[i].samples);
    free(sd);
    curl_global_cleanup();
    return 0;
}
