#include "control.h"
#include "gui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <curl/curl.h>
#include <stdarg.h>

/* Append formatted text at *pos, never writing past bsz; on truncation *pos
 * is clamped to bsz - 1 so later calls cannot underflow (bsz - *pos). */
static void appendf(char *buf, size_t bsz, size_t *pos, const char *fmt, ...)
{
    if (*pos >= bsz - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *pos, bsz - *pos, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= bsz - *pos) *pos = bsz - 1;
    else *pos += (size_t)n;
}

/* ── HTTP response buffer ────────────────────────────────────────────────────── */

typedef struct { char *data; size_t size; } CurlBuf;

static size_t curl_write_cb(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t total = size * nmemb;
    CurlBuf *buf = userp;
    char *tmp = realloc(buf->data, buf->size + total + 1);
    if (!tmp) return 0;
    buf->data = tmp;
    memcpy(buf->data + buf->size, contents, total);
    buf->size += total;
    buf->data[buf->size] = '\0';
    return total;
}

/* ── Minimal JSON helpers ────────────────────────────────────────────────────── */

/* Extract the first occurrence of "key": "value" → value into out (max outlen). */
static int json_get_str(const char *json, const char *key,
                        char *out, size_t outlen)
{
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return -1;
    p += strlen(needle);
    while (*p == ' ' || *p == ':' || *p == ' ') p++;
    if (*p != '"') return -1;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < outlen - 1) out[i++] = *p++;
    out[i] = '\0';
    return 0;
}

/* Extract "key": <number> (integer or quoted integer). */
static int json_get_u64(const char *json, const char *key, uint64_t *out)
{
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return -1;
    p += strlen(needle);
    while (*p == ' ' || *p == ':') p++;
    if (*p == '"') p++;  /* handle quoted numbers */
    if (*p < '0' || *p > '9') return -1;
    char *end;
    *out = (uint64_t)strtoull(p, &end, 10);
    return (end > p) ? 0 : -1;
}

static int json_get_bool(const char *json, const char *key, int *out)
{
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return -1;
    p += strlen(needle);
    while (*p == ' ' || *p == ':') p++;
    if (strncmp(p, "true",  4) == 0) { *out = 1; return 0; }
    if (strncmp(p, "false", 5) == 0) { *out = 0; return 0; }
    return -1;
}

/* Check whether the "error" array is non-empty. */
static int json_has_errors(const char *json)
{
    const char *p = strstr(json, "\"error\"");
    if (!p) return 0;
    p += 7;
    while (*p == ' ' || *p == ':') p++;
    if (*p != '[') return 0;
    p++;
    while (*p == ' ') p++;
    return *p != ']';
}

/* ── Control server timestamp ────────────────────────────────────────────────── */

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}

/* ── POST helper ─────────────────────────────────────────────────────────────── */

static int do_post(const char *url, const char *body,
                   int debug, CurlBuf *resp, long *http_code_out)
{
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    resp->data = malloc(1);
    resp->data[0] = '\0';
    resp->size = 0;

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL,            url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        30L);

    if (debug) {
        fprintf(stderr, "[debug] POST %s\n[debug] request body:\n%s\n", url, body);
    }

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code_out) *http_code_out = http_code;
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        gui_report_error("curl error: %s\n", curl_easy_strerror(rc));
        free(resp->data); resp->data = NULL;
        return -1;
    }
    if (http_code >= 400) {
        gui_report_error("HTTP %ld: %s\n", http_code, resp->data);
        free(resp->data); resp->data = NULL;
        return -1;
    }

    if (debug)
        fprintf(stderr, "[debug] response body:\n%s\n", resp->data);

    return 0;
}

/* ── Public: settings (registration / re-identification) ────────────────────── */

