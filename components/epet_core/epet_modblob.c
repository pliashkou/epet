#include "epet_modblob.h"
#include "epet_dynpage.h"
#include "epet_vm.h"
#include "epet_species.h"
#include <stdlib.h>
#include <string.h>

/* Everything is read byte-wise: the payload is packed with no padding, and
 * Xtensa faults on unaligned 16/32-bit loads. */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static float rdf32(const uint8_t *p)
{
    uint32_t v = rd32(p);
    float f;
    memcpy(&f, &v, sizeof f);
    return f;
}

static uint32_t fnv1a(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

const char *epet_blob_result_name(epet_blob_result_t r)
{
    switch (r) {
    case EPET_BLOB_OK:      return "OK";
    case EPET_BLOB_SHORT:   return "SHORT";
    case EPET_BLOB_MAGIC:   return "MAGIC";
    case EPET_BLOB_VERSION: return "VERSION";
    case EPET_BLOB_CRC:     return "CRC";
    case EPET_BLOB_BOUNDS:  return "BOUNDS";
    case EPET_BLOB_MEMORY:  return "MEMORY";
    }
    return "?";
}

/* One heap block holds the module and everything it points at, so freeing is
 * a single free() and there are no dangling sub-allocations. */
typedef struct {
    uint32_t         tag;          /* identifies a loaded module */
    epet_module_t    mod;
    char             id[16];
    char             name[28];

    epet_frame_t    *frames;
    uint8_t         *pixels;
    epet_key_t      *keys;
    epet_pose_t     *poses;
    char           (*pose_names)[12];
    epet_backdrop_t *backdrops;
    epet_palette_t  *bd_pals;
    char           (*bd_names)[12];
    epet_species_t  *species;
    const epet_species_t **species_ptrs;
    char           (*sp_names)[12];
    char           (*sp_blurbs)[28];
    epet_dynpage_t  *pages;
    epet_dynrow_t   *rows;
    epet_page_t    **page_ptrs;

    /* bytecode */
    uint8_t         *code;
    char            *strpool;
    uint16_t        *str_off;
    epet_vm_image_t  image;
    epet_vmpage_t   *vmpages;
    epet_vmhook_t   *hooks;
    uint8_t          n_hooks;
    int32_t         *globals;
    epet_bus_t      *bus;          /* remembered so hooks can be detached */
} loaded_t;

#define LOADED_TAG 0x4C4F4144u   /* "LOAD" */

bool epet_modblob_is_loaded(const epet_module_t *m)
{
    if (!m) return false;
    const loaded_t *l = (const loaded_t *)((const uint8_t *)m - offsetof(loaded_t, mod));
    return l->tag == LOADED_TAG;
}

/* Subscribing on install and unsubscribing on remove is what keeps a loaded
 * module's handler alive exactly as long as the module is. */
static void blob_on_install(const epet_module_t *m, epet_bus_t *bus)
{
    loaded_t *L = (loaded_t *)((uint8_t *)m - offsetof(loaded_t, mod));
    L->bus = bus;
    for (uint8_t i = 0; i < L->n_hooks; i++) {
        epet_vmhook_attach(&L->hooks[i], bus);
    }
}

static void blob_on_remove(const epet_module_t *m)
{
    loaded_t *L = (loaded_t *)((uint8_t *)m - offsetof(loaded_t, mod));
    for (uint8_t i = 0; i < L->n_hooks; i++) {
        epet_vmhook_detach(&L->hooks[i], L->bus);
    }
    L->bus = 0;
}

static void copy_str(char *dst, size_t cap, const uint8_t *src, size_t n)
{
    size_t i = 0;
    for (; i < n && i + 1 < cap && src[i]; i++) dst[i] = (char)src[i];
    dst[i] = 0;
}

bool epet_modblob_peek(const uint8_t *d, size_t len,
                       char *id_out, size_t id_cap,
                       char *name_out, size_t name_cap, uint16_t *version_out)
{
    if (len < EPET_MODBLOB_HEADER) return false;
    if (memcmp(d, EPET_MODBLOB_MAGIC, 5) != 0) return false;
    if (id_out)   copy_str(id_out, id_cap, d + 8, 12);
    if (name_out) copy_str(name_out, name_cap, d + 20, 24);
    if (version_out) *version_out = rd16(d + 44);
    return true;
}

epet_blob_result_t epet_modblob_parse(const uint8_t *d, size_t len,
                                      epet_module_t **out)
{
    if (!d || !out) return EPET_BLOB_SHORT;
    *out = 0;
    if (len < EPET_MODBLOB_HEADER)              return EPET_BLOB_SHORT;
    if (memcmp(d, EPET_MODBLOB_MAGIC, 5) != 0)  return EPET_BLOB_MAGIC;
    if (d[5] != EPET_MODBLOB_VERSION)           return EPET_BLOB_VERSION;

    const uint8_t n_frames = d[46], n_poses = d[47];
    const uint8_t n_bd = d[48], n_species = d[49], n_pages = d[50];
    const uint8_t n_vmpages = d[51];
    const uint8_t n_hooks = d[60];
    const uint32_t paylen = rd32(d + 52);
    const uint32_t want_crc = rd32(d + 56);

    if (len < EPET_MODBLOB_HEADER + (size_t)paylen) return EPET_BLOB_SHORT;
    const uint8_t *pay = d + EPET_MODBLOB_HEADER;
    if (fnv1a(pay, paylen) != want_crc) return EPET_BLOB_CRC;

    /* ---- pass 1: measure, bounds-checking as we go ------------------- */
    size_t off = 0;
    size_t total_px = 0;
    for (uint8_t i = 0; i < n_frames; i++) {
        if (off + 2 > paylen) return EPET_BLOB_BOUNDS;
        size_t n = (size_t)pay[off] * pay[off + 1];
        off += 2;
        if (off + n > paylen) return EPET_BLOB_BOUNDS;
        off += n;
        total_px += n;
    }
    size_t frames_end = off;

    size_t total_keys = 0;
    for (uint8_t i = 0; i < n_poses; i++) {
        if (off + 14 > paylen) return EPET_BLOB_BOUNDS;
        uint8_t nk = pay[off + 13];
        off += 14;
        if (off + (size_t)nk * 3 > paylen) return EPET_BLOB_BOUNDS;
        off += (size_t)nk * 3;
        total_keys += nk;
    }
    size_t poses_end = off;

    if (off + (size_t)n_bd * (12 + 1 + 1 + 2 + 1 + 32) > paylen) return EPET_BLOB_BOUNDS;
    size_t bd_start = off;
    off += (size_t)n_bd * (12 + 1 + 1 + 2 + 1 + 32);

    const size_t SP_SZ = 12 + 28 + 32 + 1 + 16 + 4 + 1;   /* +1: poop frame */
    if (off + (size_t)n_species * SP_SZ > paylen) return EPET_BLOB_BOUNDS;
    size_t sp_start = off;
    off += (size_t)n_species * SP_SZ;

    size_t pg_start = off;
    size_t total_rows = 0;
    for (uint8_t i = 0; i < n_pages; i++) {
        if (off + 14 > paylen) return EPET_BLOB_BOUNDS;
        uint8_t nr = pay[off + 13];
        off += 14;
        if (off + (size_t)nr * 16 > paylen) return EPET_BLOB_BOUNDS;
        off += (size_t)nr * 16;
        total_rows += nr;
    }

    /* code + strings + vmpages, present only in format 2 */
    uint32_t code_len = 0, str_count = 0, str_pool = 0;
    size_t code_start = 0, str_start = 0, vmpg_start = 0;
    if (n_vmpages) {
        if (off + 4 > paylen) return EPET_BLOB_BOUNDS;
        code_len = rd32(pay + off);
        off += 4;
        if (off + code_len > paylen) return EPET_BLOB_BOUNDS;
        code_start = off;
        off += code_len;

        if (off + 4 > paylen) return EPET_BLOB_BOUNDS;
        str_count = rd16(pay + off);
        str_pool  = rd16(pay + off + 2);
        off += 4;
        if (off + str_count * 2 + str_pool > paylen) return EPET_BLOB_BOUNDS;
        str_start = off;
        off += str_count * 2 + str_pool;

        if (off + (size_t)n_vmpages * 30 > paylen) return EPET_BLOB_BOUNDS;
        vmpg_start = off;
        off += (size_t)n_vmpages * 30;
    }

    size_t hook_start = 0;
    if (n_hooks) {
        if (off + (size_t)n_hooks * 8 > paylen) return EPET_BLOB_BOUNDS;
        hook_start = off;
        off += (size_t)n_hooks * 8;
    }

    /* ---- allocate one block ------------------------------------------ */
    #define ALIGN8(x) (((x) + 7u) & ~(size_t)7u)
    size_t need = ALIGN8(sizeof(loaded_t));
    size_t o_frames = need;        need += ALIGN8(n_frames * sizeof(epet_frame_t));
    size_t o_pixels = need;        need += ALIGN8(total_px);
    size_t o_keys   = need;        need += ALIGN8(total_keys * sizeof(epet_key_t));
    size_t o_poses  = need;        need += ALIGN8(n_poses * sizeof(epet_pose_t));
    size_t o_pnames = need;        need += ALIGN8(n_poses * 12);
    size_t o_bds    = need;        need += ALIGN8(n_bd * sizeof(epet_backdrop_t));
    size_t o_bpals  = need;        need += ALIGN8(n_bd * sizeof(epet_palette_t));
    size_t o_bnames = need;        need += ALIGN8(n_bd * 12);
    size_t o_sp     = need;        need += ALIGN8(n_species * sizeof(epet_species_t));
    size_t o_spp    = need;        need += ALIGN8(n_species * sizeof(epet_species_t *));
    size_t o_snames = need;        need += ALIGN8(n_species * 12);
    size_t o_sblurb = need;        need += ALIGN8(n_species * 28);
    size_t o_pages  = need;        need += ALIGN8(n_pages * sizeof(epet_dynpage_t));
    size_t o_rows   = need;        need += ALIGN8(total_rows * sizeof(epet_dynrow_t));
    size_t o_pgp    = need;        need += ALIGN8((n_pages + n_vmpages) * sizeof(epet_page_t *));
    size_t o_code   = need;        need += ALIGN8(code_len);
    size_t o_spool  = need;        need += ALIGN8(str_pool);
    size_t o_soff   = need;        need += ALIGN8(str_count * sizeof(uint16_t));
    size_t o_vmpg   = need;        need += ALIGN8(n_vmpages * sizeof(epet_vmpage_t));
    size_t o_hooks  = need;        need += ALIGN8(n_hooks * sizeof(epet_vmhook_t));
    size_t o_globs  = need;        need += ALIGN8(EPET_VM_GLOBALS * sizeof(int32_t));

    uint8_t *blk = calloc(1, need);
    if (!blk) return EPET_BLOB_MEMORY;

    loaded_t *L = (loaded_t *)blk;
    L->tag          = LOADED_TAG;
    L->frames       = (epet_frame_t *)(blk + o_frames);
    L->pixels       = blk + o_pixels;
    L->keys         = (epet_key_t *)(blk + o_keys);
    L->poses        = (epet_pose_t *)(blk + o_poses);
    L->pose_names   = (char (*)[12])(blk + o_pnames);
    L->backdrops    = (epet_backdrop_t *)(blk + o_bds);
    L->bd_pals      = (epet_palette_t *)(blk + o_bpals);
    L->bd_names     = (char (*)[12])(blk + o_bnames);
    L->species      = (epet_species_t *)(blk + o_sp);
    L->species_ptrs = (const epet_species_t **)(blk + o_spp);
    L->sp_names     = (char (*)[12])(blk + o_snames);
    L->sp_blurbs    = (char (*)[28])(blk + o_sblurb);
    L->pages        = (epet_dynpage_t *)(blk + o_pages);
    L->rows         = (epet_dynrow_t *)(blk + o_rows);
    L->page_ptrs    = (epet_page_t **)(blk + o_pgp);
    L->code         = blk + o_code;
    L->strpool      = (char *)(blk + o_spool);
    L->str_off      = (uint16_t *)(blk + o_soff);
    L->vmpages      = (epet_vmpage_t *)(blk + o_vmpg);
    L->hooks        = (epet_vmhook_t *)(blk + o_hooks);
    L->globals      = (int32_t *)(blk + o_globs);

    /* ---- pass 2: build ------------------------------------------------ */
    off = 0;
    size_t px_at = 0;
    for (uint8_t i = 0; i < n_frames; i++) {
        uint8_t w = pay[off], h = pay[off + 1];
        off += 2;
        size_t n = (size_t)w * h;
        memcpy(L->pixels + px_at, pay + off, n);
        L->frames[i].w = w;
        L->frames[i].h = h;
        L->frames[i].px = L->pixels + px_at;
        px_at += n;
        off += n;
    }
    off = frames_end;

    size_t key_at = 0;
    for (uint8_t i = 0; i < n_poses; i++) {
        copy_str(L->pose_names[i], 12, pay + off, 12);
        uint8_t loop = pay[off + 12], nk = pay[off + 13];
        off += 14;
        L->poses[i].name = L->pose_names[i];
        L->poses[i].keys = L->keys + key_at;
        L->poses[i].n_keys = nk;
        L->poses[i].loop = loop != 0;
        for (uint8_t k = 0; k < nk; k++) {
            uint8_t fi = pay[off];
            if (fi >= n_frames) { free(blk); return EPET_BLOB_BOUNDS; }
            L->keys[key_at].frame = &L->frames[fi];
            L->keys[key_at].hold_ms = rd16(pay + off + 1);
            if (L->keys[key_at].hold_ms == 0) L->keys[key_at].hold_ms = 100;
            key_at++;
            off += 3;
        }
    }
    off = poses_end;

    off = bd_start;
    for (uint8_t i = 0; i < n_bd; i++) {
        copy_str(L->bd_names[i], 12, pay + off, 12);
        uint8_t fi = pay[off + 12], sc = pay[off + 13];
        if (fi >= n_frames) { free(blk); return EPET_BLOB_BOUNDS; }
        L->backdrops[i].name        = L->bd_names[i];
        L->backdrops[i].image       = &L->frames[fi];
        L->backdrops[i].scale       = sc ? sc : 1;
        L->backdrops[i].night       = rd16(pay + off + 14);
        L->backdrops[i].night_alpha = pay[off + 16];
        L->backdrops[i].horizon_y   = 198;
        for (int c = 0; c < 16; c++) {
            L->bd_pals[i].c[c] = rd16(pay + off + 17 + c * 2);
        }
        L->backdrops[i].palette = &L->bd_pals[i];
        off += 12 + 1 + 1 + 2 + 1 + 32;
    }

    off = sp_start;
    for (uint8_t i = 0; i < n_species; i++) {
        copy_str(L->sp_names[i], 12, pay + off, 12);
        copy_str(L->sp_blurbs[i], 28, pay + off + 12, 28);
        for (int c = 0; c < 16; c++) {
            L->species[i].palette.c[c] = rd16(pay + off + 40 + c * 2);
        }
        size_t q = off + 72;
        uint8_t sc = pay[q];
        L->species[i].temper.hunger    = rdf32(pay + q + 1);
        L->species[i].temper.happiness = rdf32(pay + q + 5);
        L->species[i].temper.energy    = rdf32(pay + q + 9);
        L->species[i].temper.hygiene   = rdf32(pay + q + 13);
        uint8_t pose0 = pay[q + 17], npo = pay[q + 18];
        uint8_t bd0   = pay[q + 19], nbd = pay[q + 20];
        uint8_t poopf = pay[q + 21];
        if ((size_t)pose0 + npo > n_poses || (size_t)bd0 + nbd > n_bd) {
            free(blk); return EPET_BLOB_BOUNDS;
        }
        L->species[i].name        = L->sp_names[i];
        L->species[i].blurb       = L->sp_blurbs[i];
        L->species[i].scale       = sc ? sc : 1;
        L->species[i].poses       = L->poses + pose0;
        L->species[i].n_poses     = npo;
        L->species[i].backdrops   = nbd ? L->backdrops + bd0 : 0;
        L->species[i].n_backdrops = nbd;
        L->species[i].poop        = (poopf < n_frames) ? &L->frames[poopf] : 0;
        L->species_ptrs[i] = &L->species[i];
        off += SP_SZ;
    }

    off = pg_start;
    size_t row_at = 0;
    for (uint8_t i = 0; i < n_pages; i++) {
        copy_str(L->pages[i].title, EPET_DYN_TITLE_MAX, pay + off, 12);
        L->pages[i].icon = pay[off + 12];
        uint8_t nr = pay[off + 13];
        off += 14;
        L->pages[i].rows = L->rows + row_at;
        L->pages[i].n_rows = nr;
        for (uint8_t r = 0; r < nr; r++) {
            L->rows[row_at].kind = pay[off] < EPET_ROW_KIND_COUNT ? pay[off] : 0;
            L->rows[row_at].src  = pay[off + 1] < EPET_SRC_COUNT ? pay[off + 1] : 0;
            memcpy(L->rows[row_at].label, pay + off + 2, EPET_ROW_LABEL_MAX);
            row_at++;
            off += 16;
        }
        epet_dynpage_bind(&L->pages[i]);
        L->page_ptrs[i] = &L->pages[i].base;
    }

    /* ---- bytecode ---------------------------------------------------- */
    uint8_t vm_ok = 0;
    if (n_vmpages) {
        memcpy(L->code, pay + code_start, code_len);
        memcpy(L->str_off, pay + str_start, str_count * 2);
        memcpy(L->strpool, pay + str_start + str_count * 2, str_pool);
        /* strings must be nul terminated inside the pool, or a bad offset
         * could walk off the end when one is drawn */
        if (str_pool) L->strpool[str_pool - 1] = 0;
        for (uint32_t i = 0; i < str_count; i++) {
            if (L->str_off[i] >= str_pool) { free(blk); return EPET_BLOB_BOUNDS; }
        }

        L->image.code      = L->code;
        L->image.code_len  = code_len;
        L->image.strings   = L->strpool;
        L->image.str_len   = str_pool;
        L->image.str_off   = L->str_off;
        L->image.n_strings = (uint16_t)str_count;
        L->image.globals   = L->globals;   /* shared by pages and hooks */

        off = vmpg_start;
        for (uint8_t i = 0; i < n_vmpages; i++) {
            epet_vmpage_t *vp = &L->vmpages[i];
            copy_str(vp->title, EPET_DYN_TITLE_MAX, pay + off, 12);
            vp->icon = pay[off + 12];
            vp->img  = &L->image;
            vp->pc_enter  = rd32(pay + off + 14);
            vp->pc_update = rd32(pay + off + 18);
            vp->pc_render = rd32(pay + off + 22);
            vp->pc_leave  = rd32(pay + off + 26);

            /* Prove every entry point safe before it can ever run. */
            const uint32_t entries[4] = { vp->pc_enter, vp->pc_update,
                                          vp->pc_render, vp->pc_leave };
            for (int e = 0; e < 4; e++) {
                if (!entries[e]) continue;
                const char *why = 0;
                if (!epet_vm_validate(&L->image, entries[e], &why)) {
                    free(blk);
                    return EPET_BLOB_BOUNDS;
                }
            }
            epet_vmpage_bind(vp);
            L->page_ptrs[n_pages + vm_ok] = &vp->base;
            vm_ok++;
            off += 30;
        }
    }

    /* ---- background hooks --------------------------------------------- */
    if (n_hooks && code_len) {
        off = hook_start;
        for (uint8_t i = 0; i < n_hooks; i++) {
            uint32_t mask = rd32(pay + off);
            uint32_t pc   = rd32(pay + off + 4);
            off += 8;
            if (!pc) continue;
            const char *why = 0;
            if (!epet_vm_validate(&L->image, pc, &why)) {
                free(blk);
                return EPET_BLOB_BOUNDS;
            }
            L->hooks[L->n_hooks].img      = &L->image;
            L->hooks[L->n_hooks].pc_event = pc;
            L->hooks[L->n_hooks].mask     = mask;
            L->n_hooks++;
        }
    }

    copy_str(L->id, sizeof L->id, d + 8, 12);
    copy_str(L->name, sizeof L->name, d + 20, 24);
    L->mod.id          = L->id;
    L->mod.name        = L->name;
    L->mod.version     = rd16(d + 44);
    L->mod.species     = n_species ? L->species_ptrs : 0;
    L->mod.n_species   = n_species;
    L->mod.pages       = (n_pages + vm_ok) ? L->page_ptrs : 0;
    L->mod.n_pages     = (uint8_t)(n_pages + vm_ok);
    L->mod.on_install  = blob_on_install;
    L->mod.on_remove   = blob_on_remove;

    *out = &L->mod;
    return EPET_BLOB_OK;
}

void epet_modblob_hook_stats(const epet_module_t *m,
                             uint32_t *calls, uint32_t *faults)
{
    if (calls) *calls = 0;
    if (faults) *faults = 0;
    if (!epet_modblob_is_loaded(m)) return;
    const loaded_t *L = (const loaded_t *)((const uint8_t *)m - offsetof(loaded_t, mod));
    for (uint8_t i = 0; i < L->n_hooks; i++) {
        if (calls)  *calls  += L->hooks[i].calls;
        if (faults) *faults += L->hooks[i].faults;
    }
}

void epet_modblob_free(epet_module_t *m)
{
    if (!m) return;
    loaded_t *L = (loaded_t *)((uint8_t *)m - offsetof(loaded_t, mod));
    if (L->tag != LOADED_TAG) return;      /* not ours: compiled-in module */
    L->tag = 0;
    free(L);
}
