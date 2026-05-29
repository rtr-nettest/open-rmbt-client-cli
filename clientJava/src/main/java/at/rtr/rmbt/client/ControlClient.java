package at.rtr.rmbt.client;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;

import java.io.IOException;
import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.time.Duration;
import java.time.Instant;

/** Communicates with the RMBT control server. */
final class ControlClient {

    private static final ObjectMapper JSON = new ObjectMapper();
    private static final HttpClient   HTTP = HttpClient.newBuilder()
            .connectTimeout(Duration.ofSeconds(30))
            .build();

    private final String  host;
    private final boolean debug;
    private final String  version;

    ControlClient(String host, boolean debug, String version) {
        this.host    = host.stripTrailing().replaceAll("/+$", "");
        this.debug   = debug;
        this.version = version;
    }

    // ── /settings ─────────────────────────────────────────────────────────────

    /**
     * POST /settings to register or re-identify the client.
     * @param storedUuid previously stored UUID, or null on first run
     * @return the server-assigned client UUID
     */
    String requestSettings(String storedUuid) throws IOException, InterruptedException {
        ObjectNode body = JSON.createObjectNode();
        body.put("name",     "RMBT");
        body.put("type",     "DESKTOP");
        body.put("language", "en");
        body.put("timezone", "UTC");
        body.put("softwareRevision",    version);
        body.put("softwareVersionName", version);
        body.put("terms_and_conditions_accepted", true);
        if (storedUuid != null) body.put("uuid", storedUuid);

        String url = host + "/RMBTControlServer/settings";
        JsonNode resp = post(url, body);

        JsonNode settings = resp.path("settings");
        if (settings.isArray() && !settings.isEmpty()) {
            String uuid = settings.get(0).path("uuid").asText(null);
            if (uuid != null && !uuid.isBlank()) return uuid;
        }
        throw new IOException("Settings response contained no UUID");
    }

    // ── /testRequest ──────────────────────────────────────────────────────────

    TestParams requestTest(String uuid, boolean useWs) throws IOException, InterruptedException {
        ObjectNode body = JSON.createObjectNode();
        body.put("uuid",             uuid);
        body.put("client",           useWs ? "RMBTws" : "RMBT");
        body.put("version",          "0.9");
        body.put("type",             "DESKTOP");
        body.put("softwareVersion",  version);
        body.put("softwareRevision", version);
        body.put("language",         "en");
        body.put("timezone",         "UTC");
        body.put("time",             Instant.now().toEpochMilli());
        if (!useWs) {
            body.putObject("capabilities").put("RMBThttp", true);
        }

        String url = host + "/RMBTControlServer/testRequest";
        JsonNode resp = post(url, body);

        JsonNode errors = resp.path("error");
        if (errors.isArray() && !errors.isEmpty()) {
            StringBuilder sb = new StringBuilder();
            for (JsonNode e : errors) { if (!sb.isEmpty()) sb.append("; "); sb.append(e.asText()); }
            throw new IOException("Control server error(s): " + sb);
        }

        int port = 443;
        JsonNode portNode = resp.path("test_server_port");
        if (portNode.isNumber())      port = portNode.asInt();
        else if (portNode.isTextual()) port = Integer.parseInt(portNode.asText("443").trim());

        return new TestParams(
            resp.path("test_token").asText(),
            resp.path("test_uuid").asText(null),
            resp.path("open_test_uuid").asText(null),
            resp.path("test_server_address").asText(),
            port,
            resp.path("test_server_encryption").asBoolean(true),
            asInt(resp, "test_duration",   10),
            asInt(resp, "test_numthreads", 4),
            asInt(resp, "test_wait",       0),
            resp.path("test_server_type").asText("")
        );
    }

    // ── /result ───────────────────────────────────────────────────────────────

    void submitResult(ObjectNode result) {
        String url = host + "/RMBTControlServer/result";
        try {
            post(url, result);
        } catch (Exception e) {
            System.err.println("Warning: result submission failed: " + e.getMessage());
        }
    }

    // ── helpers ───────────────────────────────────────────────────────────────

