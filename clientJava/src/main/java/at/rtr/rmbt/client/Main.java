package at.rtr.rmbt.client;

import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicReference;

public final class Main {

    private static final int    MAX_THREADS      = 20;
    /** Full describe with commit hash (e.g. "v2.1-4-ged32b3d"), reported as client_version. */
    private static final String VERSION_FULL     = loadVersionProp("version.full");
    /** Full describe plus branch (e.g. "v2.1-4-ged32b3d-dev"), reported as softwareVersion/Name/Revision. */
    private static final String VERSION_REVISION = loadVersionProp("version.revision");

    private static String loadVersionProp(String key) {
        try (var is = Main.class.getResourceAsStream("/version.properties")) {
            if (is == null) return "dev";
            var props = new java.util.Properties();
            props.load(is);
            return props.getProperty(key, "dev").trim();
        } catch (Exception e) {
            return "dev";
        }
    }

    public static void main(String[] args) {
        try {
            run(args);
        } catch (Exception e) {
            // In gui mode, report the failure as STATE_CHANGE "ERROR"
            // (doc/json_interface.md §3.2), then exit non-zero.
            String msg = describe(e);
            Gui.error(msg);
            Gui.endingTest();
            System.err.println("Error: " + msg);
            System.exit(1);
        }
    }

    /** Single-line reason from the cause chain (some exceptions carry no message). */
    private static String describe(Throwable e) {
        StringBuilder sb = new StringBuilder();
        for (Throwable t = e; t != null; t = t.getCause()) {
            String m = t.getMessage();
            String part = (m != null && !m.isBlank()) ? m : t.getClass().getSimpleName();
            if (sb.indexOf(part) >= 0) continue;
            if (sb.length() > 0) sb.append(": ");
            sb.append(part);
        }
        return sb.toString();
    }

