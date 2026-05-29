package at.rtr.rmbt.client;

import java.io.IOException;

/** Single-thread 2-second download pre-test to determine chunk size and thread counts. */
final class Pretest {

    private static final long PRETEST_DURATION_NS = 2_000_000_000L;
    private static final int  MIN_CHUNK           = 1024;
    private static final int  MAX_CHUNK           = 4 * 1024 * 1024;

    private static final double[] DL_THRESH = {100.0, 1.0, 0.0};
    private static final int[]    DL_COUNT  = {  5,   3,   1 };
    private static final double[] UL_THRESH = {150.0, 80.0, 30.0, 0.0};
    private static final int[]    UL_COUNT  = {  5,    3,    2,   1 };

    private Pretest() {}

    static PretestResult run(String addr, int port, boolean useTls, boolean noTlsVerify,
                             int protocol, String token, int maxThreads) throws IOException {
        System.out.println("\nPre-test: measuring baseline throughput...");

        try (RmbtConn conn = RmbtConn.connect(addr, port, useTls, noTlsVerify, protocol)) {
            conn.greeting(token);

            int serverMin = conn.chunkSizeMin;
            int serverMax = conn.chunkSizeMax;

            long tStart    = System.nanoTime();
            int  cs        = Math.max(serverMin, MIN_CHUNK);
            int  n         = 1;
            long lastBytes = 0;
            long lastNs    = 0;  // TIME from the last (largest) GETCHUNKS batch
            long rttNs     = 0;  // TIME of the first (tiny) batch ≈ round-trip time

            while (System.nanoTime() - tStart < PRETEST_DURATION_NS) {
                String accept = conn.readLine();
                if (!accept.contains("GETCHUNKS"))
                    throw new IOException("Pre-test: expected GETCHUNKS, got: " + accept);

                conn.writeLine("GETCHUNKS " + n + " " + cs);

                byte[] buf = new byte[cs];
                for (int i = 0; i < n; i++) conn.readExact(buf);
                lastBytes = (long)n * cs;

                conn.writeLine("OK");
                String timeLine = conn.readLine();
                try {
                    long t = Long.parseLong(timeLine.split("\\s+")[1]);
                    if (rttNs == 0) rttNs = t;  // first tiny batch ≈ RTT
                    lastNs = t;
                } catch (Exception ignored) {}

                if (n >= 8) {
                    cs = Math.min(cs * 2, Math.min(serverMax, MAX_CHUNK));
                    n  = 1;
                } else {
                    n *= 2;
                }
            }

            // TIME = transmission_time + RTT.  Subtract the RTT estimate (first tiny
            // batch, where transmission_time ≈ 0) to get actual throughput.
            long transferNs = Math.max(0, lastNs - rttNs);
            double bps = transferNs > 0 ? lastBytes / (transferNs / 1e9)
                       : lastNs   > 0  ? lastBytes / (lastNs    / 1e9)
                       : lastBytes / ((System.nanoTime() - tStart) / 1e9);
            double mbps = bps * 8.0 / 1e6;

            int ideal    = (int)(bps / 50.0);
            int rounded  = ((ideal + 512) / 1024) * 1024;
            int floor    = Math.max(serverMin, MIN_CHUNK);
            int cap      = Math.min(serverMax, MAX_CHUNK);
            int chunkSize = Math.max(floor, Math.min(cap, rounded == 0 ? floor : rounded));

            int dl = Math.min(maxThreads, threadsFor(mbps, DL_THRESH, DL_COUNT));
            int ul = Math.min(maxThreads, threadsFor(mbps, UL_THRESH, UL_COUNT));

            System.out.printf("  pre-test: %.1f Mbit/s → chunk=%d KiB  dl_threads=%d  ul_threads=%d%n",
                    mbps, chunkSize / 1024, dl, ul);

            conn.quit();
            return new PretestResult(chunkSize, dl, ul);
        }
    }

    private static int threadsFor(double mbps, double[] thresh, int[] count) {
        for (int i = 0; i < thresh.length; i++)
            if (mbps >= thresh[i]) return count[i];
        return 1;
    }
}