    private JsonNode post(String url, ObjectNode body) throws IOException, InterruptedException {
        String bodyStr = JSON.writeValueAsString(body);
        if (debug) {
            System.err.println("[debug] POST " + url);
            System.err.println("[debug] request body:\n" + JSON.writerWithDefaultPrettyPrinter().writeValueAsString(body));
        }
        HttpRequest req = HttpRequest.newBuilder()
                .uri(URI.create(url))
                .header("Content-Type", "application/json")
                .POST(HttpRequest.BodyPublishers.ofString(bodyStr))
                .timeout(Duration.ofSeconds(30))
                .build();
        HttpResponse<String> resp = HTTP.send(req, HttpResponse.BodyHandlers.ofString());
        if (debug) System.err.println("[debug] response body:\n" + resp.body());
        if (resp.statusCode() >= 400)
            throw new IOException("HTTP " + resp.statusCode() + ": " + resp.body().strip());
        return JSON.readTree(resp.body());
    }

    static ObjectNode buildResult(
            String clientUuid, String clientName, String version, TestParams params, int port,
            PingResult[] pings, TransferResult[] dlResults, TransferResult[] ulResults) {

        long dlBytes = 0, dlNs = 0, ulBytes = 0, ulNs = 0;
        for (TransferResult r : dlResults) { dlBytes += r.bytes(); if (r.elapsedNs() > dlNs) dlNs = r.elapsedNs(); }
        for (TransferResult r : ulResults) { ulBytes += r.bytes(); if (r.elapsedNs() > ulNs) ulNs = r.elapsedNs(); }
        if (dlNs == 0) dlNs = 1;
        if (ulNs == 0) ulNs = 1;

        long dlKbps = (long)(dlBytes * 8e6 / dlNs);
        long ulKbps = (long)(ulBytes * 8e6 / ulNs);
        long pingShortestServer = 0;
        for (PingResult p : pings) if (p.serverNs() < pingShortestServer || pingShortestServer == 0) pingShortestServer = p.serverNs();

        ObjectNode r = JSON.createObjectNode();
        r.put("client_language",         "en");
        r.put("client_name",             clientName);
        r.put("client_uuid",             clientUuid);
        r.put("client_version",          version);
        r.put("client_software_version", version);
        r.putArray("geoLocations");
        r.put("model",          "Client CLI Java");
        r.put("network_type",   98);
        r.put("platform",       "CLI");
        r.put("product",        "rmbt-client-java");
        r.put("test_bytes_download",  dlBytes);
        r.put("test_bytes_upload",    ulBytes);
        r.put("test_nsec_download",   dlNs);
        r.put("test_nsec_upload",     ulNs);
        r.put("test_num_threads",     dlResults.length);
        r.put("num_threads_ul",       ulResults.length);
        r.put("test_ping_shortest",   pingShortestServer);
        r.put("test_speed_download",  dlKbps);
        r.put("test_speed_upload",    ulKbps);
        r.put("test_token",           params.token());
        if (params.testUuid() != null) r.put("test_uuid", params.testUuid());
        r.put("time",       Instant.now().toEpochMilli());
        r.put("timezone",   "UTC");
        r.put("type",       "DESKTOP");
        r.put("version_code", "1");
        r.put("user_server_selection", false);
        r.put("test_status",  "0");
        r.put("test_port_remote", port);

        ArrayNode pingsNode = r.putArray("pings");
        for (PingResult p : pings) {
            pingsNode.addObject()
                .put("value",        p.clientNs())
                .put("value_server", p.serverNs())
                .put("time_ns",      p.timeNs());
        }

        ArrayNode sdNode = r.putArray("speed_detail");
        for (TransferResult t : dlResults) {
            for (long[] s : t.samples()) {
                sdNode.addObject().put("direction", "download")
                    .put("thread", t.threadId()).put("time", s[1]).put("bytes", s[0]);
            }
        }
        for (TransferResult t : ulResults) {
            for (long[] s : t.samples()) {
                sdNode.addObject().put("direction", "upload")
                    .put("thread", t.threadId()).put("time", s[1]).put("bytes", s[0]);
            }
        }
        return r;
    }

    private static int asInt(JsonNode node, String field, int def) {
        JsonNode n = node.path(field);
        if (n.isNumber())  return n.asInt();
        if (n.isTextual()) { try { return Integer.parseInt(n.asText().trim()); } catch (NumberFormatException ignored) {} }
        return def;
    }
}
