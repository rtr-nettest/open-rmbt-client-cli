#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    char     token[512];
    char     test_uuid[128];
    char     open_test_uuid[128];
    char     server_addr[256];
    uint16_t server_port;
    int      encryption;
    uint32_t duration;
    uint32_t num_threads;
    uint32_t wait;
    char     server_type[64];
} TestParams;

typedef struct {
    uint64_t value;
    uint64_t value_server;
    uint64_t time_ns;
} PingItem;

typedef struct {
    char     direction[16];   /* "download" or "upload" */
    int      thread;
    uint64_t time;
    uint64_t bytes;
} SpeedItem;

typedef struct {
    char        client_language[8];
    char        client_name[32];
    char        client_uuid[64];
    char        client_version[32];
    char        client_software_version[32];
    char        model[32];
    uint32_t    network_type;
    char        platform[16];
    char        product[32];
    PingItem   *pings;
    int         num_pings;
    uint64_t    test_bytes_download;
    uint64_t    test_bytes_upload;
    uint64_t    test_nsec_download;
    uint64_t    test_nsec_upload;
    int         test_num_threads;
    int         num_threads_ul;
    uint64_t    test_ping_shortest;
    uint64_t    test_speed_download;
    uint64_t    test_speed_upload;
    char        test_token[512];
    char        test_uuid[128];
    uint64_t    time_ms;
    char        timezone[32];
    char        client_type[16];
    char        version_code[8];
    SpeedItem  *speed_detail;
    int         num_speed_detail;
    int         user_server_selection;
    char        test_status[4];
    uint16_t    test_port_remote;
} TestResultSubmission;

/*
 * POST /settings to register or re-identify the client.
 * uuid_in: previously stored UUID, or NULL on first run.
 * uuid_out: buffer (>=64 bytes) filled with the server-assigned UUID.
 * Returns 0 on success.
 */
int control_request_settings(const char *host, const char *uuid_in,
                              int debug, char *uuid_out, size_t uuid_out_len);

/* Returns 0 on success. */
int control_request_test(const char *host, const char *uuid,
                         int use_ws, int debug, TestParams *out);

int control_submit_result(const char *host,
                          const TestResultSubmission *r, int debug);
