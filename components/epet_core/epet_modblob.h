#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "epet_module.h"

/* Binary module packs: the wire format for content installed from a browser.
 *
 * A pack carries CHARACTERS (frames, palettes, poses, backdrops, temperament)
 * and DECLARATIVE PAGES (see epet_dynpage.h). It cannot carry code.
 *
 * Layout, all little-endian, no padding, no alignment assumptions:
 *
 *   header 64 bytes
 *     0   5   "EPMOD"
 *     5   1   format version (EPET_MODBLOB_VERSION)
 *     6   2   flags (reserved, 0)
 *     8  12   id, nul padded
 *    20  24   display name, nul padded
 *    44   2   module version
 *    46   1   n_frames
 *    47   1   n_poses
 *    48   1   n_backdrops
 *    49   1   n_species
 *    50   1   n_pages        (declarative)
 *    51   1   n_vmpages      (bytecode)
 *    52   4   payload length
 *    56   4   FNV-1a of the payload
 *    60   1   n_hooks (background event handlers)
 *    61   3   reserved
 *
 *   payload sections, in order:
 *     frames     n_frames    x { u8 w, u8 h, u8 px[w*h] }
 *     poses      n_poses     x { char name[12], u8 loop, u8 n_keys,
 *                                n_keys x { u8 frame, u16 hold_ms } }
 *     backdrops  n_backdrops x { char name[12], u8 frame, u8 scale,
 *                                u16 night, u8 night_alpha, u16 pal[16] }
 *     species    n_species   x { char name[12], char blurb[28], u16 pal[16],
 *                                u8 scale, f32 temper[4],
 *                                u8 pose0, u8 n_poses, u8 bd0, u8 n_backdrops,
 *                                u8 poop_frame (0xFF = none) }
 *     pages      n_pages     x { char title[12], u8 icon, u8 n_rows,
 *                                n_rows x { u8 kind, u8 src, char label[14] } }
 *     code       u32 len, then len bytes of bytecode
 *     strings    u16 count, u16 pool_len, count x u16 offset, pool bytes
 *     vmpages    n_vmpages   x { char title[12], u8 icon, u8 reserved,
 *                                u32 enter, u32 update, u32 render, u32 leave }
 *     hooks      n_hooks     x { u32 event_mask, u32 pc_event }
 *
 * A hook runs whenever a subscribed event fires, whether or not any of the
 * module's pages are open. That is what makes a pack a background module
 * rather than just a screen.
 *
 * A vmpage entry point of 0 means "no such hook". The code is validated
 * before anything is run; see epet_vm_validate().
 */

#define EPET_MODBLOB_MAGIC   "EPMOD"
#define EPET_MODBLOB_VERSION 3
#define EPET_MODBLOB_HEADER  64

typedef enum {
    EPET_BLOB_OK = 0,
    EPET_BLOB_SHORT,       /* truncated                        */
    EPET_BLOB_MAGIC,       /* not a module pack                */
    EPET_BLOB_VERSION,     /* different format version         */
    EPET_BLOB_CRC,         /* checksum mismatch                */
    EPET_BLOB_BOUNDS,      /* an index or length is out of range */
    EPET_BLOB_MEMORY,      /* allocation failed                */
} epet_blob_result_t;

const char *epet_blob_result_name(epet_blob_result_t r);

/* Reads the id without allocating. Handy for "is this already installed?". */
bool epet_modblob_peek(const uint8_t *data, size_t len,
                       char *id_out, size_t id_cap,
                       char *name_out, size_t name_cap, uint16_t *version_out);

/* Parses into one heap allocation. The returned module borrows nothing from
 * `data`, so the caller may free it. Free with epet_modblob_free(). */
epet_blob_result_t epet_modblob_parse(const uint8_t *data, size_t len,
                                      epet_module_t **out);
void epet_modblob_free(epet_module_t *m);
/* True if this module came from a blob rather than being compiled in. */
bool epet_modblob_is_loaded(const epet_module_t *m);
/* How many times this pack's background hooks ran, and how many of those
 * aborted. Diagnostics: a rising fault count means bad bytecode. */
void epet_modblob_hook_stats(const epet_module_t *m,
                             uint32_t *calls, uint32_t *faults);