int control_request_settings(const char *host, const char *uuid_in,
                              int debug, char *uuid_out, size_t uuid_out_len)
{
    char base_buf[256];
    strncpy(base_buf, host, sizeof(base_buf) - 1);
    size_t l = strlen(base_buf);
    while (l > 0 && base_buf[l-1] == '/') base_buf[--l] = '\0';

    char url[512];
    snprintf(url, sizeof(url), "%s/RMBTControlServer/settings", base_buf);

    char body[512];
    if (uuid_in && *uuid_in) {
        snprintf(body, sizeof(body),
            "{"
            "\"name\":\"RMBT\","
            "\"type\":\"DESKTOP\","
            "\"uuid\":\"%s\","
            "\"language\":\"en\","
            "\"timezone\":\"UTC\","
            "\"softwareRevision\":\"%s\","
            "\"softwareVersionName\":\"%s\","
            "\"terms_and_conditions_accepted\":true"
            "}", uuid_in, GIT_REVISION, GIT_REVISION);
    } else {
        snprintf(body, sizeof(body),
            "{"
            "\"name\":\"RMBT\","
            "\"type\":\"DESKTOP\","
            "\"language\":\"en\","
            "\"timezone\":\"UTC\","
            "\"softwareRevision\":\"%s\","
            "\"softwareVersionName\":\"%s\","
            "\"terms_and_conditions_accepted\":true"
            "}", GIT_REVISION, GIT_REVISION);
    }

    CurlBuf resp = {NULL, 0};
    if (do_post(url, body, debug, &resp, NULL) < 0) return -1;

    /*
     * Response: {"settings":[{ ..., "servers_ws":[{"uuid":"..."},...], ..., "uuid":"CLIENT-UUID", ...}]}
     *
     * The client UUID is at depth 1 inside settings[0].  Nested objects like
     * servers_ws[] also contain "uuid" keys (test-server UUIDs) at deeper depths —
     * the naive first-match approach picks one of those by mistake.
     *
     * Walk settings[0] character-by-character, tracking brace/bracket depth, and
     * accept only a "uuid" key found at depth 1 (directly inside settings[0]).
     */
    int found = -1;
    const char *data = resp.data ? resp.data : "";
    const char *arr = strstr(data, "\"settings\"");
    if (arr) {
        arr = strchr(arr, '[');          /* start of settings array  */
        if (arr) {
            const char *obj = strchr(arr + 1, '{');  /* start of settings[0] */
            if (obj) {
                int depth = 0;
                const char *p = obj;
                while (*p && found < 0) {
                    if (*p == '{' || *p == '[') { depth++; p++; continue; }
                    if (*p == '}' || *p == ']') {
                        depth--;
                        if (depth == 0) break; /* end of settings[0] */
                        p++; continue;
                    }
                    if (*p == '"') {
                        if (depth == 1 && strncmp(p, "\"uuid\"", 6) == 0) {
                            /* "uuid" directly inside settings[0] — this is the client UUID */
                            p += 6;
                            while (*p == ' ' || *p == ':') p++;
                            if (*p == '"') {
                                p++;
                                size_t i = 0;
                                while (*p && *p != '"' && i < uuid_out_len - 1)
                                    uuid_out[i++] = *p++;
                                uuid_out[i] = '\0';
                                if (i >= 36) found = 0;
                            }
                        } else {
                            /* skip any other string value to avoid false matches */
                            p++;
                            while (*p && *p != '"') {
                                if (*p == '\\') p++;
                                if (*p) p++;
                            }
                            if (*p) p++; /* skip closing quote */
                        }
                        continue;
                    }
                    p++;
                }
            }
        }
    }

    free(resp.data);
    if (found < 0) {
        gui_report_error("settings response contained no UUID\n");
        return -1;
    }
    return 0;
}

/* ── Public: request test ────────────────────────────────────────────────────── */