    private static void run(String[] args) throws Exception {
        // ── CLI parsing ───────────────────────────────────────────────────────
        String  host         = null;
        String  uuidCli      = null;
        int     portOvr      = 0;
        int     threadsOvr   = 0;
        int     durOvr       = 0;
        boolean forceWs      = false;
        boolean forceHttp    = false;
        boolean noTlsVerify  = false;
        boolean debug        = false;
        boolean intermediate = false;
        // JSON progress interface (open-rmbt-desktop). See doc/json_interface.md.
        boolean verbose      = false;
        int     netType      = 98;
        String  clientType   = "DESKTOP";
        String  platform     = "CLI";
        String  model        = "Client CLI Java";
        String  device       = null;
        String  serverUuid   = null;
        // Loop mode: this client runs a single measurement per invocation; the
        // caller re-spawns it per iteration. See doc/json_interface.md.
        boolean loopMode     = false;
        int     loopMaxDelay = 0;
        int     loopCounter  = 0;
        String  loopUuidArg  = null;

        for (int i = 0; i < args.length; i++) {
            String a = args[i];
            if      ("-h".equals(a) || "--host".equals(a))         host         = args[++i];
            else if ("-p".equals(a) || "--port".equals(a))         portOvr      = Integer.parseInt(args[++i]);
            else if ("-u".equals(a) || "--uuid".equals(a))         uuidCli      = args[++i];
            else if ("-t".equals(a) || "--threads".equals(a))      threadsOvr   = Integer.parseInt(args[++i]);
            else if ("-d".equals(a) || "--duration".equals(a))     durOvr       = Integer.parseInt(args[++i]);
            else if ("--ws".equals(a))                             forceWs      = true;
            else if ("--http".equals(a))                           forceHttp    = true;
            else if ("--no-tls-verify".equals(a))                  noTlsVerify  = true;
            else if ("--debug".equals(a))                          debug        = true;
            else if ("--intermediate".equals(a))                   intermediate = true;
            else if ("-v".equals(a) || "--verbose".equals(a))      verbose      = true;
            else if ("--nettype".equals(a))                        netType      = Integer.parseInt(args[++i]);
            else if ("--type".equals(a))                           clientType   = args[++i];
            else if ("--platform".equals(a))                       platform     = args[++i];
            else if ("--model".equals(a))                          model        = args[++i];
            else if ("--server_uuid".equals(a))                    serverUuid   = args[++i];
            else if ("--os".equals(a) || "--osver".equals(a))      i++; // accepted for compatibility
            // Wrapping app version (desktop-app compat, also single-dash);
            // reported as the `device` field prefixed with "App: ".
            else if ("-set-version".equals(a) || "--set-version".equals(a)) device = "App: " + args[++i];
            else if ("--user-loop-mode".equals(a))                 loopMode     = true;
            else if ("--user-loop-mode-max-delay".equals(a))       loopMaxDelay = Integer.parseInt(args[++i]);
            else if ("--user-loop-mode-test-counter".equals(a))    loopCounter  = Integer.parseInt(args[++i]);
            else if ("--user-loop-mode-uuid".equals(a))            loopUuidArg  = args[++i];
            else if ("--help".equals(a))                           { printUsage(); return; }
            else if (a.startsWith("-")) {
                System.err.println("Unknown option: " + a);
                printUsage();
                System.exit(1);
            }
        }
        if (host == null) {
            System.err.println("Error: --host is required\n");
            printUsage();
            System.exit(1);
        }
        if (!host.startsWith("http://") && !host.startsWith("https://"))
            host = "https://" + host;

        Gui.setEnabled(verbose);
        Gui.startingTest();
        Gui.stateChange("INIT");

        ControlClient control = new ControlClient(host, debug, VERSION_REVISION);

        // ── UUID: /settings flow ──────────────────────────────────────────────
        String uuid;
        if (uuidCli != null) {
            uuid = uuidCli;
        } else {
            String stored = UuidStore.load();
            uuid = control.requestSettings(stored);
            if (!uuid.equals(stored)) UuidStore.save(uuid);
        }

        // ── Step 1: test request ──────────────────────────────────────────────
        int protocol = forceWs  ? RmbtConn.PROTO_WS
                     : forceHttp ? RmbtConn.PROTO_HTTP
                     : RmbtConn.PROTO_HTTP; // resolved again after params

        LoopModeInfo loop = null;
        if (loopMode) {
            String lu = (loopUuidArg == null) ? null : loopUuidArg.trim();
            if (lu != null && (lu.isEmpty() || "null".equals(lu))) lu = null;
            // Accept the loop UUID with or without the server's "L" prefix: strip a
            // single leading 'L' so the value sent on the wire is the bare UUID.
            if (lu != null && lu.startsWith("L")) lu = lu.substring(1);
            loop = new LoopModeInfo(loopMaxDelay, 0, 0, loopCounter, lu);
        }

        System.out.println("Contacting control server: " + host);
        TestParams params = control.requestTest(uuid, forceWs, serverUuid, loop);

        Gui.uuidInfo(params.testUuid(), params.openTestUuid(), params.token(), params.loopUuid());
        if (params.loopUuid() != null) {
            System.out.println("Loop UUID: " + params.loopUuid());
        }

        System.out.println("Token:    " + params.token().substring(0, Math.min(40, params.token().length())) + "...");
        System.out.printf("Server:   %s:%d (%s)%n",
                params.serverAddr(), params.serverPort(),
                params.encryption() ? "TLS" : "plain TCP");

        protocol = forceWs                              ? RmbtConn.PROTO_WS
                 : forceHttp                            ? RmbtConn.PROTO_HTTP
                 : "RMBTws".equals(params.serverType()) ? RmbtConn.PROTO_WS
                 :                                        RmbtConn.PROTO_HTTP;

        System.out.printf("Protocol: %s  (server_type: %s)%n",
                protocol == RmbtConn.PROTO_WS ? "RMBTws" : "RMBThttp",
                params.serverType().isEmpty() ? "unset" : params.serverType());

        if (params.waitSecs() > 0) {
            System.out.printf("Waiting %ds before test...%n", params.waitSecs());
            Thread.sleep(params.waitSecs() * 1000L);
        }

        int    port       = portOvr > 0 ? portOvr : params.serverPort();
        int    duration   = durOvr  > 0 ? durOvr  : params.duration();
        int    serverCap  = Math.min(params.numThreads(), MAX_THREADS);

        // ── Step 2: pre-test ──────────────────────────────────────────────────
        Gui.stateChange("INIT_DOWN");
        PretestResult pt = Pretest.run(
                params.serverAddr(), port, params.encryption(), noTlsVerify,
                protocol, params.token(), serverCap);

        int dlThreads   = Math.max(1, Math.min(serverCap, threadsOvr > 0 ? threadsOvr : pt.dlThreads()));
        int ulThreads   = Math.max(1, Math.min(serverCap, threadsOvr > 0 ? threadsOvr : pt.ulThreads()));
        int dlChunkSize = pt.chunkSize();
        // Many servers use read() (not readFully()) for upload, so partial reads
        // bounded by TLS record size (~16 KB) cause false-positive terminal-byte
        // detection. With 4 MB chunks (256 reads/chunk) the false-positive rate
        // per chunk exceeds 60%, causing Broken Pipe. Cap upload at 512 KB.
        final int MAX_UL_CHUNK = 512 * 1024;
        int ulChunkSize = Math.min(dlChunkSize, MAX_UL_CHUNK);

        System.out.printf("%nTest plan: dl_threads=%d  ul_threads=%d  dl_chunk=%d KiB  ul_chunk=%d KiB  duration=%ds%n",
                dlThreads, ulThreads, dlChunkSize / 1024, ulChunkSize / 1024, duration);

        long testBeginMs = System.currentTimeMillis();

        // ── Step 3: ping ──────────────────────────────────────────────────────
        Gui.stateChange("PING");
        System.out.println("\nPing (1 s, 10-100 pings):");
        List<PingResult> pings;
        String serverVersion;
        try (RmbtConn conn = RmbtConn.connect(
                params.serverAddr(), port, params.encryption(), noTlsVerify, protocol)) {
            conn.greeting(params.token());
            serverVersion = conn.serverVersion;
            pings = Tests.runPing(conn, 1.0, 10, 100);
            conn.quit();
        }

        // ── Step 4: download ──────────────────────────────────────────────────
        Gui.stateChange("DOWN");
        System.out.printf("%nDownload (%d thread(s), %ds):%n", dlThreads, duration);
        Gui.Monitor dlMon = Gui.startTransferMonitor(false, duration, params.openTestUuid());
        List<TransferResult> dlResults = runPhase(
                dlThreads, params.serverAddr(), port, params.encryption(),
                noTlsVerify, protocol, params.token(),
                duration, dlChunkSize, false, false);
        dlMon.stop();
        if (dlResults.isEmpty()) throw new java.io.IOException("all " + dlThreads + " download threads failed");

        // ── Step 5: upload ────────────────────────────────────────────────────
        Gui.stateChange("INIT_UP");
        Gui.stateChange("UP");
        System.out.printf("%nUpload (%d thread(s), %ds):%n", ulThreads, duration);
        Gui.Monitor ulMon = Gui.startTransferMonitor(true, duration, params.openTestUuid());
        List<TransferResult> ulResults = runPhase(
                ulThreads, params.serverAddr(), port, params.encryption(),
                noTlsVerify, protocol, params.token(),
                duration, ulChunkSize, true, intermediate);
        ulMon.stop();
        if (ulResults.isEmpty()) throw new java.io.IOException("all " + ulThreads + " upload threads failed");

        // ── Step 6: aggregate ─────────────────────────────────────────────────
        long dlBytes = 0, dlNs = 0, ulBytes = 0, ulNs = 0;
        for (TransferResult r : dlResults) { dlBytes += r.bytes(); dlNs = Math.max(dlNs, r.elapsedNs()); }
        for (TransferResult r : ulResults) { ulBytes += r.bytes(); ulNs = Math.max(ulNs, r.elapsedNs()); }
        if (dlNs == 0) dlNs = 1;
        if (ulNs == 0) ulNs = 1;

        double dlMbps = dlBytes * 8.0 / (dlNs / 1e9) / 1e6;
        double ulMbps = ulBytes * 8.0 / (ulNs / 1e9) / 1e6;

        double pingMedianNs = median(pings.stream().mapToLong(PingResult::serverNs).toArray());

        System.out.println("\n=== Results ===");
        System.out.printf("Ping:           %7.3f ms  (%d pings)%n", pingMedianNs / 1e6, pings.size());
        System.out.printf("Download:       %7.3f Mbit/s  (%d bytes in %.2fs, %d thread(s))%n",
                dlMbps, dlBytes, dlNs / 1e9, dlResults.size());
        System.out.printf("Upload:         %7.3f Mbit/s  (%d bytes in %.2fs, %d thread(s))%n",
                ulMbps, ulBytes, ulNs / 1e9, ulResults.size());

        Gui.finalResult(params.testUuid(), params.openTestUuid(), params.loopUuid(),
                dlMbps, ulMbps, pingMedianNs, pings.size(),
                dlBytes, dlNs, dlResults.size(), ulBytes, ulNs, ulResults.size());

        // ── Step 7: submit ────────────────────────────────────────────────────
        Gui.stateChange("SUBMITTING_RESULTS");
        String clientName = protocol == RmbtConn.PROTO_WS ? "RMBTws" : "RMBT";
        var resultNode = ControlClient.buildResult(
                uuid, clientName, VERSION_FULL, serverVersion, device, params, port,
                pings.toArray(PingResult[]::new),
                dlResults.toArray(TransferResult[]::new),
                ulResults.toArray(TransferResult[]::new));
        resultNode.put("time", testBeginMs);
        // Metadata reported to the control server (from CLI args or defaults).
        resultNode.put("network_type", netType);
        resultNode.put("type", clientType);
        resultNode.put("platform", platform);
        resultNode.put("model", model);
        resultNode.put("user_server_selection", serverUuid != null && !serverUuid.isEmpty());

        if (params.openTestUuid() != null)
            System.out.println("Result:         https://www.netztest.at/share/" + params.openTestUuid());

        System.out.println("\nSubmitting results to control server...");
        ControlClient.SubmitStatus submitted = control.submitResult(resultNode);
        Gui.submitResult(submitted.success(), submitted.httpStatus(), submitted.error());

        Gui.stateChange("END");
        Gui.endingTest();
    }

