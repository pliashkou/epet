#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "epet_ui.h"

/* Declarative sub-programs.
 *
 * A page that arrives over the wire cannot be code -- epet_page_t is function
 * pointers. So a loaded page is DATA: a title, an icon id, and a list of rows
 * naming what to show. One compiled renderer interprets them.
 *
 * This covers information screens, which is most of what a content pack
 * wants. It does NOT cover arbitrary logic: anything that needs real
 * behaviour still has to be compiled in. */

typedef enum {
    EPET_ROW_GAP = 0,      /* blank space                                   */
    EPET_ROW_TEXT,         /* the label, on its own                         */
    EPET_ROW_VALUE,        /* label + a number from `src`                   */
    EPET_ROW_BAR,          /* label + a 0..100 meter from `src`             */
    EPET_ROW_STRING,       /* label + a string from `src`                   */
    EPET_ROW_SPRITE,       /* the current character, drawn                  */
    EPET_ROW_KIND_COUNT
} epet_row_kind_t;

typedef enum {
    EPET_SRC_NONE = 0,
    EPET_SRC_HUNGER, EPET_SRC_FED, EPET_SRC_HAPPINESS, EPET_SRC_ENERGY,
    EPET_SRC_HYGIENE, EPET_SRC_HEALTH,
    EPET_SRC_AGE_S, EPET_SRC_POOP,
    EPET_SRC_SPECIES_NAME, EPET_SRC_SPECIES_BLURB, EPET_SRC_MODULE_ID,
    EPET_SRC_BACKDROP_NAME, EPET_SRC_POSE_NAME, EPET_SRC_MOOD,
    EPET_SRC_TEMPER_HUNGER, EPET_SRC_TEMPER_HAPPY,
    EPET_SRC_TEMPER_ENERGY, EPET_SRC_TEMPER_HYGIENE,
    EPET_SRC_N_BACKDROPS, EPET_SRC_N_POSES, EPET_SRC_SPRITE_SCALE,
    EPET_SRC_SPECIES_COUNT, EPET_SRC_MODULE_COUNT,
    EPET_SRC_COUNT
} epet_src_t;

#define EPET_ROW_LABEL_MAX 14
#define EPET_DYN_TITLE_MAX 12

typedef struct {
    uint8_t kind;                          /* epet_row_kind_t */
    uint8_t src;                           /* epet_src_t      */
    char    label[EPET_ROW_LABEL_MAX];
} epet_dynrow_t;

typedef struct {
    epet_page_t          base;             /* must be first: cast works */
    char                 title[EPET_DYN_TITLE_MAX];
    uint8_t              icon;             /* small built-in icon id */
    uint8_t              n_rows;
    const epet_dynrow_t *rows;
} epet_dynpage_t;

/* Point `base` at the shared renderer. Call once after filling the struct. */
void epet_dynpage_bind(epet_dynpage_t *p);

/* Exposed for tests and for anything that wants the same formatting. */
bool        epet_src_number(epet_src_t s, const epet_t *pet, float *out);
const char *epet_src_string(epet_src_t s, const epet_t *pet);