int control_request_test(const char *host, const char *uuid,
                         int use_ws, const char *prefer_server,
                         int loop_mode, int loop_max_delay,
                         int loop_test_counter, const char *loop_uuid,
                         int debug, TestParams *out)
{
    char url[512];
    const char *base = host;
    /* strip trailing slash */
    char base_buf[256];
    strncpy(base_buf, host, sizeof(base_buf) - 1);
    size_t l = strlen(base_buf);
    while (l > 0 && base_buf[l-1] == '/') base_buf[--l] = '\0';
    base = base_buf;

    snprintf(url, sizeof(url), "%s/RMBTControlServer/testRequest", base);

    const char *client_id = use_ws ? "RMBTws" : "RMBT";
    uint64_t ts = now_ms();

    /* Optional user server selection: request a specific measurement server. */
    char server_frag[128] = "";
    if (prefer_server && *prefer_server) {
        snprintf(server_frag, sizeof(server_frag),
                 ",\"prefer_server\":\"%s\",\"user_server_selection\":true",
                 prefer_server);
    }

    /*
     * Loop mode: the server recognises a loop test by the presence of
     * loopmode_info. loop_uuid is null on the first iteration (server mints one)
     * and echoed thereafter. See doc/json_interface.md.
     */
    char loop_frag[384];
    if (loop_mode) {
        char uuid_field[160];
        if (loop_uuid && *loop_uuid)
            snprintf(uuid_field, sizeof(uuid_field), "\"loop_uuid\":\"%s\"", loop_uuid);
        else
            snprintf(uuid_field, sizeof(uuid_field), "\"loop_uuid\":null");
        snprintf(loop_frag, sizeof(loop_frag),
                 ",\"measurement_type\":\"LOOP_ACTIVE\",\"loopmode\":true,"
                 "\"loopmode_info\":{\"max_delay\":%d,\"max_movement\":0,"
                 "\"max_tests\":0,\"test_counter\":%d,%s}",
                 loop_max_delay, loop_test_counter, uuid_field);
    } else {
        snprintf(loop_frag, sizeof(loop_frag), ",\"measurement_type\":\"REGULAR\"");
    }

    char body[1536];
    if (uuid && *uuid) {
        snprintf(body, sizeof(body),
            "{"
            "\"uuid\":\"%s\","
            "\"client\":\"%s\","
            "\"version\":\"0.9\","
            "\"type\":\"DESKTOP\","
            "\"softwareVersion\":\"%s\","
            "\"softwareRevision\":\"%s\","
            "\"language\":\"en\","
            "\"timezone\":\"UTC\","
            "\"time\":%llu"
            "%s"
            "%s"
            "%s"
            "}",
            uuid, client_id, GIT_REVISION, GIT_REVISION, (unsigned long long)ts, loop_frag, server_frag,
            use_ws ? "" : ",\"capabilities\":{\"RMBThttp\":true}");
    } else {
        snprintf(body, sizeof(body),
            "{"
            "\"client\":\"%s\","
            "\"version\":\"0.9\","
            "\"type\":\"DESKTOP\","
            "\"softwareVersion\":\"%s\","
            "\"softwareRevision\":\"%s\","
            "\"language\":\"en\","
            "\"timezone\":\"UTC\","
            "\"time\":%llu"
            "%s"
            "%s"
            "%s"
            "}",
            client_id, GIT_REVISION, GIT_REVISION, (unsigned long long)ts, loop_frag, server_frag,
            use_ws ? "" : ",\"capabilities\":{\"RMBThttp\":true}");
    }

    CurlBuf resp = {NULL, 0};
    if (do_post(url, body, debug, &resp, NULL) < 0) return -1;

    if (json_has_errors(resp.data)) {
        const char *ep = strstr(resp.data, "\"error\"");
        gui_report_error("Control server error: %s\n", ep ? ep : resp.data);
        free(resp.data);
        return -1;
    }

    memset(out, 0, sizeof(*out));

    json_get_str(resp.data, "test_token",          out->token,          sizeof(out->token));
    json_get_str(resp.data, "test_uuid",           out->test_uuid,      sizeof(out->test_uuid));
    json_get_str(resp.data, "open_test_uuid",      out->open_test_uuid, sizeof(out->open_test_uuid));
    json_get_str(resp.data, "test_server_address", out->server_addr,    sizeof(out->server_addr));
    json_get_str(resp.data, "test_server_type",    out->server_type,    sizeof(out->server_type));
    json_get_str(resp.data, "loop_uuid",           out->loop_uuid,      sizeof(out->loop_uuid));
    /* Treat the literal string "null" as "no loop UUID". */
    if (strcmp(out->loop_uuid, "null") == 0) out->loop_uuid[0] = '\0';

    uint64_t port_v = 443;
    json_get_u64(resp.data, "test_server_port", &port_v);
    out->server_port = (uint16_t)port_v;

    int enc = 1;
    json_get_bool(resp.data, "test_server_encryption", &enc);
    out->encryption = enc;

    uint64_t dur = 10, threads = 4, wait = 0;
    json_get_u64(resp.data, "test_duration",   &dur);
    json_get_u64(resp.data, "test_numthreads", &threads);
    json_get_u64(resp.data, "test_wait",       &wait);
    out->duration    = (uint32_t)dur;
    out->num_threads = (uint32_t)threads;
    out->wait        = (uint32_t)wait;

    free(resp.data);

    if (!out->token[0]) {
        gui_report_error("Control server: missing test_token\n");
        return -1;
    }
    if (!out->server_addr[0]) {
        gui_report_error("Control server: missing test_server_address\n");
        return -1;
    }
    return 0;
}

/* ── Public: submit result ───────────────────────────────────────────────────── */

