#pragma once
#include <stddef.h>

/* Load UUID from ~/.rmbt_client_uuid into buf.  Returns 0 on success, -1 if not found. */
int uuid_load(char *buf, size_t buflen);

/* Save a server-assigned UUID to ~/.rmbt_client_uuid. */
int uuid_save(const char *uuid);
