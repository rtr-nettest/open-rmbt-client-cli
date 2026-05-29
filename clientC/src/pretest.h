#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    size_t chunk_size;
    int    dl_threads;
    int    ul_threads;
} PretestResult;

int run_pretest(const char *addr, uint16_t port,
                int use_tls, int no_tls_verify, int protocol,
                const char *token, int max_threads,
                PretestResult *out);
