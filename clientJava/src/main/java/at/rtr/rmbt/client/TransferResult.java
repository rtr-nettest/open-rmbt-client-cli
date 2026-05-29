package at.rtr.rmbt.client;

import java.util.List;

/**
 * @param bytes      total bytes transferred in this phase
 * @param elapsedNs  server-reported phase duration in nanoseconds
 * @param threadId   0-based thread index
 * @param samples    intermediate speed samples: each {@code long[2]} is
 *                   {@code {cumulativeBytes, timeNsFromPhaseStart}}.
 *                   The last entry always uses the server-reported elapsed time.
 */
record TransferResult(long bytes, long elapsedNs, int threadId, List<long[]> samples) {}