    /** Median of {@code v} (mean of the two middle values for an even count), 0 if empty. */
    private static double median(long[] v) {
        if (v.length == 0) return 0;
        Arrays.sort(v);
        int m = v.length / 2;
        return v.length % 2 == 0 ? (v[m - 1] + (double) v[m]) / 2.0 : v[m];
    }

    // ── Multi-threaded phase runner ────────────────────────────────────────────

    private static List<TransferResult> runPhase(
            int n, String addr, int port, boolean useTls, boolean noTlsVerify,
            int protocol, String token, int duration, int chunkSize,
            boolean upload, boolean intermediate) throws InterruptedException {

        CyclicBarrier barrier = new CyclicBarrier(n);
        List<Future<TransferResult>> futures = new ArrayList<>(n);
        ExecutorService pool = Executors.newFixedThreadPool(n);

        for (int i = 0; i < n; i++) {
            final int tid = i;
            futures.add(pool.submit(() -> {
                try (RmbtConn conn = RmbtConn.connect(addr, port, useTls, noTlsVerify, protocol)) {
                    conn.greeting(token);
                    barrier.await();  // all threads start test simultaneously
                    TransferResult result = upload
                        ? Tests.runUpload(conn, duration, chunkSize, tid, intermediate)
                        : Tests.runDownload(conn, duration, chunkSize, tid);
                    conn.quit();
                    return result;
                } catch (Exception e) {
                    try { barrier.await(); } catch (Exception ignored) {}
                    throw e;
                }
            }));
        }
        pool.shutdown();

        List<TransferResult> results = new ArrayList<>();
        for (int i = 0; i < n; i++) {
            try {
                results.add(futures.get(i).get());
            } catch (ExecutionException e) {
                System.err.printf("[thread %d] dropped (skipping): %s%n", i, e.getCause().getMessage());
            }
        }
        return results;
    }

