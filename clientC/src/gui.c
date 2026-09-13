#include "gui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>
#include <pthread.h>

static int              g_enabled  = 0;
static _Atomic uint64_t g_progress = 0;   /* cumulative bytes, current phase */

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}

void gui_set_enabled(int v) { g_enabled = v; }
int  gui_enabled(void)      { return g_enabled; }

/* ── Plain-text lifecycle sentinels (backward-compat shim, gui mode only) ────── */

void gui_starting_test(void) { if (g_enabled) { printf("STARTING TEST.\n"); fflush(stdout); } }
void gui_ending_test(void)   { if (g_enabled) { printf("ENDING TEST.\n");   fflush(stdout); } }

/* ── Messages ────────────────────────────────────────────────────────────────── */

void gui_state_change(const char *state)
{
    if (!g_enabled) return;
    printf("{\"type\":\"STATE_CHANGE\",\"time\":%llu,\"state\":\"%s\"}\n",
           (unsigned long long)now_ms(), state);
    fflush(stdout);
}

void gui_uuid_info(const char *test_uuid, const char *open_test_uuid, const char *token)
{
    if (!g_enabled) return;
    printf("{\"type\":\"UUID_INFO\",\"testUuid\":\"%s\",\"openTestUuid\":\"%s\",\"testToken\":\"%s\"}\n",
           test_uuid ? test_uuid : "",
           open_test_uuid ? open_test_uuid : "",
           token ? token : "");
    fflush(stdout);
}

void gui_ping_result(uint64_t client_ns, uint64_t server_ns, uint64_t time_ns)
{
    if (!g_enabled) return;
    printf("{\"type\":\"PING_RESULT\",\"time\":%llu,\"phase\":\"PING\","
           "\"pingTimeNs\":%llu,\"pingClient\":%.6f,\"pingServer\":%.6f,\"status\":\"PING\"}\n",
           (unsigned long long)now_ms(), (unsigned long long)time_ns,
           (double)client_ns / 1e6, (double)server_ns / 1e6);
    fflush(stdout);
}

/* ── Interim throughput monitor ──────────────────────────────────────────────── */

void gui_add_progress(uint64_t bytes) { atomic_fetch_add(&g_progress, bytes); }

struct GuiMonitor {
    int          upload;
    uint32_t     duration_secs;
    char         open_test_uuid[160];
    _Atomic int  stop;
    pthread_t    thread;
    int          started;
};

static void emit_transfer(int upload, uint64_t bytes, double mbps,
                          const char *otu, double frac)
{
    const char *typ   = upload ? "UPLOAD_RESULT" : "DOWNLOAD_RESULT";
    const char *phase = upload ? "UP" : "DOWN";
    double down = upload ? -1.0 : mbps;
    double up   = upload ?  mbps : -1.0;
    printf("{\"type\":\"%s\",\"time\":%llu,\"phase\":\"%s\","
           "\"down\":%.6f,\"up\":%.6f,\"bytes\":%llu,\"testOpenUuid\":\"%s\","
           "\"progress\":%.4f,\"status\":\"%s\"}\n",
           typ, (unsigned long long)now_ms(), phase, down, up,
           (unsigned long long)bytes, otu ? otu : "", frac, phase);
    fflush(stdout);
}

static void *monitor_fn(void *arg)
{
    GuiMonitor *m = arg;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    const struct timespec interval = { 0, 250 * 1000 * 1000 }; /* 250 ms */

    while (!atomic_load(&m->stop)) {
        nanosleep(&interval, NULL);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double secs = (double)(now.tv_sec - start.tv_sec)
                    + (double)(now.tv_nsec - start.tv_nsec) / 1e9;
        if (secs <= 0.0) continue;
        uint64_t bytes = atomic_load(&g_progress);
        double mbps = (double)bytes * 8.0 / secs / 1e6;     /* decimal Mbit/s */
        double frac = secs / (m->duration_secs ? m->duration_secs : 1);
        if (frac > 1.0) frac = 1.0;
        emit_transfer(m->upload, bytes, mbps,
                      m->open_test_uuid[0] ? m->open_test_uuid : NULL, frac);
    }
    return NULL;
}

GuiMonitor *gui_monitor_start(int upload, uint32_t duration_secs, const char *open_test_uuid)
{
    atomic_store(&g_progress, 0);
    GuiMonitor *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->upload        = upload;
    m->duration_secs = duration_secs;
    if (open_test_uuid)
        snprintf(m->open_test_uuid, sizeof(m->open_test_uuid), "%s", open_test_uuid);
    atomic_store(&m->stop, 0);
    if (g_enabled && pthread_create(&m->thread, NULL, monitor_fn, m) == 0)
        m->started = 1;
    return m;
}

void gui_monitor_stop(GuiMonitor *m)
{
    if (!m) return;
    atomic_store(&m->stop, 1);
    if (m->started) pthread_join(m->thread, NULL);
    free(m);
}
