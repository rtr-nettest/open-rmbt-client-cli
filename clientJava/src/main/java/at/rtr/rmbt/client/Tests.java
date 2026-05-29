package at.rtr.rmbt.client;

import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

/** Ping, download, and upload test phases. */
final class Tests {

    private static final long SAMPLE_INTERVAL_NS = 40_000_000L; // 40 ms

    private Tests() {}

    static long parseTimeNs(String line) throws IOException {
        String[] parts = line.split("\\s+");
        if (parts.length < 2) throw new IOException("Invalid TIME line: " + line);
        try { return Long.parseUnsignedLong(parts[1]); }
        catch (NumberFormatException e) { throw new IOException("Invalid TIME line: " + line); }
    }

    // ── Ping ──────────────────────────────────────────────────────────────────

    static List<PingResult> runPing(RmbtConn conn,
                                    double durationSecs, int minPings, int maxPings)
            throws IOException {
        long phaseStart = System.nanoTime();
        long deadline   = phaseStart + (long)(durationSecs * 1e9);
        List<PingResult> results = new ArrayList<>();

        while (results.size() < maxPings) {
            if (System.nanoTime() >= deadline && results.size() >= minPings) break;

            String accept = conn.readLine();
            if (!accept.contains("PING"))
                throw new IOException("Expected ACCEPT with PING, got: " + accept);

            long t0      = System.nanoTime();
            long timeNs  = t0 - phaseStart;
            conn.writeLine("PING");

            String pong     = conn.readLine();
            long clientNs   = System.nanoTime() - t0;

            if (!"PONG".equals(pong))
                throw new IOException("Expected PONG, got: " + pong);

            conn.writeLine("OK");
            long serverNs = parseTimeNs(conn.readLine());

            System.out.printf("  ping  client=%.3fms  server=%.3fms%n",
                    clientNs / 1e6, serverNs / 1e6);
            results.add(new PingResult(clientNs, serverNs, timeNs));
        }
        return results;
    }

    // ── Download ──────────────────────────────────────────────────────────────

    static TransferResult runDownload(RmbtConn conn, int durationSecs,
                                      int chunkSize, int threadId) throws IOException {
        String accept = conn.readLine();
        if (!accept.contains("GETTIME"))
            throw new IOException("Expected ACCEPT with GETTIME, got: " + accept);

        conn.writeLine("GETTIME " + durationSecs + " " + chunkSize);

        byte[]       buf        = new byte[chunkSize];
        long         t0         = System.nanoTime();
        long         total      = 0;
        List<long[]> samples    = new ArrayList<>();
        long         lastSample = t0;

        for (;;) {
            conn.readExact(buf);
            total += chunkSize;

            long now = System.nanoTime();
            if (now - lastSample >= SAMPLE_INTERVAL_NS) {
                samples.add(new long[]{total, now - t0});
                lastSample = now;
            }

            if ((buf[chunkSize - 1] & 0xFF) == 0xFF) break;
        }

        conn.writeLine("OK");
        long elapsedNs = parseTimeNs(conn.readLine());

        // Final entry uses the server-reported elapsed time.
        samples.add(new long[]{total, elapsedNs});

        System.out.printf("  dl[%2d]  %.2f Mbit/s  (%d bytes in %.3fs, client %.3fs)%n",
                threadId,
                total * 8.0 / (elapsedNs / 1e9) / 1e6,
                total, elapsedNs / 1e9,
                (System.nanoTime() - t0) / 1e9);

        return new TransferResult(total, elapsedNs, threadId, samples);
    }

    // ── Upload ────────────────────────────────────────────────────────────────

    /** Speed samples are always collected; {@code intermediate} only controls printing. */
    static TransferResult runUpload(RmbtConn conn, int durationSecs,
                                    int chunkSize, int threadId,
                                    boolean intermediate) throws IOException {
        String accept = conn.readLine();
        if (!accept.contains("PUT"))
            throw new IOException("Expected ACCEPT with PUT/PUTNORESULT, got: " + accept);

        conn.writeLine("PUTNORESULT " + chunkSize);

        String ok = conn.readLine();
        if (!"OK".equals(ok))
            throw new IOException("Expected OK after PUTNORESULT, got: " + ok);

        byte[] chunk = new byte[chunkSize];
        for (int i = 0; i < chunkSize; i++) chunk[i] = (byte)(i & 0xFF);

        long deadline        = System.nanoTime() + (long)durationSecs * 1_000_000_000L;
        long t0              = System.nanoTime();
        long total           = 0;
        List<long[]> samples = new ArrayList<>();
        long lastSample      = t0;
        long lastSampleBytes = 0;

        for (;;) {
            boolean terminal = System.nanoTime() >= deadline;
            chunk[chunkSize - 1] = terminal ? (byte)0xFF : 0x00;
            conn.writeBytes(chunk);
            total += chunkSize;

            // Don't record a sample on the terminal iteration — the authoritative
            // final entry (with server-reported time) is always pushed after the loop.
            if (!terminal) {
                long now = System.nanoTime();
                if (now - lastSample >= SAMPLE_INTERVAL_NS) {
                    samples.add(new long[]{total, now - t0});

                    if (intermediate) {
                        double dt = (now - lastSample) / 1e9;
                        long   db = total - lastSampleBytes;
                        if (dt > 0) System.out.printf("  ul[%2d] +%.2f Mbit/s%n",
                                threadId, db * 8.0 / dt / 1e6);
                    }

                    lastSample      = now;
                    lastSampleBytes = total;
                }
            }

            if (terminal) break;
        }
        conn.flush();

        long elapsedNs = parseTimeNs(conn.readLine());

        // Final entry: total bytes with server-reported elapsed time.
        samples.add(new long[]{total, elapsedNs});

        System.out.printf("  ul[%2d]  %.2f Mbit/s  (%d bytes in %.3fs, client %.3fs)%n",
                threadId,
                total * 8.0 / (elapsedNs / 1e9) / 1e6,
                total, elapsedNs / 1e9,
                (System.nanoTime() - t0) / 1e9);

        return new TransferResult(total, elapsedNs, threadId, samples);
    }
}
