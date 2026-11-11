#include "smi_core.h"

#include <stdlib.h>
#include <string.h>

#define NOMAP 0xFFFFFFFFu
#define CHUNK 65536u
#define HEADER_SIZE 28u
#define ROMREC_SIZE 24u
#define ENTRY_SIZE 24u

/* ------------------------------------------------------------------------------------------ */
/* SHA-1                                                                                       */
/* ------------------------------------------------------------------------------------------ */

typedef struct {
    uint32_t h[5];
    uint64_t total;
    uint8_t buf[64];
    uint32_t used;
} sha1_ctx;

static uint32_t rol(uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

static void sha1_block(sha1_ctx *c, const uint8_t *p) {
    uint32_t w[80], a, b, d, e, f, k, t, cc;
    int i;
    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t) p[i * 4] << 24) | ((uint32_t) p[i * 4 + 1] << 16) | ((uint32_t) p[i * 4 + 2] << 8) | p[i * 4 + 3];
    }
    for (i = 16; i < 80; i++) {
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    a = c->h[0];
    b = c->h[1];
    cc = c->h[2];
    d = c->h[3];
    e = c->h[4];
    for (i = 0; i < 80; i++) {
        if (i < 20) {
            f = (b & cc) | (~b & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ cc ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & cc) | (b & d) | (cc & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ cc ^ d;
            k = 0xCA62C1D6u;
        }
        t = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = cc;
        cc = rol(b, 30);
        b = a;
        a = t;
    }
    c->h[0] += a;
    c->h[1] += b;
    c->h[2] += cc;
    c->h[3] += d;
    c->h[4] += e;
}

static void sha1_init(sha1_ctx *c) {
    c->h[0] = 0x67452301u;
    c->h[1] = 0xEFCDAB89u;
    c->h[2] = 0x98BADCFEu;
    c->h[3] = 0x10325476u;
    c->h[4] = 0xC3D2E1F0u;
    c->total = 0;
    c->used = 0;
}

static void sha1_update(sha1_ctx *c, const uint8_t *data, size_t len) {
    c->total += len;
    while (len > 0) {
        size_t n = 64 - c->used;
        if (n > len) {
            n = len;
        }
        memcpy(c->buf + c->used, data, n);
        c->used += (uint32_t) n;
        data += n;
        len -= n;
        if (c->used == 64) {
            sha1_block(c, c->buf);
            c->used = 0;
        }
    }
}

static void sha1_final(sha1_ctx *c, uint8_t out[20]) {
    uint64_t bits = c->total * 8;
    uint8_t pad = 0x80, zero = 0, len[8];
    int i;
    sha1_update(c, &pad, 1);
    while (c->used != 56) {
        sha1_update(c, &zero, 1);
    }
    for (i = 0; i < 8; i++) {
        len[i] = (uint8_t) (bits >> (56 - 8 * i));
    }
    sha1_update(c, len, 8);
    for (i = 0; i < 5; i++) {
        out[i * 4] = (uint8_t) (c->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t) (c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t) (c->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t) c->h[i];
    }
}

/* ------------------------------------------------------------------------------------------ */
/* Helpers                                                                                     */
/* ------------------------------------------------------------------------------------------ */

static uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static uint32_t rd_be32(const uint8_t *p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
}

static int progress(const smi_env *env, int stage, uint32_t done, uint32_t total) {
    if (env->progress != NULL && env->progress(env->user, stage, done, total) != 0) {
        return SMI_E_ABORT;
    }
    return SMI_OK;
}

/* Minimal file layer. On the PSP it uses the kernel I/O calls directly (no newlib stdio: it needs heap memory and the loader keeps a tiny
 * heap on purpose); on a PC build it is a thin wrapper over stdio, so the same installer code is tested on both. */
#ifdef SMI_PSP_IO
#include <pspiofilemgr.h>
typedef struct {
    SceUID fd;
    int used;
} smi_f;
static smi_f g_files[8];

static smi_f *sf_open(const char *path, int write) {
    int i;
    for (i = 0; i < 8; i++) {
        if (!g_files[i].used) {
            SceUID fd = sceIoOpen(path, write ? (PSP_O_RDWR | PSP_O_CREAT | PSP_O_TRUNC) : PSP_O_RDONLY, 0777);
            if (fd < 0) {
                return NULL;
            }
            g_files[i].fd = fd;
            g_files[i].used = 1;
            return &g_files[i];
        }
    }
    return NULL;
}
static size_t sf_read(smi_f *f, void *b, size_t n) {
    int r = sceIoRead(f->fd, b, (SceSize) n);
    return r < 0 ? 0 : (size_t) r;
}
static size_t sf_write(smi_f *f, const void *b, size_t n) {
    int r = sceIoWrite(f->fd, b, (SceSize) n);
    return r < 0 ? 0 : (size_t) r;
}
static int sf_seek(smi_f *f, long off, int whence) {
    SceOff r = sceIoLseek(f->fd, off, whence == SEEK_END ? PSP_SEEK_END : PSP_SEEK_SET);
    return r < 0 ? -1 : 0;
}
static long sf_tell(smi_f *f) {
    SceOff r = sceIoLseek(f->fd, 0, PSP_SEEK_CUR);
    return r < 0 ? -1 : (long) r;
}
static int sf_close(smi_f *f) {
    int r = sceIoClose(f->fd);
    f->used = 0;
    return r < 0 ? -1 : 0;
}
#else
typedef FILE smi_f;
static smi_f *sf_open(const char *path, int write) {
    smi_f *f = fopen(path, write ? "w+b" : "rb");
    if (f != NULL) {
        setvbuf(f, NULL, _IONBF, 0); /* all reads and writes below are already in large blocks */
    }
    return f;
}
static size_t sf_read(smi_f *f, void *b, size_t n) { return fread(b, 1, n, f); }
static size_t sf_write(smi_f *f, const void *b, size_t n) { return fwrite(b, 1, n, f); }
static int sf_seek(smi_f *f, long off, int whence) { return fseek(f, off, whence); }
static long sf_tell(smi_f *f) { return ftell(f); }
static int sf_close(smi_f *f) { return fclose(f); }
#endif

long smi_file_size(const char *path) {
    long size;
    smi_f *f = sf_open(path, 0);
    if (f == NULL) {
        return -1;
    }
    if (sf_seek(f, 0, SEEK_END) != 0) {
        sf_close(f);
        return -1;
    }
    size = sf_tell(f);
    sf_close(f);
    return size;
}

/* ------------------------------------------------------------------------------------------ */
/* Manifest                                                                                    */
/* ------------------------------------------------------------------------------------------ */

int smi_load_manifest(const smi_env *env, const char *path, smi_manifest *m) {
    long size;
    smi_f *f;
    uint8_t *b;
    uint32_t i, pos, nroms, nents, nmaps;

    memset(m, 0, sizeof(*m));
    size = smi_file_size(path);
    if (size < (long) HEADER_SIZE || size > 4 * 1024 * 1024) {
        return SMI_E_MANIFEST;
    }
    b = (uint8_t *) env->alloc((size_t) size);
    if (b == NULL) {
        return SMI_E_MEMORY;
    }
    f = sf_open(path, 0);
    if (f == NULL || sf_read(f, b, (size_t) size) != (size_t) size) {
        if (f != NULL) {
            sf_close(f);
        }
        env->release(b);
        return SMI_E_IO;
    }
    sf_close(f);

    nroms = rd_le32(b + 8);
    nents = rd_le32(b + 12);
    nmaps = rd_le32(b + 16);
    if (memcmp(b, "SMAN", 4) != 0 || rd_le32(b + 4) != 1 || nroms == 0 || nroms > SMI_MAX_ROMS ||
        (uint64_t) HEADER_SIZE + (uint64_t) nroms * ROMREC_SIZE + 40 + (uint64_t) nents * ENTRY_SIZE + nmaps != (uint64_t) size) {
        env->release(b);
        return SMI_E_MANIFEST;
    }
    m->nroms = nroms;
    m->nents = nents;
    m->nmaps = nmaps;
    m->tsize = rd_le32(b + 20);
    pos = HEADER_SIZE;
    for (i = 0; i < nroms; i++) {
        memcpy(m->lang[i], b + pos, 4);
        m->lang[i][4] = 0;
        memcpy(m->rom_sha[i], b + pos + 4, 20);
        pos += ROMREC_SIZE;
    }
    memcpy(m->tpl_sha, b + pos, 20);
    memcpy(m->out_sha, b + pos + 20, 20);
    pos += 40;
    m->ents = (smi_entry *) env->alloc(sizeof(smi_entry) * (nents ? nents : 1));
    if (m->ents == NULL) {
        env->release(b);
        return SMI_E_MEMORY;
    }
    for (i = 0; i < nents; i++) {
        const uint8_t *e = b + pos + i * ENTRY_SIZE;
        m->ents[i].prx_off = rd_le32(e);
        m->ents[i].len = rd_le32(e + 4);
        m->ents[i].rom_id = e[8];
        m->ents[i].kind = e[9];
        m->ents[i].mio0_off = rd_le32(e + 12);
        m->ents[i].src_off = rd_le32(e + 16);
        m->ents[i].map_off = rd_le32(e + 20);
        if (m->ents[i].rom_id >= nroms || m->ents[i].kind > 1 || (uint64_t) m->ents[i].prx_off + m->ents[i].len > m->tsize ||
            (m->ents[i].map_off != NOMAP && ((m->ents[i].len & 3) != 0 || (uint64_t) m->ents[i].map_off + (m->ents[i].len / 4 + 3) / 4 > nmaps))) {
            env->release(m->ents);
            env->release(b);
            memset(m, 0, sizeof(*m));
            return SMI_E_MANIFEST;
        }
    }
    m->blob = b;
    m->maps = b + pos + (size_t) nents * ENTRY_SIZE;
    return SMI_OK;
}

void smi_free_manifest(const smi_env *env, smi_manifest *m) {
    if (m->ents != NULL) {
        env->release(m->ents);
    }
    if (m->blob != NULL) {
        env->release(m->blob);
    }
    memset(m, 0, sizeof(*m));
}

/* ------------------------------------------------------------------------------------------ */
/* ROM access: .z64 (big endian), .v64 (16-bit swapped) and .n64 (32-bit reversed)             */
/* ------------------------------------------------------------------------------------------ */

typedef struct {
    smi_f *f;
    uint32_t size;
    int order; /* 0 = z64, 1 = v64, 2 = n64 */
    uint8_t *scratch;
} smi_rom;

static void convert_order(int order, uint8_t *p, size_t n) {
    size_t i;
    uint8_t t;
    if (order == 1) {
        for (i = 0; i + 1 < n; i += 2) {
            t = p[i];
            p[i] = p[i + 1];
            p[i + 1] = t;
        }
    } else if (order == 2) {
        for (i = 0; i + 3 < n; i += 4) {
            t = p[i];
            p[i] = p[i + 3];
            p[i + 3] = t;
            t = p[i + 1];
            p[i + 1] = p[i + 2];
            p[i + 2] = t;
        }
    }
}

static int rom_open(const smi_env *env, smi_rom *r, const char *path) {
    uint8_t magic[4];
    long size;
    memset(r, 0, sizeof(*r));
    r->f = sf_open(path, 0);
    if (r->f == NULL) {
        return SMI_E_IO;
    }
    if (sf_seek(r->f, 0, SEEK_END) != 0 || (size = sf_tell(r->f)) < 0x1000 || (size & 3) != 0 || sf_seek(r->f, 0, SEEK_SET) != 0 ||
        sf_read(r->f, magic, 4) != 4) {
        sf_close(r->f);
        r->f = NULL;
        return SMI_E_NOT_ROM;
    }
    if (magic[0] == 0x80 && magic[1] == 0x37 && magic[2] == 0x12 && magic[3] == 0x40) {
        r->order = 0;
    } else if (magic[0] == 0x37 && magic[1] == 0x80 && magic[2] == 0x40 && magic[3] == 0x12) {
        r->order = 1;
    } else if (magic[0] == 0x40 && magic[1] == 0x12 && magic[2] == 0x37 && magic[3] == 0x80) {
        r->order = 2;
    } else {
        sf_close(r->f);
        r->f = NULL;
        return SMI_E_NOT_ROM;
    }
    r->size = (uint32_t) size;
    r->scratch = (uint8_t *) env->alloc(CHUNK + 8);
    if (r->scratch == NULL) {
        sf_close(r->f);
        r->f = NULL;
        return SMI_E_MEMORY;
    }
    return SMI_OK;
}

static void rom_close(const smi_env *env, smi_rom *r) {
    if (r->f != NULL) {
        sf_close(r->f);
    }
    if (r->scratch != NULL) {
        env->release(r->scratch);
    }
    memset(r, 0, sizeof(*r));
}

/* Read 'len' bytes at ROM offset 'off' as they are in a .z64 file. */
static int rom_read(smi_rom *r, uint32_t off, uint8_t *dst, uint32_t len) {
    if ((uint64_t) off + len > r->size) {
        return SMI_E_DECODE;
    }
    while (len > 0) {
        uint32_t base = off & ~3u, skip = off - base, want = len + skip, n;
        if (want > CHUNK) {
            want = CHUNK;
        }
        want = (want + 3) & ~3u;
        if (base + want > r->size) {
            want = r->size - base;
        }
        if (sf_seek(r->f, (long) base, SEEK_SET) != 0 || sf_read(r->f, r->scratch, want) != want) {
            return SMI_E_IO;
        }
        convert_order(r->order, r->scratch, want);
        n = want - skip;
        if (n > len) {
            n = len;
        }
        memcpy(dst, r->scratch + skip, n);
        dst += n;
        off += n;
        len -= n;
    }
    return SMI_OK;
}

static int rom_hash(const smi_env *env, smi_rom *r, uint8_t out[20]) {
    sha1_ctx c;
    uint8_t *buf = (uint8_t *) env->alloc(CHUNK);
    uint32_t off = 0;
    int rc = SMI_OK;
    if (buf == NULL) {
        return SMI_E_MEMORY;
    }
    sha1_init(&c);
    while (off < r->size) {
        uint32_t n = r->size - off < CHUNK ? r->size - off : CHUNK;
        rc = rom_read(r, off, buf, n);
        if (rc != SMI_OK) {
            break;
        }
        sha1_update(&c, buf, n);
        off += n;
        rc = progress(env, SMI_STAGE_HASH_ROM, off, r->size);
        if (rc != SMI_OK) {
            break;
        }
    }
    env->release(buf);
    if (rc == SMI_OK) {
        sha1_final(&c, out);
    }
    return rc;
}

int smi_identify_rom(const smi_env *env, const smi_manifest *m, const char *path) {
    smi_rom r;
    uint8_t sha[20];
    uint32_t i;
    int rc = rom_open(env, &r, path);
    if (rc != SMI_OK) {
        return rc;
    }
    if (r.size != SMI_ROM_SIZE) {
        rom_close(env, &r);
        return SMI_E_WRONG_ROM;
    }
    rc = rom_hash(env, &r, sha);
    rom_close(env, &r);
    if (rc != SMI_OK) {
        return rc;
    }
    for (i = 0; i < m->nroms; i++) {
        if (memcmp(sha, m->rom_sha[i], 20) == 0) {
            return (int) i;
        }
    }
    return SMI_E_WRONG_ROM;
}

/* ------------------------------------------------------------------------------------------ */
/* MIO0                                                                                        */
/* ------------------------------------------------------------------------------------------ */

static int mio0_decode(const uint8_t *w, uint32_t wl, uint8_t *out, uint32_t n) {
    uint32_t bits_pos = 16, comp_pos, raw_pos, bits = 0, mask = 0, produced = 0;
    if (wl < 16 || memcmp(w, "MIO0", 4) != 0) {
        return SMI_E_DECODE;
    }
    comp_pos = rd_be32(w + 8);
    raw_pos = rd_be32(w + 12);
    while (produced < n) {
        if (mask == 0) {
            if (bits_pos + 4 > wl) {
                return SMI_E_DECODE;
            }
            bits = rd_be32(w + bits_pos);
            bits_pos += 4;
            mask = 0x80000000u;
        }
        if (bits & mask) {
            if (raw_pos >= wl) {
                return SMI_E_DECODE;
            }
            out[produced++] = w[raw_pos++];
        } else {
            uint32_t v, len, dist;
            if (comp_pos + 2 > wl) {
                return SMI_E_DECODE;
            }
            v = ((uint32_t) w[comp_pos] << 8) | w[comp_pos + 1];
            comp_pos += 2;
            len = (v >> 12) + 3;
            dist = (v & 0xFFF) + 1;
            if (dist > produced) {
                return SMI_E_DECODE;
            }
            while (len-- > 0 && produced < n) {
                out[produced] = out[produced - dist];
                produced++;
            }
        }
        mask >>= 1;
    }
    return SMI_OK;
}

/* ------------------------------------------------------------------------------------------ */
/* Install                                                                                     */
/* ------------------------------------------------------------------------------------------ */

static const smi_manifest *g_sort_m; /* qsort has no user pointer; the installer is single threaded */

static int cmp_entries(const void *pa, const void *pb) {
    const smi_entry *a = *(const smi_entry *const *) pa, *b = *(const smi_entry *const *) pb;
    if (a->kind != b->kind) {
        return a->kind < b->kind ? -1 : 1;
    }
    if (a->rom_id != b->rom_id) {
        return a->rom_id < b->rom_id ? -1 : 1;
    }
    if (a->mio0_off != b->mio0_off) {
        return a->mio0_off < b->mio0_off ? -1 : 1;
    }
    if (a->src_off != b->src_off) {
        return a->src_off < b->src_off ? -1 : 1;
    }
    return 0;
}

static void apply_map(uint8_t *p, uint32_t len, const uint8_t *maps, uint32_t map_off) {
    uint32_t w, nwords = len / 4;
    for (w = 0; w < nwords; w++) {
        int code = (maps[map_off + w / 4] >> ((w % 4) * 2)) & 3;
        uint8_t *x = p + w * 4, t;
        if (code == 1) {
            t = x[0];
            x[0] = x[3];
            x[3] = t;
            t = x[1];
            x[1] = x[2];
            x[2] = t;
        } else if (code == 2) {
            t = x[0];
            x[0] = x[1];
            x[1] = t;
            t = x[2];
            x[2] = x[3];
            x[3] = t;
        }
    }
}

static int write_at(smi_f *out, uint32_t off, const uint8_t *data, uint32_t len) {
    if (sf_seek(out, (long) off, SEEK_SET) != 0 || sf_write(out, data, len) != len) {
        return SMI_E_IO;
    }
    return SMI_OK;
}

int smi_install(const smi_env *env, const smi_manifest *m, const char *template_path, const char *const rom_paths[SMI_MAX_ROMS],
                const char *out_path) {
    smi_rom roms[SMI_MAX_ROMS];
    smi_f *tpl = NULL, *out = NULL;
    uint8_t *buf = NULL, *block = NULL, *window = NULL;
    const smi_entry **order = NULL;
    uint32_t i, done = 0, total = 0, block_rom = 0xFFFFFFFFu, block_off = 0, block_len = 0;
    sha1_ctx c;
    uint8_t sha[20];
    int rc = SMI_OK;

    memset(roms, 0, sizeof(roms));
    for (i = 0; i < m->nroms; i++) {
        rc = rom_open(env, &roms[i], rom_paths[i]);
        if (rc != SMI_OK) {
            goto done;
        }
    }
    buf = (uint8_t *) env->alloc(CHUNK);
    order = (const smi_entry **) env->alloc(sizeof(*order) * (m->nents ? m->nents : 1));
    if (buf == NULL || order == NULL) {
        rc = SMI_E_MEMORY;
        goto done;
    }

    /* 1. copy the template, checking that it is the one the manifest was made for */
    if (smi_file_size(template_path) != (long) m->tsize) {
        rc = SMI_E_TEMPLATE;
        goto done;
    }
    tpl = sf_open(template_path, 0);
    out = sf_open(out_path, 1);
    if (tpl == NULL || out == NULL) {
        rc = SMI_E_IO;
        goto done;
    }
    sha1_init(&c);
    for (i = 0; i < m->tsize;) {
        uint32_t n = m->tsize - i < CHUNK ? m->tsize - i : CHUNK;
        if (sf_read(tpl, buf, n) != n) {
            rc = SMI_E_IO;
            goto done;
        }
        sha1_update(&c, buf, n);
        if (sf_write(out, buf, n) != n) {
            rc = SMI_E_IO;
            goto done;
        }
        i += n;
        rc = progress(env, SMI_STAGE_COPY_TEMPLATE, i, m->tsize);
        if (rc != SMI_OK) {
            goto done;
        }
    }
    sha1_final(&c, sha);
    if (memcmp(sha, m->tpl_sha, 20) != 0) {
        rc = SMI_E_TEMPLATE;
        goto done;
    }

    /* 2. fill in the ROM data, decoding each MIO0 block only once */
    for (i = 0; i < m->nents; i++) {
        order[i] = &m->ents[i];
        total += m->ents[i].len;
    }
    g_sort_m = m;
    qsort((void *) order, m->nents, sizeof(*order), cmp_entries);
    for (i = 0; i < m->nents; i++) {
        const smi_entry *e = order[i];
        smi_rom *rom = &roms[e->rom_id];
        if (e->kind == 1) {
            if (block == NULL || block_rom != e->rom_id || block_off != e->mio0_off) {
                uint8_t head[16];
                uint32_t n, wl;
                if (block != NULL) {
                    env->release(block);
                    block = NULL;
                }
                if (window != NULL) {
                    env->release(window);
                    window = NULL;
                }
                rc = rom_read(rom, e->mio0_off, head, 16);
                if (rc != SMI_OK) {
                    goto done;
                }
                if (memcmp(head, "MIO0", 4) != 0) {
                    rc = SMI_E_DECODE;
                    goto done;
                }
                n = rd_be32(head + 4);
                if (n == 0 || n > 8u * 1024 * 1024) {
                    rc = SMI_E_DECODE;
                    goto done;
                }
                wl = n + n / 8 + 4096;
                if (e->mio0_off + wl > rom->size) {
                    wl = rom->size - e->mio0_off;
                }
                window = (uint8_t *) env->alloc(wl);
                block = (uint8_t *) env->alloc(n);
                if (window == NULL || block == NULL) {
                    rc = SMI_E_MEMORY;
                    goto done;
                }
                rc = rom_read(rom, e->mio0_off, window, wl);
                if (rc != SMI_OK) {
                    goto done;
                }
                rc = mio0_decode(window, wl, block, n);
                env->release(window);
                window = NULL;
                if (rc != SMI_OK) {
                    goto done;
                }
                block_rom = e->rom_id;
                block_off = e->mio0_off;
                block_len = n;
            }
            if ((uint64_t) e->src_off + e->len > block_len) {
                rc = SMI_E_DECODE;
                goto done;
            }
            if (e->map_off == NOMAP) {
                rc = write_at(out, e->prx_off, block + e->src_off, e->len);
            } else {
                uint8_t *tmp = (uint8_t *) env->alloc(e->len);
                if (tmp == NULL) {
                    rc = SMI_E_MEMORY;
                    goto done;
                }
                memcpy(tmp, block + e->src_off, e->len);
                apply_map(tmp, e->len, m->maps, e->map_off);
                rc = write_at(out, e->prx_off, tmp, e->len);
                env->release(tmp);
            }
            if (rc != SMI_OK) {
                goto done;
            }
        } else if (e->map_off == NOMAP) {
            uint32_t pos = 0;
            while (pos < e->len) {
                uint32_t n = e->len - pos < CHUNK ? e->len - pos : CHUNK;
                rc = rom_read(rom, e->src_off + pos, buf, n);
                if (rc == SMI_OK) {
                    rc = write_at(out, e->prx_off + pos, buf, n);
                }
                if (rc != SMI_OK) {
                    goto done;
                }
                pos += n;
            }
        } else {
            uint8_t *tmp = (uint8_t *) env->alloc(e->len);
            if (tmp == NULL) {
                rc = SMI_E_MEMORY;
                goto done;
            }
            rc = rom_read(rom, e->src_off, tmp, e->len);
            if (rc == SMI_OK) {
                apply_map(tmp, e->len, m->maps, e->map_off);
                rc = write_at(out, e->prx_off, tmp, e->len);
            }
            env->release(tmp);
            if (rc != SMI_OK) {
                goto done;
            }
        }
        done += e->len;
        rc = progress(env, SMI_STAGE_BUILD, done, total);
        if (rc != SMI_OK) {
            goto done;
        }
    }

    /* 3. check the result */
    if (sf_seek(out, 0, SEEK_SET) != 0) {
        rc = SMI_E_IO;
        goto done;
    }
    sha1_init(&c);
    for (i = 0; i < m->tsize;) {
        uint32_t n = m->tsize - i < CHUNK ? m->tsize - i : CHUNK;
        if (sf_read(out, buf, n) != n) {
            rc = SMI_E_IO;
            goto done;
        }
        sha1_update(&c, buf, n);
        i += n;
        rc = progress(env, SMI_STAGE_VERIFY, i, m->tsize);
        if (rc != SMI_OK) {
            goto done;
        }
    }
    sha1_final(&c, sha);
    if (memcmp(sha, m->out_sha, 20) != 0) {
        rc = SMI_E_VERIFY;
    }

done:
    if (tpl != NULL) {
        sf_close(tpl);
    }
    if (out != NULL && sf_close(out) != 0 && rc == SMI_OK) {
        rc = SMI_E_IO;
    }
    for (i = 0; i < m->nroms; i++) {
        rom_close(env, &roms[i]);
    }
    if (buf != NULL) {
        env->release(buf);
    }
    if (order != NULL) {
        env->release((void *) order);
    }
    if (block != NULL) {
        env->release(block);
    }
    if (window != NULL) {
        env->release(window);
    }
    return rc;
}

/* ------------------------------------------------------------------------------------------ */
/* Stamp                                                                                       */
/* ------------------------------------------------------------------------------------------ */

int smi_stamp_matches(const char *stamp_path, const smi_manifest *m) {
    uint8_t b[24];
    smi_f *f = sf_open(stamp_path, 0);
    int ok;
    if (f == NULL) {
        return 0;
    }
    ok = sf_read(f, b, 24) == 24 && memcmp(b, "SMS1", 4) == 0 && memcmp(b + 4, m->out_sha, 20) == 0;
    sf_close(f);
    return ok;
}

int smi_stamp_write(const char *stamp_path, const smi_manifest *m) {
    uint8_t b[24];
    smi_f *f = sf_open(stamp_path, 1);
    int ok;
    if (f == NULL) {
        return SMI_E_IO;
    }
    memcpy(b, "SMS1", 4);
    memcpy(b + 4, m->out_sha, 20);
    ok = sf_write(f, b, 24) == 24;
    return (sf_close(f) == 0 && ok) ? SMI_OK : SMI_E_IO;
}
