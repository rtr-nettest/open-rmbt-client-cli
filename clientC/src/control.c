#include "control.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <curl/curl.h>

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
                   int debug, CurlBuf *resp)
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
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        fprintf(stderr, "curl error: %s\n", curl_easy_strerror(rc));
        free(resp->data); resp->data = NULL;
        return -1;
    }
    if (http_code >= 400) {
        fprintf(stderr, "HTTP %ld: %s\n", http_code, resp->data);
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
            "\"softwareRevision\":\"" GIT_VERSION "\","
            "\"softwareVersionName\":\"" GIT_VERSION "\","
            "\"terms_and_conditions_accepted\":true"
            "}", uuid_in);
    } else {
        snprintf(body, sizeof(body),
            "{"
            "\"name\":\"RMBT\","
            "\"type\":\"DESKTOP\","
            "\"language\":\"en\","
            "\"timezone\":\"UTC\","
            "\"softwareRevision\":\"" GIT_VERSION "\","
            "\"softwareVersionName\":\"" GIT_VERSION "\","
            "\"terms_and_conditions_accepted\":true"
            "}");
    }

    CurlBuf resp = {NULL, 0};
    if (do_post(url, body, debug, &resp) < 0) return -1;

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
        fprintf(stderr, "settings response contained no UUID\n");
        return -1;
    }
    return 0;
}

/* ── Public: request test ────────────────────────────────────────────────────── */

int control_request_test(const char *host, const char *uuid,
                         int use_ws, int debug, TestParams *out)
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

    char body[1024];
    if (uuid && *uuid) {
        snprintf(body, sizeof(body),
            "{"
            "\"uuid\":\"%s\","
            "\"client\":\"%s\","
            "\"version\":\"0.9\","
            "\"type\":\"DESKTOP\","
            "\"softwareVersion\":\"" GIT_VERSION "\","
            "\"softwareRevision\":\"" GIT_VERSION "\","
            "\"language\":\"en\","
            "\"timezone\":\"UTC\","
            "\"time\":%llu"
            "%s"
            "}",
            uuid, client_id, (unsigned long long)ts,
            use_ws ? "" : ",\"capabilities\":{\"RMBThttp\":true}");
    } else {
        snprintf(body, sizeof(body),
            "{"
            "\"client\":\"%s\","
            "\"version\":\"0.9\","
            "\"type\":\"DESKTOP\","
            "\"softwareVersion\":\"" GIT_VERSION "\","
            "\"softwareRevision\":\"" GIT_VERSION "\","
            "\"language\":\"en\","
            "\"timezone\":\"UTC\","
            "\"time\":%llu"
            "%s"
            "}",
            client_id, (unsigned long long)ts,
            use_ws ? "" : ",\"capabilities\":{\"RMBThttp\":true}");
    }

    CurlBuf resp = {NULL, 0};
    if (do_post(url, body, debug, &resp) < 0) return -1;

    if (json_has_errors(resp.data)) {
        const char *ep = strstr(resp.data, "\"error\"");
        fprintf(stderr, "Control server error: %s\n", ep ? ep : resp.data);
        free(resp.data);
        return -1;
    }

    memset(out, 0, sizeof(*out));

    json_get_str(resp.data, "test_token",          out->token,          sizeof(out->token));
    json_get_str(resp.data, "test_uuid",           out->test_uuid,      sizeof(out->test_uuid));
    json_get_str(resp.data, "open_test_uuid",      out->open_test_uuid, sizeof(out->open_test_uuid));
    json_get_str(resp.data, "test_server_address", out->server_addr,    sizeof(out->server_addr));
    json_get_str(resp.data, "test_server_type",    out->server_type,    sizeof(out->server_type));

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
        fprintf(stderr, "Control server: missing test_token\n");
        return -1;
    }
    if (!out->server_addr[0]) {
        fprintf(stderr, "Control server: missing test_server_address\n");
        return -1;
    }
    return 0;
}

/* ── Public: submit result ───────────────────────────────────────────────────── */

int control_submit_result(const char *host,
                          const TestResultSubmission *r, int debug)
{
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

    int pos = 0;
    pos += snprintf(body + pos, bsz - pos,
        "{"
        "\"client_language\":\"%s\","
        "\"client_name\":\"%s\","
        "\"client_uuid\":\"%s\","
        "\"client_version\":\"%s\","
        "\"client_software_version\":\"%s\","
        "\"geoLocations\":[],"
        "\"model\":\"%s\","
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
        r->client_software_version,
        r->model,
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
    pos += snprintf(body + pos, bsz - pos, "\"pings\":[");
    for (int i = 0; i < r->num_pings; i++) {
        pos += snprintf(body + pos, bsz - pos,
            "%s{\"value\":%llu,\"value_server\":%llu,\"time_ns\":%llu}",
            i ? "," : "",
            (unsigned long long)r->pings[i].value,
            (unsigned long long)r->pings[i].value_server,
            (unsigned long long)r->pings[i].time_ns);
    }
    pos += snprintf(body + pos, bsz - pos, "],");

    /* speed_detail array */
    pos += snprintf(body + pos, bsz - pos, "\"speed_detail\":[");
    for (int i = 0; i < r->num_speed_detail; i++) {
        pos += snprintf(body + pos, bsz - pos,
            "%s{\"direction\":\"%s\",\"thread\":%d,\"time\":%llu,\"bytes\":%llu}",
            i ? "," : "",
            r->speed_detail[i].direction,
            r->speed_detail[i].thread,
            (unsigned long long)r->speed_detail[i].time,
            (unsigned long long)r->speed_detail[i].bytes);
    }
    pos += snprintf(body + pos, bsz - pos, "]}");

    CurlBuf resp = {NULL, 0};
    int rc = do_post(url, body, debug, &resp);
    free(body);
    if (resp.data) free(resp.data);

    if (rc < 0) {
        fprintf(stderr, "Warning: result submission failed\n");
        return -1;   /* non-fatal: caller ignores this */
    }
    return 0;
}
