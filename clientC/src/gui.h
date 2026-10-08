#pragma once
#include <stdint.h>

/*
 * Machine-readable JSON progress interface for the open-rmbt-desktop app.
 * See doc/json_interface.md. All JSON output is gated by the -v flag.
 *
 * Throughput is reported in decimal Mbit/s (bytes*8/s/1e6), ping RTTs in ms,
 * and pingTimeNs in ns.
 */

void gui_set_enabled(int v);
int  gui_enabled(void);

void gui_starting_test(void);
void gui_ending_test(void);
void gui_state_change(const char *state);
void gui_uuid_info(const char *test_uuid, const char *open_test_uuid, const char *token, const char *loop_uuid);
void gui_ping_result(uint64_t client_ns, uint64_t server_ns, uint64_t time_ns);

/* STATE_CHANGE to ERROR with the phase in which the run failed and a
 * human-readable reason. The caller then prints ENDING TEST. and exits non-zero. */
void gui_error(const char *msg);

/* Locally measured final result, emitted once before result submission.
 * Units as in the interim messages: decimal Mbit/s and ms, rounded to 3
 * decimals on output (ping_median_ns is
 * the median of the server-measured RTTs, converted to ms on output). */
typedef struct {
    const char *test_uuid;
    const char *open_test_uuid;
    const char *loop_uuid;
    double      down_mbps;
    double      up_mbps;
    double      ping_median_ns;
    int         ping_count;
    uint64_t    down_bytes;
    uint64_t    down_ns;
    int         down_threads;
    uint64_t    up_bytes;
    uint64_t    up_ns;
    int         up_threads;
} GuiFinalResult;
void gui_final_result(const GuiFinalResult *r);

/* Outcome of the /result submission. http_status 0 = no HTTP response;
 * error NULL/empty on success. */
void gui_submit_result(int success, long http_status, const char *error);

/* Report an error: printed to stderr and kept as the "last error" of the
 * current phase (cleared on every state change), which main() appends to the
 * ERROR message. Thread-safe. */
void        gui_report_error(const char *fmt, ...);
const char *gui_last_error(void);

/* Cumulative bytes for the interim monitor (called from transfer threads). */
void gui_add_progress(uint64_t bytes);

/* Interim throughput monitor. upload=0 → DOWNLOAD_RESULT, upload=1 → UPLOAD_RESULT.
 * Resets the progress counter and (in gui mode) starts a thread emitting every
 * ~250 ms until gui_monitor_stop(). */
typedef struct GuiMonitor GuiMonitor;
GuiMonitor *gui_monitor_start(int upload, uint32_t duration_secs, const char *open_test_uuid);
void        gui_monitor_stop(GuiMonitor *m);
