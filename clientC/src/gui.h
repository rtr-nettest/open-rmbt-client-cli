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
void gui_uuid_info(const char *test_uuid, const char *open_test_uuid, const char *token);
void gui_ping_result(uint64_t client_ns, uint64_t server_ns, uint64_t time_ns);

/* Cumulative bytes for the interim monitor (called from transfer threads). */
void gui_add_progress(uint64_t bytes);

/* Interim throughput monitor. upload=0 → DOWNLOAD_RESULT, upload=1 → UPLOAD_RESULT.
 * Resets the progress counter and (in gui mode) starts a thread emitting every
 * ~250 ms until gui_monitor_stop(). */
typedef struct GuiMonitor GuiMonitor;
GuiMonitor *gui_monitor_start(int upload, uint32_t duration_secs, const char *open_test_uuid);
void        gui_monitor_stop(GuiMonitor *m);
