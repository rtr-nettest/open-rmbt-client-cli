package at.rtr.rmbt.client;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ObjectNode;

import java.util.concurrent.atomic.AtomicLong;

/**
 * Machine-readable JSON progress interface for the open-rmbt-desktop app.
 *
 * <p>See {@code doc/json_interface.md} for the specification. All JSON output is
 * gated by the {@code -v} flag; when disabled these methods are no-ops (except
 * the progress counter, a cheap atomic used by the interim monitor).
 *
 * <p>Throughput is reported in decimal Mbit/s ({@code bytes * 8 / s / 1e6}),
 * ping RTTs in ms, and {@code pingTimeNs} in ns.
 */
final class Gui {

    private static final ObjectMapper JSON = new ObjectMapper();
    private static volatile boolean enabled = false;
    /** Cumulative bytes transferred in the current transfer phase (all threads). */
    private static final AtomicLong PROGRESS = new AtomicLong(0);
    /** Last state passed to {@link #stateChange}, reported as {@code phase} on ERROR. */
    private static volatile String phase = "";

    private Gui() {}

    static void setEnabled(boolean v) { enabled = v; }
    static boolean enabled() { return enabled; }

    private static void emit(ObjectNode n) {
        if (!enabled) return;
        try { System.out.println(JSON.writeValueAsString(n)); }
        catch (Exception ignored) {}
    }

    // ── Plain-text lifecycle sentinels (backward-compat shim, gui mode only) ────

    static void startingTest() { if (enabled) System.out.println("STARTING TEST."); }
    static void endingTest()   { if (enabled) System.out.println("ENDING TEST."); }

    // ── Messages ────────────────────────────────────────────────────────────

    static void stateChange(String state) {
        phase = state;
        ObjectNode n = JSON.createObjectNode();
        n.put("type", "STATE_CHANGE");
        n.put("time", System.currentTimeMillis());
        n.put("state", state);
        emit(n);
    }

    /**
     * STATE_CHANGE to ERROR, carrying the phase in which the run failed and a
     * human-readable reason. The caller then prints ENDING TEST. and exits non-zero.
     */
    static void error(String msg) {
        ObjectNode n = JSON.createObjectNode();
        n.put("type", "STATE_CHANGE");
        n.put("time", System.currentTimeMillis());
        n.put("state", "ERROR");
        n.put("phase", phase);
        n.put("error", msg);
        emit(n);
    }

    static void uuidInfo(String testUuid, String openTestUuid, String token, String loopUuid) {
        ObjectNode n = JSON.createObjectNode();
        n.put("type", "UUID_INFO");
        n.put("testUuid", testUuid);
        n.put("openTestUuid", openTestUuid);
        n.put("testToken", token);
        n.put("loopUuid", loopUuid); // null when not a loop test
        emit(n);
    }

    static void pingResult(long clientNs, long serverNs, long timeNs) {
        ObjectNode n = JSON.createObjectNode();
        n.put("type", "PING_RESULT");
        n.put("time", System.currentTimeMillis());
        n.put("phase", "PING");
        n.put("pingTimeNs", timeNs);
        n.put("pingClient", clientNs / 1e6); // ns → ms
        n.put("pingServer", serverNs / 1e6); // ns → ms
        n.put("status", "PING");
        emit(n);
    }

    /**
     * Locally measured final result, emitted once before result submission.
     * Units as in the interim messages: decimal Mbit/s and ms.
     */
    static void finalResult(String testUuid, String openTestUuid, String loopUuid,
                            double downMbps, double upMbps,
                            double pingMedianNs, int pingCount,
                            long downBytes, long downNs, int downThreads,
                            long upBytes, long upNs, int upThreads) {
        ObjectNode n = JSON.createObjectNode();
        n.put("type", "FINAL_RESULT");
        n.put("time", System.currentTimeMillis());
        n.put("testUuid", testUuid);
        n.put("openTestUuid", openTestUuid);
        n.put("loopUuid", loopUuid);
        n.put("down", downMbps);
        n.put("up", upMbps);
        n.put("pingMedian", pingMedianNs / 1e6); // ns → ms
        n.put("pingCount", pingCount);
        n.put("downBytes", downBytes);
        n.put("downNs", downNs);
        n.put("downThreads", downThreads);
        n.put("upBytes", upBytes);
        n.put("upNs", upNs);
        n.put("upThreads", upThreads);
        emit(n);
    }

    static void submitResult(boolean success, Integer httpStatus, String error) {
        ObjectNode n = JSON.createObjectNode();
        n.put("type", "SUBMIT_RESULT");
        n.put("time", System.currentTimeMillis());
        n.put("success", success);
        n.put("httpStatus", httpStatus);
        n.put("error", error);
        emit(n);
    }

    private static void transferResult(boolean upload, long bytes, double mbps,
                                       String openTestUuid, double progress) {
        ObjectNode n = JSON.createObjectNode();
        n.put("type", upload ? "UPLOAD_RESULT" : "DOWNLOAD_RESULT");
        n.put("time", System.currentTimeMillis());
        n.put("phase", upload ? "UP" : "DOWN");
        n.put("down", upload ? -1.0 : mbps);
        n.put("up", upload ? mbps : -1.0);
        n.put("bytes", bytes);
        if (openTestUuid != null) n.put("testOpenUuid", openTestUuid);
        n.put("progress", progress);
        n.put("status", upload ? "UP" : "DOWN");
        emit(n);
    }

    // ── Interim throughput monitor ────────────────────────────────────────────

    static void addProgress(long bytes) { PROGRESS.addAndGet(bytes); }

    /**
     * Reset the progress counter and, in gui mode, start a daemon thread emitting
     * a DOWNLOAD_RESULT/UPLOAD_RESULT every 250 ms until {@link Monitor#stop()}.
     */
    static Monitor startTransferMonitor(boolean upload, int durationSecs, String openTestUuid) {
        PROGRESS.set(0);
        Monitor m = new Monitor(upload, durationSecs, openTestUuid);
        if (enabled) m.start();
        return m;
    }

    static final class Monitor {
        private final boolean upload;
        private final int     durationSecs;
        private final String  openTestUuid;
        private volatile boolean stop = false;
        private Thread thread;

        Monitor(boolean upload, int durationSecs, String openTestUuid) {
            this.upload = upload;
            this.durationSecs = durationSecs;
            this.openTestUuid = openTestUuid;
        }

        void start() {
            final long startNs = System.nanoTime();
            thread = new Thread(() -> {
                while (!stop) {
                    try { Thread.sleep(250); } catch (InterruptedException e) { break; }
                    double secs = (System.nanoTime() - startNs) / 1e9;
                    if (secs <= 0) continue;
                    long bytes  = PROGRESS.get();
                    double mbps = bytes * 8.0 / secs / 1e6; // decimal Mbit/s
                    double frac = Math.min(1.0, secs / Math.max(1, durationSecs));
                    transferResult(upload, bytes, mbps, openTestUuid, frac);
                }
            }, "gui-transfer-monitor");
            thread.setDaemon(true);
            thread.start();
        }

        void stop() {
            stop = true;
            if (thread != null) {
                thread.interrupt();
                try { thread.join(500); } catch (InterruptedException ignored) {}
            }
        }
    }
}