int control_submit_result(const char *host,
                          const TestResultSubmission *r, int debug,
                          SubmitStatus *st)
{
    memset(st, 0, sizeof(*st));
    char base_buf[256];
    strncpy(base_buf, host, sizeof(base_buf) - 1);
    size_t l = strlen(base_buf);
    while (l > 0 && base_buf[l-1] == '/') base_buf[--l] = '\0';

    char url[512];
    snprintf(url, sizeof(url), "%s/RMBTControlServer/result", base_buf);

    /* Build JSON body dynamically. */
    size_t bsz = 65536 + (size_t)r->num_pings * 128
                       + (size_t)r->num_speed_detail * 128;
    char *body = malloc(bsz);
    if (!body) return -1;

    /* client_software_version carries this client's own version and is normally
     * always set; emit JSON null only in the defensive empty case. */
    char csv_frag[80];
    if (r->client_software_version[0])
        snprintf(csv_frag, sizeof(csv_frag), "\"%s\"", r->client_software_version);
    else
        snprintf(csv_frag, sizeof(csv_frag), "null");

    /* device: emitted only when set (e.g. from --set-version as "App: <ver>"). */
    char device_frag[80];
    if (r->device[0])
        snprintf(device_frag, sizeof(device_frag), "\"device\":\"%s\",", r->device);
    else
        device_frag[0] = '\0';

    size_t pos = 0;
    appendf(body, bsz, &pos,
        "{"
        "\"client_language\":\"%s\","
        "\"client_name\":\"%s\","
        "\"client_uuid\":\"%s\","
        "\"client_version\":\"%s\","
        "\"client_software_version\":%s,"
        "\"geoLocations\":[],"
        "\"model\":\"%s\","
        "%s"
        "\"network_type\":%u,"
        "\"platform\":\"%s\","
        "\"product\":\"%s\","
        "\"test_bytes_download\":%llu,"
        "\"test_bytes_upload\":%llu,"
        "\"test_nsec_download\":%llu,"
        "\"test_nsec_upload\":%llu,"
        "\"test_num_threads\":%d,"
        "\"num_threads_ul\":%d,"
        "\"test_ping_shortest\":%llu,"
        "\"test_speed_download\":%llu,"
        "\"test_speed_upload\":%llu,"
        "\"test_token\":\"%s\","
        "\"test_uuid\":\"%s\","
        "\"time\":%llu,"
        "\"timezone\":\"%s\","
        "\"type\":\"%s\","
        "\"version_code\":\"%s\","
        "\"user_server_selection\":%s,"
        "\"test_status\":\"%s\","
        "\"test_port_remote\":%u,",
        r->client_language,
        r->client_name,
        r->client_uuid,
        r->client_version,
        csv_frag,
        r->model,
        device_frag,
        r->network_type,
        r->platform,
        r->product,
        (unsigned long long)r->test_bytes_download,
        (unsigned long long)r->test_bytes_upload,
        (unsigned long long)r->test_nsec_download,
        (unsigned long long)r->test_nsec_upload,
        r->test_num_threads,
        r->num_threads_ul,
        (unsigned long long)r->test_ping_shortest,
        (unsigned long long)r->test_speed_download,
        (unsigned long long)r->test_speed_upload,
        r->test_token,
        r->test_uuid,
        (unsigned long long)r->time_ms,
        r->timezone,
        r->client_type,
        r->version_code,
        r->user_server_selection ? "true" : "false",
        r->test_status,
        r->test_port_remote);

    /* pings array */
    appendf(body, bsz, &pos, "\"pings\":[");
    for (int i = 0; i < r->num_pings; i++) {
        appendf(body, bsz, &pos,
            "%s{\"value\":%llu,\"value_server\":%llu,\"time_ns\":%llu}",
            i ? "," : "",
            (unsigned long long)r->pings[i].value,
            (unsigned long long)r->pings[i].value_server,
            (unsigned long long)r->pings[i].time_ns);
    }
    appendf(body, bsz, &pos, "],");

    /* speed_detail array */
    appendf(body, bsz, &pos, "\"speed_detail\":[");
    for (int i = 0; i < r->num_speed_detail; i++) {
        appendf(body, bsz, &pos,
            "%s{\"direction\":\"%s\",\"thread\":%d,\"time\":%llu,\"bytes\":%llu}",
            i ? "," : "",
            r->speed_detail[i].direction,
            r->speed_detail[i].thread,
            (unsigned long long)r->speed_detail[i].time,
            (unsigned long long)r->speed_detail[i].bytes);
    }
    appendf(body, bsz, &pos, "]}");

    CurlBuf resp = {NULL, 0};
    int rc = do_post(url, body, debug, &resp, &st->http_status);
    free(body);

    if (rc < 0) {
        /* do_post reported the cause (curl error or HTTP status) via gui_report_error. */
        const char *e = gui_last_error();
        snprintf(st->error, sizeof(st->error), "%s", (e && *e) ? e : "result submission failed");
        fprintf(stderr, "Warning: result submission failed\n");
    } else if (json_has_errors(resp.data)) {
        const char *ep = strstr(resp.data, "\"error\"");
        snprintf(st->error, sizeof(st->error), "%s", ep ? ep : resp.data);
        fprintf(stderr, "Warning: result rejected by control server: %s\n", st->error);
    } else {
        st->success = 1;
    }
    if (resp.data) free(resp.data);
    return 0;
}
