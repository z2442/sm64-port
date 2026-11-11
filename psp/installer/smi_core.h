/* Portable core of the on-device installer: it rebuilds SM64.PRX from the user's own ROM(s), the
 * ROM-free template (SM64.template.prx) and the manifest (manifest.bin) produced by
 * tools/psp/make_release.py. It only needs stdio, so the same code is tested on a PC and runs on
 * the PSP. The manifest format is documented at the top of tools/psp/make_release.py. */
#ifndef SMI_CORE_H
#define SMI_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define SMI_MAX_ROMS 2
#define SMI_ROM_SIZE 0x800000u /* every supported ROM is exactly 8 MiB */

enum smi_status {
    SMI_OK = 0,
    SMI_E_IO = -1,        /* cannot read or write a file (also: no space left) */
    SMI_E_MANIFEST = -2,  /* manifest missing, damaged or of an unknown format */
    SMI_E_MEMORY = -3,
    SMI_E_NOT_ROM = -4,   /* the file is not a Nintendo 64 ROM */
    SMI_E_WRONG_ROM = -5, /* a valid ROM, but not one this package needs */
    SMI_E_TEMPLATE = -6,  /* template missing or different from the one in the manifest */
    SMI_E_DECODE = -7,    /* damaged ROM data */
    SMI_E_VERIFY = -8,    /* the rebuilt module does not have the expected checksum */
    SMI_E_ABORT = -9      /* the progress callback asked to stop */
};

enum smi_stage { SMI_STAGE_HASH_ROM = 0, SMI_STAGE_COPY_TEMPLATE, SMI_STAGE_BUILD, SMI_STAGE_VERIFY };

/* Memory and progress hooks. 'alloc' must return NULL on failure; 'progress' may be NULL and
 * returns non-zero to abort. */
typedef struct smi_env {
    void *(*alloc)(size_t size);
    void (*release)(void *ptr);
    int (*progress)(void *user, int stage, uint32_t done, uint32_t total);
    void *user;
} smi_env;

typedef struct smi_entry {
    uint32_t prx_off, len, mio0_off, src_off, map_off;
    uint8_t rom_id, kind;
} smi_entry;

typedef struct smi_manifest {
    uint32_t nroms, nents, nmaps, tsize;
    char lang[SMI_MAX_ROMS][5];
    uint8_t rom_sha[SMI_MAX_ROMS][20];
    uint8_t tpl_sha[20], out_sha[20];
    smi_entry *ents;
    uint8_t *blob; /* whole manifest file; the swap maps live inside it */
    const uint8_t *maps;
} smi_manifest;

/* Load manifest.bin. */
int smi_load_manifest(const smi_env *env, const char *path, smi_manifest *m);
void smi_free_manifest(const smi_env *env, smi_manifest *m);

/* Identify a ROM file (any byte order). Returns the index of the ROM it matches in the manifest
 * (>= 0), SMI_E_NOT_ROM if it is not an N64 ROM, SMI_E_WRONG_ROM if it matches none, or another
 * negative status. */
int smi_identify_rom(const smi_env *env, const smi_manifest *m, const char *path);

/* Build the module into 'out_path' (it is overwritten). rom_paths[i] is the ROM for manifest ROM i. */
int smi_install(const smi_env *env, const smi_manifest *m, const char *template_path, const char *const rom_paths[SMI_MAX_ROMS],
                const char *out_path);

/* A small file next to the module records which build was installed, so that the installer
 * does nothing on later starts and re-installs when the package is updated. */
int smi_stamp_matches(const char *stamp_path, const smi_manifest *m);
int smi_stamp_write(const char *stamp_path, const smi_manifest *m);

/* Size of a file, or -1. */
long smi_file_size(const char *path);

#endif
