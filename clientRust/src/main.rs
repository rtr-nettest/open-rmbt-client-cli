mod control;
mod connection;
mod gui;
mod pretest;
mod tests;
mod uuid_store;

use anyhow::Result;
use clap::{Arg, ArgAction, Command};
use connection::Protocol;
use std::sync::{Arc, Barrier};
use std::thread;

const MAX_THREADS: usize = 20;
const MODEL:       &str  = "Client CLI Rust";
/// Full `git describe` with commit hash (e.g. `v2.1-3-gc833667`), reported as `client_version`.
const VERSION_FULL:     &str = match option_env!("GIT_VERSION_FULL") { Some(v) => v, None => "dev" };
/// Full describe suffixed with branch (e.g. `v2.1-3-gc833667-master`), reported
/// as `software_revision`, `software_version` and `software_version_name`.
const VERSION_REVISION: &str = match option_env!("GIT_REVISION") { Some(v) => v, None => "dev" };

fn main() -> Result<()> {
    let _ = rustls::crypto::ring::default_provider().install_default();

    // The desktop app passes the historic single-dash long flag `-set-version`.
    // Normalize it to clap's `--set-version` before parsing (see doc/json_interface.md §4).
    let argv: Vec<String> = std::env::args()
        .map(|a| if a == "-set-version" { "--set-version".to_string() } else { a })
        .collect();

    let matches = Command::new("rmbt-client")
        .about("RMBT network measurement client")
        .disable_help_flag(true)
        .disable_version_flag(true)
        .arg(Arg::new("help").long("help").action(ArgAction::Help).help("Print this help"))
        .arg(
            Arg::new("host")
                .short('h').long("host").value_name("URL").required(true)
                .help("Control server base URL (e.g. https://measure.example.com)"),
        )
        .arg(
            Arg::new("port")
                .short('p').long("port").value_name("PORT")
                .help("Override test server port")
                .value_parser(clap::value_parser!(u16)),
        )
        .arg(
            Arg::new("uuid")
                .short('u').long("uuid").value_name("UUID")
                .help("Client UUID (leave empty on first run)"),
        )
        .arg(
            Arg::new("threads")
                .short('t').long("threads").value_name("N")
                .help("Force thread count for both download and upload (overrides pre-test)")
                .value_parser(clap::value_parser!(usize)),
        )
        .arg(
            Arg::new("duration")
                .short('d').long("duration").value_name("SECS")
                .help("Test duration in seconds (default: from control server)")
                .value_parser(clap::value_parser!(u32)),
        )
        .arg(
            Arg::new("ws")
                .long("ws").action(ArgAction::SetTrue).conflicts_with("http")
                .help("Use WebSocket (RMBTws) framing instead of plain HTTP upgrade"),
        )
        .arg(
            Arg::new("http")
                .long("http").action(ArgAction::SetTrue).conflicts_with("ws")
                .help("Use plain HTTP upgrade (RMBThttp) — overrides auto-detection"),
        )
        .arg(
            Arg::new("no-tls-verify")
                .long("no-tls-verify").action(ArgAction::SetTrue)
                .help("Skip TLS certificate verification (insecure)"),
        )
        .arg(
            Arg::new("debug")
                .long("debug").action(ArgAction::SetTrue)
                .help("Print raw control server request and response JSON"),
        )
        .arg(
            Arg::new("intermediate")
                .long("intermediate").action(ArgAction::SetTrue)
                .help("Print intermediate upload throughput every 40 ms per thread"),
        )
        // ── open-rmbt-desktop JSON interface (doc/json_interface.md) ──────────
        .arg(
            Arg::new("verbose")
                .short('v').long("verbose").action(ArgAction::SetTrue)
                .help("Emit machine-readable JSON progress messages on stdout (GUI integration)"),
        )
        .arg(Arg::new("nettype").long("nettype").value_name("CODE")
            .help("Network type code reported to the control server (default 98 = LAN)"))
        .arg(Arg::new("type").long("type").value_name("TYPE")
            .help("Client type reported to the control server (default CLI)"))
        .arg(Arg::new("platform").long("platform").value_name("PLATFORM")
            .help("Platform label reported to the control server"))
        .arg(Arg::new("os").long("os").value_name("OS")
            .help("Operating-system string (accepted for compatibility)"))
        .arg(Arg::new("osver").long("osver").value_name("VER")
            .help("Operating-system version (accepted for compatibility)"))
        .arg(Arg::new("model").long("model").value_name("MODEL")
            .help("Device model reported to the control server"))
        .arg(Arg::new("set-version").long("set-version").value_name("VER")
            .help("Override the reported client software version (client_version)"))
        // Loop-mode flags: accepted for compatibility (single-run only for now).
        .arg(Arg::new("user-loop-mode").long("user-loop-mode").action(ArgAction::SetTrue)
            .help("Accepted for compatibility; loop mode is not yet implemented"))
        .arg(Arg::new("user-loop-mode-max-delay").long("user-loop-mode-max-delay").value_name("MIN"))
        .arg(Arg::new("user-loop-mode-test-counter").long("user-loop-mode-test-counter").value_name("N"))
        .arg(Arg::new("user-loop-mode-uuid").long("user-loop-mode-uuid").value_name("UUID"))
        .get_matches_from(argv);

    let raw_host      = matches.get_one::<String>("host").unwrap();
    let host_owned    = if raw_host.starts_with("http://") || raw_host.starts_with("https://") {
        raw_host.clone()
    } else {
        format!("https://{raw_host}")
    };
    let host          = host_owned.as_str();
    let port_ovr      = matches.get_one::<u16>("port").copied();
    let threads_ovr   = matches.get_one::<usize>("threads").copied();
    let dur_ovr       = matches.get_one::<u32>("duration").copied();
    let force_ws      = matches.get_flag("ws");
    let force_http    = matches.get_flag("http");
    let no_tls_verify = matches.get_flag("no-tls-verify");
    let debug         = matches.get_flag("debug");
    let intermediate  = matches.get_flag("intermediate");

    // JSON progress interface (open-rmbt-desktop). See doc/json_interface.md.
    let verbose       = matches.get_flag("verbose");
    gui::set_enabled(verbose);

    // Metadata reported to the control server (from the desktop app or defaults).
    let net_type      = matches.get_one::<String>("nettype")
        .and_then(|s| s.parse::<u32>().ok()).unwrap_or(98);
    let client_type   = matches.get_one::<String>("type").cloned().unwrap_or_else(|| "CLI".to_string());
    let platform      = matches.get_one::<String>("platform").cloned().unwrap_or_else(|| "CLI".to_string());
    let model         = matches.get_one::<String>("model").cloned().unwrap_or_else(|| MODEL.to_string());
    let client_version = matches.get_one::<String>("set-version").cloned()
        .unwrap_or_else(|| VERSION_FULL.to_string());

    if matches.get_flag("user-loop-mode") {
        eprintln!("Warning: --user-loop-mode is accepted but not yet implemented; running a single test.");
    }

    gui::starting_test();
    gui::state_change("INIT");

    // Resolve client UUID:
    //   1. CLI --uuid flag takes priority (user-supplied, skip settings call)
    //   2. Otherwise call /settings to register or re-identify; server returns UUID
    //   3. Persist the server-assigned UUID for subsequent runs
    let uuid_owned: String = match matches.get_one::<String>("uuid") {
        Some(s) => s.clone(),
        None => {
            let stored = uuid_store::load();
            let uuid = control::request_settings(host, stored.as_deref(), VERSION_REVISION, VERSION_REVISION, debug)?;
            if stored.as_deref() != Some(uuid.as_str()) {
                uuid_store::save(&uuid);
            }
            uuid
        }
    };
    let uuid = uuid_owned.as_str();

    // ── Step 1: request test parameters from the control server ──────────────
    println!("Contacting control server: {host}");
    let params = control::request_test(host, Some(uuid), VERSION_REVISION, VERSION_REVISION, force_ws, debug)?;

    gui::uuid_info(params.test_uuid.as_deref(), params.open_test_uuid.as_deref(), &params.token);

    let preview_token = &params.token[..params.token.len().min(40)];
    println!("Token:    {preview_token}…");
    println!(
        "Server:   {}:{} ({})",
        params.server_addr,
        params.server_port,
        if params.encryption { "TLS" } else { "plain TCP" }
    );

    let protocol = if force_ws { Protocol::Ws }
        else if force_http { Protocol::Http }
        else if params.server_type == "RMBTws" { Protocol::Ws }
        else { Protocol::Http };

    println!(
        "Protocol: {}  (server_type: {})",
        match protocol { Protocol::Ws => "RMBTws", Protocol::Http => "RMBThttp" },
        if params.server_type.is_empty() { "unset" } else { &params.server_type },
    );

    if params.wait > 0 {
        println!("Waiting {}s before test…", params.wait);
        std::thread::sleep(std::time::Duration::from_secs(params.wait as u64));
    }

    let port     = port_ovr.unwrap_or(params.server_port);
    let duration = dur_ovr.unwrap_or(params.duration);
    let addr     = Arc::new(params.server_addr.clone());
    let token    = Arc::new(params.token.clone());

    // ── Step 2: pre-test ──────────────────────────────────────────────────────
    gui::state_change("INIT_DOWN");
    let pt = pretest::run_pretest(
        &params.server_addr, port, params.encryption, no_tls_verify, protocol,
        &params.token,
        params.num_threads.min(MAX_THREADS as u32) as usize,
    )?;

    let server_cap    = params.num_threads.min(MAX_THREADS as u32) as usize;
    let dl_threads    = threads_ovr.unwrap_or(pt.dl_threads).max(1).min(server_cap);
    let ul_threads    = threads_ovr.unwrap_or(pt.ul_threads).max(1).min(server_cap);
    let dl_chunk_size = pt.chunk_size;
    // Many servers use read() (not readFully()) for upload, so they get
    // partial reads bounded by the TLS record size (~16 KB). With very large
    // chunks (4 MB = 256 reads/chunk) the probability of a false-positive
    // terminal-byte detection per chunk exceeds 60%, causing Broken Pipe.
    // Cap the upload chunk at 512 KB where partial-read false positives are
    // rare in practice (~5% per chunk) and the server handles them gracefully.
    const MAX_UL_CHUNK: usize = 512 * 1024;
    let ul_chunk_size = dl_chunk_size.min(MAX_UL_CHUNK);

    println!(
        "\nTest plan: dl_threads={dl_threads}  ul_threads={ul_threads}  \
         dl_chunk={} KiB  ul_chunk={} KiB  duration={}s",
        dl_chunk_size / 1024, ul_chunk_size / 1024, duration
    );

    // Record test begin time for the submission payload.
    let test_begin_ms = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64;

    // ── Step 3: ping ──────────────────────────────────────────────────────────
    gui::state_change("PING");
    println!("\nPing (1 s, 10–100 pings):");
    let (ping_results, server_version) = {
        let mut conn = connection::RmbtConn::connect(
            &addr, port, params.encryption, no_tls_verify, protocol,
        )?;
        conn.greeting(&token)?;
        let server_version = conn.server_version.clone();
        let r = tests::run_ping(&mut conn, 1.0, 10, 100)?;
        conn.quit()?;
        (r, server_version)
    };

    // ── Step 4: download ─────────────────────────────────────────────────────
    gui::state_change("DOWN");
    println!("\nDownload ({dl_threads} thread(s), {duration}s):");
    let dl_mon = gui::spawn_transfer_monitor(gui::Direction::Down, duration, params.open_test_uuid.clone());
    let dl_results = run_phase(
        dl_threads, &addr, port, params.encryption, no_tls_verify, protocol, &token,
        move |conn, tid| tests::run_download(conn, duration, dl_chunk_size, tid),
    )?;
    dl_mon.stop();

    // ── Step 5: upload ───────────────────────────────────────────────────────
    gui::state_change("INIT_UP");
    gui::state_change("UP");
    println!("\nUpload ({ul_threads} thread(s), {duration}s):");
    let ul_mon = gui::spawn_transfer_monitor(gui::Direction::Up, duration, params.open_test_uuid.clone());
    let ul_results = run_phase(
        ul_threads, &addr, port, params.encryption, no_tls_verify, protocol, &token,
        move |conn, tid| tests::run_upload(conn, duration, ul_chunk_size, tid, intermediate),
    )?;
    ul_mon.stop();

    // ── Step 6: aggregate results ─────────────────────────────────────────────
    let dl_bytes: u64 = dl_results.iter().map(|r| r.bytes).sum();
    let dl_ns          = dl_results.iter().map(|r| r.elapsed_ns).max().unwrap_or(1);
    let ul_bytes: u64 = ul_results.iter().map(|r| r.bytes).sum();
    let ul_ns          = ul_results.iter().map(|r| r.elapsed_ns).max().unwrap_or(1);

    let dl_mbps = dl_bytes as f64 * 8.0 / (dl_ns as f64 / 1e9) / 1_000_000.0;
    let ul_mbps = ul_bytes as f64 * 8.0 / (ul_ns as f64 / 1e9) / 1_000_000.0;

    let ping_min_ms    = ping_results.iter().map(|p| p.client_ns).min().unwrap_or(0) as f64 / 1e6;
    let ping_median_ms = {
        let mut v: Vec<u64> = ping_results.iter().map(|p| p.client_ns).collect();
        v.sort_unstable();
        *v.get(v.len() / 2).unwrap_or(&0)
    } as f64 / 1e6;

    println!("\n=== Results ===");
    println!("Ping (min):     {:7.2} ms  ({} pings)", ping_min_ms, ping_results.len());
    println!("Ping (median):  {:7.2} ms", ping_median_ms);
    println!(
        "Download:       {:7.2} Mbit/s  ({} bytes in {:.2}s, {} thread(s))",
        dl_mbps, dl_bytes, dl_ns as f64 / 1e9, dl_results.len()
    );
    println!(
        "Upload:         {:7.2} Mbit/s  ({} bytes in {:.2}s, {} thread(s))",
        ul_mbps, ul_bytes, ul_ns as f64 / 1e9, ul_results.len()
    );

    // ── Step 7: submit results to control server ──────────────────────────────
    let dl_kbps = (dl_bytes as f64 * 8e6 / dl_ns as f64) as u64;
    let ul_kbps = (ul_bytes as f64 * 8e6 / ul_ns as f64) as u64;
    let ping_shortest_ns = ping_results.iter().map(|p| p.server_ns).min().unwrap_or(0);

    let pings: Vec<control::PingItem> = ping_results.iter().map(|p| control::PingItem {
        value:        p.client_ns,
        value_server: p.server_ns,
        time_ns:      p.time_ns,
    }).collect();

    let speed_detail: Vec<control::SpeedItem> =
        dl_results.iter()
            .flat_map(|r| r.samples.iter().map(move |&(bytes, time)| control::SpeedItem {
                direction: "download".into(),
                thread:    r.thread_id,
                time,
                bytes,
            }))
        .chain(
            ul_results.iter()
                .flat_map(|r| r.samples.iter().map(move |&(bytes, time)| control::SpeedItem {
                    direction: "upload".into(),
                    thread:    r.thread_id,
                    time,
                    bytes,
                }))
        )
        .collect();

    let client_name = match protocol {
        Protocol::Ws   => "RMBTws",
        Protocol::Http => "RMBT",
    }.to_string();

    let result = control::TestResultSubmission {
        client_language:         "en".into(),
        client_name,
        client_uuid:             Some(uuid.to_string()),
        client_version:          client_version.clone(),
        client_software_version: server_version,
        geo_locations:           vec![],
        model:                   model.clone(),
        network_type:            net_type,
        platform:                platform.clone(),
        product:                 "rmbt-client".into(),
        pings,
        test_bytes_download:     dl_bytes,
        test_bytes_upload:       ul_bytes,
        test_nsec_download:      dl_ns,
        test_nsec_upload:        ul_ns,
        test_num_threads:        dl_results.len(),
        num_threads_ul:          ul_results.len(),
        test_ping_shortest:      ping_shortest_ns,
        test_speed_download:     dl_kbps,
        test_speed_upload:       ul_kbps,
        test_token:              params.token.clone(),
        test_uuid:               params.test_uuid.clone(),
        time:                    test_begin_ms,
        timezone:                "UTC".into(),
        client_type:             client_type.clone(),
        version_code:            "1".into(),
        speed_detail,
        user_server_selection:   false,
        test_status:             "0".into(),
        test_port_remote:        Some(port),
    };

    if let Some(ref otu) = params.open_test_uuid {
        println!("Result:         https://www.netztest.at/share/{otu}");
    }

    gui::state_change("SUBMITTING_RESULTS");
    println!("\nSubmitting results to control server…");
    control::submit_result(host, &result, debug)?;

    gui::state_change("END");
    gui::ending_test();

    Ok(())
}

