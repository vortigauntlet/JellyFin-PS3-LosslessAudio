// Remembered version per title -- see vremember.h.

#include "vremember.h"
#include "jf_paths.h"
#include <stdio.h>
#include <string.h>

#define VR_FILE  "jellyfin_versions.txt"
#define VR_MAX   96
#define VR_ID    100          // MediaSourceIds are 32 hex; room to spare

const char *vremember_get(const char *item_id)
{
    static char out[VR_ID];
    if (!item_id || !item_id[0]) return NULL;
    FILE *f = fopen(jf_data_path(VR_FILE), "r");
    if (!f) return NULL;
    char id[VR_ID], src[VR_ID];
    const char *found = NULL;
    while (fscanf(f, "%99s %99s", id, src) == 2) {
        if (strcmp(id, item_id) == 0) {
            snprintf(out, sizeof out, "%s", src);
            found = out;
            break;
        }
    }
    fclose(f);
    return found;
}

void vremember_put(const char *item_id, const char *source_id)
{
    if (!item_id || !item_id[0] || !source_id || !source_id[0]) return;
    if (strlen(item_id) >= VR_ID || strlen(source_id) >= VR_ID) return;
    {   // unchanged: no write
        const char *have = vremember_get(item_id);
        if (have && strcmp(have, source_id) == 0) return;
    }
    static char ids[VR_MAX][VR_ID], srcs[VR_MAX][VR_ID];
    int n = 0;
    snprintf(ids[n], VR_ID, "%s", item_id);
    snprintf(srcs[n], VR_ID, "%s", source_id);
    n++;
    FILE *f = fopen(jf_data_path(VR_FILE), "r");
    if (f) {
        char id[VR_ID], src[VR_ID];
        while (n < VR_MAX && fscanf(f, "%99s %99s", id, src) == 2) {
            if (strcmp(id, item_id) == 0) continue;
            snprintf(ids[n], VR_ID, "%s", id);
            snprintf(srcs[n], VR_ID, "%s", src);
            n++;
        }
        fclose(f);
    }
    f = fopen(jf_data_path(VR_FILE), "w");
    if (!f) return;
    for (int i = 0; i < n; i++) fprintf(f, "%s %s\n", ids[i], srcs[i]);
    fclose(f);
}
