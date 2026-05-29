#include "uuid_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void uuid_file_path(char *out, size_t len)
{
    const char *home = getenv("HOME");
    if (!home) home = getenv("USERPROFILE");
    if (home)
        snprintf(out, len, "%s/.rmbt_client_uuid", home);
    else
        snprintf(out, len, ".rmbt_client_uuid");
}

int uuid_load(char *buf, size_t buflen)
{
    char path[512];
    uuid_file_path(path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char tmp[64] = "";
    int ok = -1;
    if (fgets(tmp, sizeof(tmp), f)) {
        size_t l = strlen(tmp);
        while (l > 0 && (tmp[l-1] == '\n' || tmp[l-1] == '\r' || tmp[l-1] == ' '))
            tmp[--l] = '\0';
        if (l >= 36) {
            snprintf(buf, buflen, "%s", tmp);
            ok = 0;
        }
    }
    fclose(f);
    return ok;
}

int uuid_save(const char *uuid)
{
    char path[512];
    uuid_file_path(path, sizeof(path));

    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "Warning: could not save UUID to %s\n", path);
        return -1;
    }
    fprintf(f, "%s\n", uuid);
    fclose(f);
    printf("Client UUID saved: %s\n  (%s)\n\n", uuid, path);
    return 0;
}
