use anyhow::{bail, Result};
use std::time::{Duration, Instant};

use crate::connection::RmbtConn;

const SAMPLE_INTERVAL: Duration = Duration::from_millis(40);

pub struct TransferResult {
    pub bytes:      u64,
    pub elapsed_ns: u64,
    pub thread_id:  usize,
    /// (cumulative_bytes, time_ns_from_phase_start) — one entry per 40 ms interval
    /// plus a final entry that uses the server-reported elapsed time.
    pub samples:    Vec<(u64, u64)>,
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

    let t0          = Instant::now();
    let mut total   = 0u64;
    let mut buf     = vec![0u8; chunk_size];
    let mut samples = Vec::new();
    let mut last_sample = t0;

    loop {
        conn.read_exact(&mut buf)?;
        total += chunk_size as u64;

        let now = Instant::now();
        if now.duration_since(last_sample) >= SAMPLE_INTERVAL {
            samples.push((total, now.duration_since(t0).as_nanos() as u64));
            last_sample = now;
        }

        if *buf.last().unwrap() == 0xFF {
            break;
        }
    }

    conn.write_line("OK")?;
    let time_line  = conn.read_line()?;
    let elapsed_ns = parse_time_ns(&time_line)?;

    // Final entry uses the server-reported elapsed time for accuracy.
    samples.push((total, elapsed_ns));

    println!(
        "  dl[{thread_id:2}]  {:.2} Mbit/s  ({total} bytes in {:.3}s, client {:.3}s)",
        total as f64 * 8.0 / (elapsed_ns as f64 / 1e9) / 1_000_000.0,
        elapsed_ns as f64 / 1e9,
        t0.elapsed().as_secs_f64(),
    );

    Ok(TransferResult { bytes: total, elapsed_ns, thread_id, samples })
}

// ─── Upload (PUTNORESULT with optional client-side intermediate output) ───────

/// `intermediate=true` prints a throughput line every ~40 ms while uploading.
/// Speed samples are always collected regardless of the `intermediate` flag.
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

    let deadline            = Instant::now() + Duration::from_secs(duration_secs as u64);
    let t0                  = Instant::now();
    let mut total           = 0u64;
    let mut samples         = Vec::new();
    let mut last_sample     = t0;
    let mut last_sample_bytes = 0u64;

    loop {
        let terminal = Instant::now() >= deadline;
        *chunk.last_mut().unwrap() = if terminal { 0xFF } else { 0x00 };
        conn.write_bytes(&chunk)?;
        total += chunk_size as u64;

        // Don't record a sample on the terminal iteration — the authoritative
        // final entry (with server-reported time) is always pushed after the loop.
        if !terminal {
            let now = Instant::now();
            if now.duration_since(last_sample) >= SAMPLE_INTERVAL {
                samples.push((total, now.duration_since(t0).as_nanos() as u64));

                if intermediate {
                    let dt = now.duration_since(last_sample).as_secs_f64();
                    let db = total - last_sample_bytes;
                    if dt > 0.0 {
                        println!(
                            "  ul[{thread_id:2}] +{:.2} Mbit/s",
                            db as f64 * 8.0 / dt / 1_000_000.0,
                        );
                    }
                }

                last_sample       = now;
                last_sample_bytes = total;
            }
        }

        if terminal { break; }
    }

    conn.flush()?;
    let time_line  = conn.read_line()?;
    let elapsed_ns = parse_time_ns(&time_line)?;

    // Final entry: total bytes with server-reported elapsed time.
    samples.push((total, elapsed_ns));

    println!(
        "  ul[{thread_id:2}]  {:.2} Mbit/s  ({total} bytes in {:.3}s, client {:.3}s)",
        total as f64 * 8.0 / (elapsed_ns as f64 / 1e9) / 1_000_000.0,
        elapsed_ns as f64 / 1e9,
        t0.elapsed().as_secs_f64(),
    );

    Ok(TransferResult { bytes: total, elapsed_ns, thread_id, samples })
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

pub(crate) fn parse_time_ns(line: &str) -> Result<u64> {
    line.split_whitespace()
        .nth(1)
        .and_then(|s| s.parse::<u64>().ok())
        .ok_or_else(|| anyhow::anyhow!("invalid TIME line: {line}"))
}
