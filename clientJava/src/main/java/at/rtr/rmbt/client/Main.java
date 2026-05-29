package at.rtr.rmbt.client;

import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicReference;

public final class Main {

    private static final int    MAX_THREADS = 20;
    private static final String VERSION     = loadVersion();

    private static String loadVersion() {
        try (var is = Main.class.getResourceAsStream("/version.properties")) {
            if (is == null) return "dev";
            var props = new java.util.Properties();
            props.load(is);
            String raw = props.getProperty("version", "dev").trim();
            // Strip "-g<abbrev>" suffix: "v1.0-5-gabcdef" → "v1.0-5"
            int i = raw.lastIndexOf('-');
            if (i > 0 && i + 1 < raw.length() && raw.charAt(i + 1) == 'g') {
                return raw.substring(0, i);
            }
            return raw;
        } catch (Exception e) {
            return "dev";
        }
    }

    public static void main(String[] args) throws Exception {
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

        ControlClient control = new ControlClient(host, debug, VERSION);

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

        System.out.println("Contacting control server: " + host);
        TestParams params = control.requestTest(uuid, forceWs);

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
        System.out.println("\nPing (1 s, 10-100 pings):");
        List<PingResult> pings;
        try (RmbtConn conn = RmbtConn.connect(
                params.serverAddr(), port, params.encryption(), noTlsVerify, protocol)) {
            conn.greeting(params.token());
            pings = Tests.runPing(conn, 1.0, 10, 100);
            conn.quit();
        }

        // ── Step 4: download ──────────────────────────────────────────────────
        System.out.printf("%nDownload (%d thread(s), %ds):%n", dlThreads, duration);
        List<TransferResult> dlResults = runPhase(
                dlThreads, params.serverAddr(), port, params.encryption(),
                noTlsVerify, protocol, params.token(),
                duration, dlChunkSize, false, false);
        if (dlResults.isEmpty()) { System.err.println("All download threads failed"); System.exit(1); }

        // ── Step 5: upload ────────────────────────────────────────────────────
        System.out.printf("%nUpload (%d thread(s), %ds):%n", ulThreads, duration);
        List<TransferResult> ulResults = runPhase(
                ulThreads, params.serverAddr(), port, params.encryption(),
                noTlsVerify, protocol, params.token(),
                duration, ulChunkSize, true, intermediate);
        if (ulResults.isEmpty()) { System.err.println("All upload threads failed"); System.exit(1); }

        // ── Step 6: aggregate ─────────────────────────────────────────────────
        long dlBytes = 0, dlNs = 0, ulBytes = 0, ulNs = 0;
        for (TransferResult r : dlResults) { dlBytes += r.bytes(); dlNs = Math.max(dlNs, r.elapsedNs()); }
        for (TransferResult r : ulResults) { ulBytes += r.bytes(); ulNs = Math.max(ulNs, r.elapsedNs()); }
        if (dlNs == 0) dlNs = 1;
        if (ulNs == 0) ulNs = 1;

        double dlMbps = dlBytes * 8.0 / (dlNs / 1e9) / 1e6;
        double ulMbps = ulBytes * 8.0 / (ulNs / 1e9) / 1e6;

        long[] clientNsArr = pings.stream().mapToLong(PingResult::clientNs).toArray();
        Arrays.sort(clientNsArr);
        long pingMinClient  = clientNsArr.length > 0 ? clientNsArr[0] : 0;
        long pingMedian     = clientNsArr.length > 0 ? clientNsArr[clientNsArr.length / 2] : 0;

        System.out.println("\n=== Results ===");
        System.out.printf("Ping (min):     %7.2f ms  (%d pings)%n", pingMinClient / 1e6, pings.size());
        System.out.printf("Ping (median):  %7.2f ms%n", pingMedian / 1e6);
        System.out.printf("Download:       %7.2f Mbit/s  (%d bytes in %.2fs, %d thread(s))%n",
                dlMbps, dlBytes, dlNs / 1e9, dlResults.size());
        System.out.printf("Upload:         %7.2f Mbit/s  (%d bytes in %.2fs, %d thread(s))%n",
                ulMbps, ulBytes, ulNs / 1e9, ulResults.size());

        // ── Step 7: submit ────────────────────────────────────────────────────
        String clientName = protocol == RmbtConn.PROTO_WS ? "RMBTws" : "RMBT";
        var resultNode = ControlClient.buildResult(
                uuid, clientName, VERSION, params, port,
                pings.toArray(PingResult[]::new),
                dlResults.toArray(TransferResult[]::new),
                ulResults.toArray(TransferResult[]::new));
        resultNode.put("time", testBeginMs);

        if (params.openTestUuid() != null)
            System.out.println("Result:         https://www.netztest.at/share/" + params.openTestUuid());

        System.out.println("\nSubmitting results to control server...");
        control.submitResult(resultNode);
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
                  --help              Print this help
            """);
    }
}