    private static void printUsage() {
        System.out.println("""
            Usage: java -jar rmbt-client.jar --host URL [options]

            Options:
              -h, --host URL          Control server base URL (required)
              -p, --port PORT         Override test server port
              -u, --uuid UUID         Client UUID (uses/creates ~/.rmbt_client_uuid if omitted)
              -t, --threads N         Force thread count for download and upload
              -d, --duration SECS     Test duration in seconds
                  --ws                Use WebSocket (RMBTws) framing
                  --http              Use plain HTTP upgrade (RMBThttp, default)
                  --no-tls-verify     Skip TLS certificate verification
                  --debug             Print control server JSON
                  --intermediate      Print upload throughput every 40 ms per thread
              -v, --verbose           Emit machine-readable JSON progress on stdout (GUI)
                  --nettype CODE      Network type code (default 98 = LAN)
                  --type TYPE         Client type (default DESKTOP)
                  --platform NAME     Platform label
                  --os / --osver STR  Accepted for compatibility
                  --model NAME        Device model
                  --server_uuid UUID  Preferred measurement-server UUID (prefer_server)
                  --set-version VER   Wrapping app version; reported as device = "App: VER"
                  --user-loop-mode    Mark this run as one loop iteration (sends loopmode_info)
                  --user-loop-mode-max-delay MIN     Max delay between iterations (minutes)
                  --user-loop-mode-test-counter N    0-based iteration index
                  --user-loop-mode-uuid UUID         Server loop UUID; omit on the first iteration
                  --help              Print this help
            """);
    }
}
