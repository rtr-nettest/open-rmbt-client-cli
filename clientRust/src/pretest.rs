use anyhow::{bail, Result};
use std::time::{Duration, Instant};

use crate::connection::{RmbtConn, Protocol};

const PRETEST_DURATION: Duration = Duration::from_millis(2000);
const MIN_CHUNK: usize = 1024;
const MAX_CHUNK: usize = 4 * 1024 * 1024;

pub struct PretestResult {
    pub chunk_size: usize,
    pub dl_threads: usize,
    pub ul_threads: usize,
}

// Rows are (min_mbps, thread_count), sorted descending so the first match wins.
const DL_TABLE: &[(f64, usize)] = &[(100.0, 5), (1.0, 3), (0.0, 1)];
const UL_TABLE: &[(f64, usize)] = &[(150.0, 5), (80.0, 3), (30.0, 2), (0.0, 1)];

fn threads_for(mbps: f64, table: &[(f64, usize)]) -> usize {
    table.iter()
         .find(|&&(thresh, _)| mbps >= thresh)
         .map(|&(_, c)| c)
         .unwrap_or(1)
}

/// Run a 2-second single-thread download pre-test using GETCHUNKS with
/// exponential chunk-count and chunk-size progression.
/// Returns the optimal chunk size and suggested thread counts for each direction.
pub fn run_pretest(
    addr:          &str,
    port:          u16,
    use_tls:       bool,
    no_tls_verify: bool,
    protocol:      Protocol,
    token:         &str,
    max_threads:   usize,
) -> Result<PretestResult> {
    println!("\nPre-test: measuring baseline throughput…");

    let mut conn = RmbtConn::connect(addr, port, use_tls, no_tls_verify, protocol)?;
    conn.greeting(token)?;

    let server_min = conn.chunk_size_min;
    let server_max = conn.chunk_size_max;

    let t_start        = Instant::now();
    let mut cs         = server_min.max(MIN_CHUNK);
    let mut n          = 1usize;
    let mut last_bytes = 0u64;
    let mut last_ns    = 0u64;  // TIME from the last (largest) GETCHUNKS batch
    let mut rtt_ns     = 0u64;  // TIME of the first (tiny) batch ≈ round-trip time

    loop {
        if t_start.elapsed() >= PRETEST_DURATION { break; }

        let accept = conn.read_accept()?;
        if !accept.contains("GETCHUNKS") {
            bail!("pre-test: expected ACCEPT with GETCHUNKS, got: {accept}");
        }

        conn.write_line(&format!("GETCHUNKS {n} {cs}"))?;

        let mut buf = vec![0u8; cs];
        for _ in 0..n {
            conn.read_exact(&mut buf)?;
        }
        last_bytes = (n as u64) * (cs as u64);

        conn.write_line("OK")?;
        let time_line = conn.read_line()?;
        if let Ok(ns) = crate::tests::parse_time_ns(&time_line) {
            if rtt_ns == 0 { rtt_ns = ns; }  // first tiny batch ≈ RTT
            last_ns = ns;
        }

        // Exponential progression: double n until n≥8, then double chunk size.
        if n >= 8 {
            cs = (cs * 2).min(server_max.min(MAX_CHUNK));
            n = 1;
        } else {
            n *= 2;
        }
    }

    // TIME = transmission_time + RTT.  Subtract the RTT estimate (from the
    // first tiny batch where transmission time ≈ 0) to get actual throughput.
    let transfer_ns = last_ns.saturating_sub(rtt_ns);
    let bps = if transfer_ns > 0 {
        last_bytes as f64 / (transfer_ns as f64 / 1_000_000_000.0)
    } else if last_ns > 0 {
        last_bytes as f64 / (last_ns as f64 / 1_000_000_000.0)
    } else {
        last_bytes as f64 / t_start.elapsed().as_secs_f64()
    };
    let mbps = bps * 8.0 / 1_000_000.0;

    // Target 50 chunks/sec (1 chunk per 20 ms), rounded to the nearest KiB.
    let ideal     = (bps / 50.0) as usize;
    let rounded   = ((ideal + 512) / 1024) * 1024;
    let chunk_size = rounded
        .max(server_min.max(MIN_CHUNK))
        .min(server_max.min(MAX_CHUNK));

    let dl = threads_for(mbps, DL_TABLE).min(max_threads);
    let ul = threads_for(mbps, UL_TABLE).min(max_threads);

    println!(
        "  pre-test: {mbps:.1} Mbit/s → chunk={} KiB  dl_threads={dl}  ul_threads={ul}",
        chunk_size / 1024
    );

    conn.quit()?;
    Ok(PretestResult { chunk_size, dl_threads: dl, ul_threads: ul })
}
