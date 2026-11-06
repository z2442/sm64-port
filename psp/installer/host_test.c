/* PC test driver for smi_core.c:  host_test DIR OUT ROM [ROM]
 * DIR has manifest.bin and SM64.template.prx; the ROMs may be given in any order and byte order. */
#include "smi_core.h"

#include <stdlib.h>
#include <string.h>

static void *a(size_t n) { return malloc(n); }
static void r(void *p) { free(p); }
static int prog(void *u, int stage, uint32_t d, uint32_t t) {
    (void) u;
    (void) stage;
    (void) d;
    (void) t;
    return 0;
}

int main(int argc, char **argv) {
    smi_env env = { a, r, prog, NULL };
    smi_manifest m;
    char mp[512], tp[512];
    const char *roms[SMI_MAX_ROMS] = { NULL, NULL };
    int i, rc;
    if (argc < 4) {
        fprintf(stderr, "usage: host_test DIR OUT ROM [ROM]\n");
        return 2;
    }
    snprintf(mp, sizeof(mp), "%s/manifest.bin", argv[1]);
    snprintf(tp, sizeof(tp), "%s/SM64.template.prx", argv[1]);
    rc = smi_load_manifest(&env, mp, &m);
    if (rc != SMI_OK) {
        printf("manifest error %d\n", rc);
        return 1;
    }
    for (i = 3; i < argc; i++) {
        rc = smi_identify_rom(&env, &m, argv[i]);
        printf("%s -> %d\n", argv[i], rc);
        if (rc >= 0) {
            roms[rc] = argv[i];
        }
    }
    for (i = 0; i < (int) m.nroms; i++) {
        if (roms[i] == NULL) {
            printf("missing ROM '%s'\n", m.lang[i]);
            smi_free_manifest(&env, &m);
            return 3;
        }
    }
    rc = smi_install(&env, &m, tp, roms, argv[2]);
    printf("install -> %d\n", rc);
    smi_free_manifest(&env, &m);
    return rc == SMI_OK ? 0 : 1;
}
