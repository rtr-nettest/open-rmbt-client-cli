use anyhow::{bail, Result};
use std::time::{Duration, Instant};

use crate::connection::RmbtConn;

#[allow(dead_code)]
pub struct TransferResult {
    pub bytes:      u64,
    pub elapsed_ns: u64,
    pub thread_id:  usize,
}

pub struct PingResult {
    pub client_ns: u64,
    pub server_ns: u64,
    pub time_ns:   u64, // offset from the first ping's send time
}

// ─── Ping ─────────────────────────────────────────────────────────────────────

/// Ping as fast as possible for `duration_secs` seconds (client-side deadline),
/// stopping early at `max_pings` and continuing past the deadline until
/// `min_pings` have completed.  Returns per-ping client RTT, server RTT, and
/// time offset from the start of the ping phase.
pub fn run_ping(
    conn:          &mut RmbtConn,
    duration_secs: f64,
    min_pings:     u32,
    max_pings:     u32,
) -> Result<Vec<PingResult>> {
    let phase_start = Instant::now();
    let deadline    = phase_start + Duration::from_secs_f64(duration_secs);
    let mut results = Vec::new();

    loop {
        let count = results.len() as u32;
        if count >= max_pings { break; }
        if Instant::now() >= deadline && count >= min_pings { break; }

        let accept = conn.read_accept()?;
        if !accept.contains("PING") {
            bail!("expected ACCEPT with PING, got: {accept}");
        }

        let t0      = Instant::now();
        let time_ns = t0.duration_since(phase_start).as_nanos() as u64;
        conn.write_line("PING")?;

        let pong      = conn.read_line()?;
        let client_ns = t0.elapsed().as_nanos() as u64;

        if pong != "PONG" {
            bail!("expected PONG, got: {pong}");
        }

        conn.write_line("OK")?;
        let time_line = conn.read_line()?;
        let server_ns = parse_time_ns(&time_line)?;

        println!(
            "  ping  client={:.3}ms  server={:.3}ms",
            client_ns as f64 / 1_000_000.0,
            server_ns as f64 / 1_000_000.0,
        );
        results.push(PingResult { client_ns, server_ns, time_ns });
    }

    Ok(results)
}

// ─── Download (GETTIME) ───────────────────────────────────────────────────────

pub fn run_download(
    conn:          &mut RmbtConn,
    duration_secs: u32,
    chunk_size:    usize,
    thread_id:     usize,
) -> Result<TransferResult> {
    let accept = conn.read_accept()?;
    if !accept.contains("GETTIME") {
        bail!("expected ACCEPT with GETTIME, got: {accept}");
    }

    conn.write_line(&format!("GETTIME {duration_secs} {chunk_size}"))?;

    let t0        = Instant::now();
    let mut total = 0u64;
    let mut buf   = vec![0u8; chunk_size];

    loop {
        conn.read_exact(&mut buf)?;
        total += chunk_size as u64;
        if *buf.last().unwrap() == 0xFF {
            break;
        }
    }

    conn.write_line("OK")?;
    let time_line  = conn.read_line()?;
    let elapsed_ns = parse_time_ns(&time_line)?;

    println!(
        "  dl[{thread_id:2}]  {:.2} Mbit/s  ({total} bytes in {:.3}s, client {:.3}s)",
        total as f64 * 8.0 / (elapsed_ns as f64 / 1e9) / 1_000_000.0,
        elapsed_ns as f64 / 1e9,
        t0.elapsed().as_secs_f64(),
    );

    Ok(TransferResult { bytes: total, elapsed_ns, thread_id })
}

// ─── Upload (PUTNORESULT with optional client-side intermediate output) ───────

/// `intermediate=true` prints a throughput line every ~40 ms while uploading.
pub fn run_upload(
    conn:          &mut RmbtConn,
    duration_secs: u32,
    chunk_size:    usize,
    thread_id:     usize,
    intermediate:  bool,
) -> Result<TransferResult> {
    let accept = conn.read_accept()?;
    if !accept.contains("PUT") {
        bail!("expected ACCEPT with PUT/PUTNORESULT, got: {accept}");
    }

    conn.write_line(&format!("PUTNORESULT {chunk_size}"))?;

    let ok = conn.read_line()?;
    if ok != "OK" {
        bail!("expected OK after PUTNORESULT, got: {ok}");
    }

    let mut chunk = vec![0u8; chunk_size];
    fastrand::fill(&mut chunk);

    let deadline   = Instant::now() + Duration::from_secs(duration_secs as u64);
    let t0         = Instant::now();
    let mut total  = 0u64;

    let report_interval       = Duration::from_millis(40);
    let mut last_report       = Instant::now();
    let mut last_report_bytes = 0u64;

    loop {
        let terminal = Instant::now() >= deadline;
        *chunk.last_mut().unwrap() = if terminal { 0xFF } else { 0x00 };
        conn.write_bytes(&chunk)?;
        total += chunk_size as u64;

        if intermediate {
            let now = Instant::now();
            if now.duration_since(last_report) >= report_interval || terminal {
                let dt = now.duration_since(last_report).as_secs_f64();
                let db = total - last_report_bytes;
                if dt > 0.0 {
                    println!(
                        "  ul[{thread_id:2}] +{:.2} Mbit/s",
                        db as f64 * 8.0 / dt / 1_000_000.0,
                    );
                }
                last_report       = now;
                last_report_bytes = total;
            }
        }

        if terminal { break; }
    }

    conn.flush()?;
    let time_line  = conn.read_line()?;
    let elapsed_ns = parse_time_ns(&time_line)?;

    println!(
        "  ul[{thread_id:2}]  {:.2} Mbit/s  ({total} bytes in {:.3}s, client {:.3}s)",
        total as f64 * 8.0 / (elapsed_ns as f64 / 1e9) / 1_000_000.0,
        elapsed_ns as f64 / 1e9,
        t0.elapsed().as_secs_f64(),
    );

    Ok(TransferResult { bytes: total, elapsed_ns, thread_id })
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

pub(crate) fn parse_time_ns(line: &str) -> Result<u64> {
    line.split_whitespace()
        .nth(1)
        .and_then(|s| s.parse::<u64>().ok())
        .ok_or_else(|| anyhow::anyhow!("invalid TIME line: {line}"))
}
