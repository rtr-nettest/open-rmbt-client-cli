//! Machine-readable JSON progress interface for the `open-rmbt-desktop` app.
//!
//! See `doc/json_interface.md` for the full specification. All JSON output is
//! gated by the `-v` flag; when disabled these functions are no-ops (except the
//! progress counter, which is a cheap atomic used by the interim monitor).
//!
//! Throughput is reported in **decimal** Mbit/s (`bytes * 8 / seconds / 1e6`),
//! ping RTTs in **ms**, and `pingTimeNs` in **ns**, per the spec.

use serde_json::json;
use std::sync::{Arc, Mutex};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

static ENABLED:  AtomicBool = AtomicBool::new(false);
/// Cumulative bytes transferred in the current transfer phase (all threads).
static PROGRESS: AtomicU64  = AtomicU64::new(0);
/// Last state passed to `state_change`, reported as `phase` on `ERROR`.
static PHASE:    Mutex<String> = Mutex::new(String::new());

#[derive(Clone, Copy)]
pub enum Direction { Down, Up }

pub fn set_enabled(v: bool) { ENABLED.store(v, Ordering::Relaxed); }
pub fn enabled() -> bool    { ENABLED.load(Ordering::Relaxed) }

fn now_ms() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_millis() as u64
}

fn emit(v: serde_json::Value) {
    if enabled() { println!("{v}"); }
}

// ─── Plain-text lifecycle sentinels (backward-compat shim, gui mode only) ───────

pub fn starting_test() { if enabled() { println!("STARTING TEST."); } }
pub fn ending_test()   { if enabled() { println!("ENDING TEST."); } }

// ─── Messages ───────────────────────────────────────────────────────────────

pub fn state_change(state: &str) {
    if let Ok(mut p) = PHASE.lock() { *p = state.to_string(); }
    emit(json!({ "type": "STATE_CHANGE", "time": now_ms(), "state": state }));
}

/// `STATE_CHANGE` to `ERROR`, carrying the phase in which the run failed and a
/// human-readable reason. The caller then prints `ENDING TEST.` and exits non-zero.
pub fn error(msg: &str) {
    let phase = PHASE.lock().map(|p| p.clone()).unwrap_or_default();
    emit(json!({
        "type":  "STATE_CHANGE",
        "time":  now_ms(),
        "state": "ERROR",
        "phase": phase,
        "error": msg,
    }));
}

pub fn uuid_info(test_uuid: Option<&str>, open_test_uuid: Option<&str>, token: &str, loop_uuid: Option<&str>) {
    emit(json!({
        "type":         "UUID_INFO",
        "testUuid":     test_uuid,
        "openTestUuid": open_test_uuid,
        "testToken":    token,
        "loopUuid":     loop_uuid,
    }));
}

pub fn ping_result(client_ns: u64, server_ns: u64, time_ns: u64) {
    emit(json!({
        "type":       "PING_RESULT",
        "time":       now_ms(),
        "phase":      "PING",
        "pingTimeNs": time_ns,
        "pingClient": client_ns as f64 / 1e6, // ns → ms
        "pingServer": server_ns as f64 / 1e6, // ns → ms
        "status":     "PING",
    }));
}

/// Locally measured final result, emitted once before result submission.
/// Units as in the interim messages: decimal Mbit/s and ms.
pub struct FinalResult<'a> {
    pub test_uuid:      Option<&'a str>,
    pub open_test_uuid: Option<&'a str>,
    pub loop_uuid:      Option<&'a str>,
    pub down_mbps:      f64,
    pub up_mbps:        f64,
    /// Median of the server-measured ping RTTs, in ns.
    pub ping_median_ns: f64,
    pub ping_count:     usize,
    pub down_bytes:     u64,
    pub down_ns:        u64,
    pub down_threads:   usize,
    pub up_bytes:       u64,
    pub up_ns:          u64,
    pub up_threads:     usize,
}

pub fn final_result(r: &FinalResult) {
    emit(json!({
        "type":         "FINAL_RESULT",
        "time":         now_ms(),
        "testUuid":     r.test_uuid,
        "openTestUuid": r.open_test_uuid,
        "loopUuid":     r.loop_uuid,
        "down":         r.down_mbps,
        "up":           r.up_mbps,
        "pingMedian":   r.ping_median_ns / 1e6, // ns → ms
        "pingCount":    r.ping_count,
        "downBytes":    r.down_bytes,
        "downNs":       r.down_ns,
        "downThreads":  r.down_threads,
        "upBytes":      r.up_bytes,
        "upNs":         r.up_ns,
        "upThreads":    r.up_threads,
    }));
}

pub fn submit_result(success: bool, http_status: Option<u16>, error: Option<&str>) {
    emit(json!({
        "type":       "SUBMIT_RESULT",
        "time":       now_ms(),
        "success":    success,
        "httpStatus": http_status,
        "error":      error,
    }));
}

fn transfer_result(dir: Direction, bytes: u64, mbps: f64, open_test_uuid: Option<&str>, progress_frac: f64) {
    let (typ, phase) = match dir {
        Direction::Down => ("DOWNLOAD_RESULT", "DOWN"),
        Direction::Up   => ("UPLOAD_RESULT",   "UP"),
    };
    let (down, up) = match dir {
        Direction::Down => (mbps, -1.0),
        Direction::Up   => (-1.0, mbps),
    };
    emit(json!({
        "type":         typ,
        "time":         now_ms(),
        "phase":        phase,
        "down":         down,
        "up":           up,
        "bytes":        bytes,
        "testOpenUuid": open_test_uuid,
        "progress":     progress_frac,
        "status":       phase,
    }));
}

// ─── Interim throughput monitor ───────────────────────────────────────────────

pub fn add_progress(bytes: u64) { PROGRESS.fetch_add(bytes, Ordering::Relaxed); }

/// Handle for a running interim-throughput monitor thread.
pub struct Monitor {
    stop:   Arc<AtomicBool>,
    handle: Option<JoinHandle<()>>,
}

impl Monitor {
    /// Stop the monitor and wait for the thread to finish.
    pub fn stop(mut self) {
        self.stop.store(true, Ordering::Relaxed);
        if let Some(h) = self.handle.take() { let _ = h.join(); }
    }
}

/// Reset the progress counter and, in gui mode, spawn a thread that emits a
/// `DOWNLOAD_RESULT`/`UPLOAD_RESULT` every 250 ms until stopped. A no-op monitor
/// is returned when gui mode is disabled.
pub fn spawn_transfer_monitor(dir: Direction, duration_secs: u32, open_test_uuid: Option<String>) -> Monitor {
    PROGRESS.store(0, Ordering::Relaxed);
    let stop = Arc::new(AtomicBool::new(false));

    let handle = if enabled() {
        let stop2 = stop.clone();
        Some(thread::spawn(move || {
            let start = Instant::now();
            while !stop2.load(Ordering::Relaxed) {
                thread::sleep(Duration::from_millis(250));
                let secs = start.elapsed().as_secs_f64();
                if secs <= 0.0 { continue; }
                let bytes = PROGRESS.load(Ordering::Relaxed);
                let mbps  = bytes as f64 * 8.0 / secs / 1_000_000.0; // decimal Mbit/s
                let frac  = (secs / duration_secs.max(1) as f64).min(1.0);
                transfer_result(dir, bytes, mbps, open_test_uuid.as_deref(), frac);
            }
        }))
    } else {
        None
    };

    Monitor { stop, handle }
}