/// Spawn `n` threads.  Each connects independently, waits at a barrier so all
/// start the test body simultaneously, then calls `f(conn, thread_id)`.
/// If a thread fails to connect or errors mid-test, it is excluded from results
/// and a warning is printed.  Returns an error only if all threads fail.
fn run_phase<F>(
    n:             usize,
    addr:          &Arc<String>,
    port:          u16,
    use_tls:       bool,
    no_tls_verify: bool,
    protocol:      Protocol,
    token:         &Arc<String>,
    f:             F,
) -> Result<Vec<tests::TransferResult>>
where
    F: Fn(&mut connection::RmbtConn, usize) -> Result<tests::TransferResult>
       + Send + Sync + 'static,
{
    let f   = Arc::new(f);
    let bar = Arc::new(Barrier::new(n));
    let mut handles = Vec::with_capacity(n);

    for i in 0..n {
        let addr2  = addr.clone();
        let tok2   = token.clone();
        let bar2   = bar.clone();
        let f2     = f.clone();

        handles.push(thread::spawn(move || -> Result<tests::TransferResult> {
            let mut conn = match connection::RmbtConn::connect(
                &addr2, port, use_tls, no_tls_verify, protocol,
            ) {
                Ok(c)  => c,
                Err(e) => { bar2.wait(); return Err(e); }
            };
            if let Err(e) = conn.greeting(&tok2) {
                bar2.wait();
                return Err(e);
            }
            bar2.wait();

            let result = f2(&mut conn, i);
            let _ = conn.quit();
            result
        }));
    }

    let mut results = Vec::with_capacity(n);
    for (i, h) in handles.into_iter().enumerate() {
        match h.join().expect("thread panicked") {
            Ok(r)  => results.push(r),
            Err(e) => eprintln!("[thread {i}] dropped (skipping): {e}"),
        }
    }

    if results.is_empty() {
        anyhow::bail!("all {n} threads failed");
    }
    Ok(results)
}
