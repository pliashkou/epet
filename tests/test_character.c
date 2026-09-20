/* Sprite frames, pose lookup, the animation player, and temperament. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "epet.h"
#include "epet_species.h"
#include "epet_draw.h"
#include "epet_module.h"

static int failures = 0;
#define CHECK(cond, ...) do {                           \
    if (!(cond)) {                                      \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);   \
        printf(__VA_ARGS__); printf("\n");              \
        failures++;                                     \
    }                                                   \
} while (0)

static const bool NONE[EPET_BTN_COUNT] = {false};
static uint16_t fb[EPET_W * EPET_H];

int main(void)
{
    /* Characters come from modules now, so content must be installed first. */
    epet_modules_reset();
    epet_modules_install(epet_module_core_get(), NULL);

    printf("roster\n");

    CHECK(epet_species_count() >= 2, "need at least 2 species, got %d",
          epet_species_count());

    for (uint8_t i = 0; i < epet_species_count(); i++) {
        const epet_species_t *sp = epet_species_builtin(i);
        CHECK(sp->name && sp->name[0], "species %d needs a name", i);
        CHECK(sp->scale >= 1, "%s: scale must be >= 1", sp->name);
        /* the four poses asked for */
        const char *want[] = { EPET_POSE_IDLE, EPET_POSE_BIRTH,
                               EPET_POSE_HAPPY, EPET_POSE_SAD,
                               EPET_POSE_DEAD };
        const unsigned n_want = sizeof want / sizeof want[0];
        for (unsigned k = 0; k < n_want; k++) {
            const epet_pose_t *po = epet_species_pose(sp, want[k]);
            CHECK(po != NULL, "%s is missing pose '%s'", sp->name, want[k]);
            if (!po) continue;
            CHECK(po->n_keys > 0, "%s/%s has no frames", sp->name, want[k]);
            for (uint8_t f = 0; f < po->n_keys; f++) {
                CHECK(po->keys[f].frame && po->keys[f].frame->px,
                      "%s/%s frame %d has no pixels", sp->name, want[k], f);
                CHECK(po->keys[f].hold_ms > 0,
                      "%s/%s frame %d has a 0 ms hold, which would stall the "
                      "player", sp->name, want[k], f);
            }
        }
        /* Nothing may touch the canvas edge: an opaque pixel on the
         * outermost row or column means the art ran off and was cut. This is
         * how SPROUT's leaf was being sliced off in the tall happy frames. */
        for (unsigned k = 0; k < n_want; k++) {
            const epet_pose_t *po = epet_species_pose(sp, want[k]);
            if (!po) continue;
            for (uint8_t fi = 0; fi < po->n_keys; fi++) {
                const epet_frame_t *fr = po->keys[fi].frame;
                if (!fr || !fr->px) continue;
                int clipped = 0;
                for (int x = 0; x < fr->w; x++) {
                    if (fr->px[x]) clipped++;                        /* top */
                    if (fr->px[(fr->h - 1) * fr->w + x]) clipped++;  /* bottom */
                }
                for (int y = 0; y < fr->h; y++) {
                    if (fr->px[y * fr->w]) clipped++;                /* left */
                    if (fr->px[y * fr->w + fr->w - 1]) clipped++;    /* right */
                }
                CHECK(clipped == 0,
                      "%s/%s frame %d has %d opaque pixels on the canvas edge "
                      "-- the art is being clipped",
                      sp->name, want[k], fi, clipped);
            }
        }

        CHECK(epet_species_pose(sp, EPET_POSE_IDLE)->loop,
              "%s: idle must loop", sp->name);
        CHECK(!epet_species_pose(sp, EPET_POSE_BIRTH)->loop,
              "%s: birth must be a one-shot", sp->name);
    }

    CHECK(epet_species_by_name(epet_species_builtin(0)->name) == epet_species_builtin(0),
          "lookup by name should round-trip");
    CHECK(epet_species_by_name("NOPE") == NULL, "unknown name should be NULL");

    printf("pose fallback\n");

    const epet_species_t *sp = epet_species_builtin(0);
    CHECK(epet_species_pose(sp, "cartwheel") == NULL,
          "an undefined pose should not be invented");
    CHECK(epet_species_pose_or_idle(sp, "cartwheel")
              == epet_species_pose(sp, EPET_POSE_IDLE),
          "an unknown pose must fall back to idle, so adding pose names later "
          "cannot crash an older species");

    printf("player\n");

    epet_actor_t a;
    epet_actor_init(&a, sp);
    CHECK(strcmp(epet_actor_pose_name(&a), EPET_POSE_IDLE) == 0,
          "should start idle, got %s", epet_actor_pose_name(&a));

    /* a one-shot returns to its resume pose */
    epet_actor_play(&a, EPET_POSE_BIRTH, EPET_POSE_IDLE);
    CHECK(strcmp(epet_actor_pose_name(&a), EPET_POSE_BIRTH) == 0, "birth playing");
    for (int i = 0; i < 400; i++) epet_actor_tick(&a, 50);   /* 20 s */
    CHECK(strcmp(epet_actor_pose_name(&a), EPET_POSE_IDLE) == 0,
          "birth should return to idle, got %s", epet_actor_pose_name(&a));

    /* a looping pose never finishes */
    epet_actor_play(&a, EPET_POSE_IDLE, NULL);
    for (int i = 0; i < 400; i++) epet_actor_tick(&a, 50);
    CHECK(strcmp(epet_actor_pose_name(&a), EPET_POSE_IDLE) == 0, "idle still idle");
    CHECK(!epet_actor_finished(&a), "a looping pose should never report finished");

    /* a single huge tick must not skip past the end of a one-shot */
    epet_actor_play(&a, EPET_POSE_BIRTH, EPET_POSE_IDLE);
    epet_actor_tick(&a, 600000);      /* 10 minutes in one go, as after sleep */
    CHECK(strcmp(epet_actor_pose_name(&a), EPET_POSE_IDLE) == 0,
          "a long catch-up tick should land on idle, got %s",
          epet_actor_pose_name(&a));

    /* ensure() is idempotent */
    epet_actor_ensure(&a, EPET_POSE_IDLE);
    uint8_t k = a.key;
    epet_actor_ensure(&a, EPET_POSE_IDLE);
    CHECK(a.key == k, "ensure on the running pose must not restart it");

    printf("transparency\n");

    /* index 0 must leave the background untouched */
    const epet_frame_t *f = epet_species_pose(sp, EPET_POSE_IDLE)->keys[0].frame;
    const uint16_t bg = EPET_RGB565(255, 0, 255);
    epet_fill(fb, bg);
    epet_blit_centred(fb, 120, 120, f, &sp->palette, 1);
    int untouched = 0, painted = 0;
    for (int i = 0; i < EPET_W * EPET_H; i++) {
        if (fb[i] == bg) untouched++; else painted++;
    }
    CHECK(painted > 100, "the sprite should have painted something, got %d", painted);
    CHECK(untouched > EPET_W * EPET_H / 2,
          "most of the screen should be untouched by a small transparent "
          "sprite, got %d painted", painted);

    /* corners are index 0 in every frame, so they must survive */
    CHECK(fb[0] == bg, "top-left must be untouched");
    CHECK(fb[EPET_W * EPET_H - 1] == bg, "bottom-right must be untouched");

    printf("temperament\n");

    /* two species with different temperaments must diverge */
    const epet_species_t *a_sp = epet_species_builtin(0);
    const epet_species_t *b_sp = epet_species_builtin(1);
    CHECK(a_sp != b_sp, "need two distinct species");

    epet_t pa, pb;
    epet_init(&pa); epet_set_species(&pa, a_sp, false);
    epet_init(&pb); epet_set_species(&pb, b_sp, false);
    CHECK(pa.species == a_sp && pb.species == b_sp, "species pinned for the test");
    pa.display_timeout_ms = pb.display_timeout_ms = 0;
    for (int i = 0; i < 600; i++) {     /* 30 s */
        epet_update(&pa, 50, NONE, NONE);
        epet_update(&pb, 50, NONE, NONE);
    }
    bool differs = fabsf(pa.hunger - pb.hunger) > 1.0f ||
                   fabsf(pa.hygiene - pb.hygiene) > 1.0f ||
                   fabsf(pa.energy - pb.energy) > 1.0f;
    CHECK(differs,
          "temperament should make the classes diverge: %s hunger=%.1f wsh=%.1f "
          "vs %s hunger=%.1f wsh=%.1f",
          a_sp->name, pa.hunger, pa.hygiene, b_sp->name, pb.hunger, pb.hygiene);

    printf("backdrops\n");

    for (uint8_t i = 0; i < epet_species_count(); i++) {
        const epet_species_t *s2 = epet_species_builtin(i);
        CHECK(s2->n_backdrops >= 1, "%s needs at least one backdrop", s2->name);

        for (uint8_t b = 0; b < s2->n_backdrops; b++) {
            const epet_backdrop_t *bd = epet_species_backdrop(s2, b);
            CHECK(bd != NULL, "%s backdrop %d should resolve", s2->name, b);
            CHECK(bd->name && bd->name[0], "%s backdrop %d needs a name", s2->name, b);
            CHECK(bd->image && bd->palette,
                  "%s/%s needs an image and its own palette", s2->name, bd->name);
            if (!bd->image) continue;

            int sc = bd->scale ? bd->scale : 1;
            CHECK(bd->image->w * sc >= EPET_W && bd->image->h * sc >= EPET_H,
                  "%s/%s is %dx%d at scale %d -- too small to cover the screen",
                  s2->name, bd->name, bd->image->w, bd->image->h, sc);

            /* A backdrop must be fully opaque. Index 0 is transparent, so a
             * gap would leave the PREVIOUS frame showing through. */
            int holes = 0;
            for (int px = 0; px < bd->image->w * bd->image->h; px++) {
                if (bd->image->px[px] == 0) holes++;
            }
            CHECK(holes == 0,
                  "%s/%s has %d transparent pixels; a backdrop must be opaque "
                  "or the last frame shows through", s2->name, bd->name, holes);
        }

        /* the index wraps rather than reading off the end */
        CHECK(epet_species_backdrop(s2, 200) != NULL,
              "%s: an out-of-range backdrop index should wrap", s2->name);
    }

    /* drawing must touch every pixel of the screen */
    const epet_species_t *bs = epet_species_builtin(0);
    const uint16_t canary = EPET_RGB565(255, 0, 255);
    epet_fill(fb, canary);
    epet_backdrop_draw(epet_species_backdrop(bs, 0), fb, false);
    int left = 0;
    for (int i = 0; i < EPET_W * EPET_H; i++) if (fb[i] == canary) left++;
    CHECK(left == 0, "backdrop left %d pixels unpainted", left);

    /* a zeroed backdrop, and NULL, must still fill the screen */
    epet_fill(fb, canary);
    epet_backdrop_draw(NULL, fb, false);
    left = 0;
    for (int i = 0; i < EPET_W * EPET_H; i++) if (fb[i] == canary) left++;
    CHECK(left == 0, "a NULL backdrop should still fill the screen, %d left", left);

    /* the night tint must actually change the image */
    static uint16_t day[EPET_W * EPET_H];
    epet_backdrop_draw(epet_species_backdrop(bs, 0), day, false);
    epet_backdrop_draw(epet_species_backdrop(bs, 0), fb, true);
    int changed = 0;
    for (int i = 0; i < EPET_W * EPET_H; i++) if (fb[i] != day[i]) changed++;
    CHECK(changed > EPET_W * EPET_H / 2,
          "night should tint most of the backdrop, changed %d", changed);

    /* a pet gets a backdrop, and cycling wraps */
    epet_t bp; epet_init(&bp);
    CHECK(epet_pet_backdrop(&bp) != NULL, "a pet should have a backdrop");
    uint8_t n_bd = bp.species->n_backdrops;
    uint8_t start = bp.backdrop;
    for (int i = 0; i < n_bd; i++) epet_pet_next_backdrop(&bp);
    CHECK(bp.backdrop == start,
          "cycling through all %d backdrops should return to the start", n_bd);

    printf("random birth\n");

    /* the class is rolled at birth, and the roll is reproducible from a seed */
    epet_seed_random(12345);
    epet_t r1; epet_init(&r1);
    epet_seed_random(12345);
    epet_t r2; epet_init(&r2);
    CHECK(r1.species == r2.species,
          "the same seed must produce the same class, got %s vs %s",
          r1.species->name, r2.species->name);

    /* over many births we should see more than one class */
    epet_seed_random(1);
    const epet_species_t *first = NULL;
    bool varied = false;
    for (int i = 0; i < 80; i++) {
        epet_t t; epet_init(&t);
        if (!first) first = t.species;
        else if (t.species != first) varied = true;
    }
    CHECK(varied, "births should roll different classes over time");

    printf("integration\n");

    /* a new pet hatches */
    epet_t pet; epet_init(&pet);
    CHECK(pet.species != NULL, "a fresh pet should have a species");
    CHECK(strcmp(epet_actor_pose_name(&pet.actor), EPET_POSE_BIRTH) == 0,
          "a fresh pet should be playing birth, got %s",
          epet_actor_pose_name(&pet.actor));

    /* feeding makes it happy */
    pet.display_timeout_ms = 0;
    for (int i = 0; i < 100; i++) epet_update(&pet, 50, NONE, NONE);  /* past birth */
    epet_apply_action(&pet, EPET_ACT_FEED);
    CHECK(strcmp(epet_actor_pose_name(&pet.actor), EPET_POSE_HAPPY) == 0,
          "feeding should play happy, got %s", epet_actor_pose_name(&pet.actor));

    /* a dead pet plays its own pose, not a greyed idle frame */
    epet_init(&pet);
    pet.display_timeout_ms = 0;
    for (int i = 0; i < 4000; i++) epet_update(&pet, 50, NONE, NONE);
    CHECK(!pet.alive, "pet should be dead");
    CHECK(strcmp(epet_actor_pose_name(&pet.actor), EPET_POSE_DEAD) == 0,
          "a dead pet should play the dead pose, got %s",
          epet_actor_pose_name(&pet.actor));
    CHECK(epet_species_pose(pet.species, EPET_POSE_DEAD)->loop,
          "the dead pose should loop");

    /* a miserable pet settles into sad on its own */
    epet_init(&pet);
    pet.display_timeout_ms = 0;
    for (int i = 0; i < 1600; i++) epet_update(&pet, 50, NONE, NONE);  /* 80 s */
    if (pet.alive) {
        epet_mood_t m = epet_mood(&pet);
        if (m == EPET_MOOD_SAD || m == EPET_MOOD_SICK) {
            CHECK(strcmp(epet_actor_pose_name(&pet.actor), EPET_POSE_SAD) == 0,
                  "a sad pet should loop the sad pose, got %s",
                  epet_actor_pose_name(&pet.actor));
        }
    }

    /* swapping class keeps the stats but restarts the animation */
    epet_init(&pet);
    pet.display_timeout_ms = 0;
    for (int i = 0; i < 400; i++) epet_update(&pet, 50, NONE, NONE);
    float hunger_before = pet.hunger;
    epet_set_species(&pet, b_sp, true);
    CHECK(pet.species == b_sp, "species should have changed");
    CHECK(fabsf(pet.hunger - hunger_before) < 0.01f,
          "swapping class must not reset the pet's stats");
    CHECK(strcmp(epet_actor_pose_name(&pet.actor), EPET_POSE_BIRTH) == 0,
          "adopting should replay birth");

    printf("fractional blitter\n");
    {
        /* epet_blit_q8() replaced the integer blitter on the main screen on
         * the promise that it is identical at whole-number scales. That is a
         * claim about output, so check the output rather than trusting the
         * arithmetic. */
        static uint16_t a[EPET_W * EPET_H], b[EPET_W * EPET_H];
        const epet_species_t *sp = epet_species_builtin(0);
        const epet_pose_t *po = epet_species_pose(sp, EPET_POSE_IDLE);
        const epet_frame_t *f = po->keys[0].frame;

        for (int sc = 1; sc <= 3; sc++) {
            memset(a, 0, sizeof a);
            memset(b, 0, sizeof b);
            epet_blit(a, 20, 30, f, &sp->palette, sc);
            epet_blit_q8(b, 20, 30, f, &sp->palette, sc * 256);
            CHECK(memcmp(a, b, sizeof a) == 0,
                  "q8 blit differs from the integer blit at scale %d", sc);
        }

        /* A fractional scale must land between the two integer sizes, or the
         * ramp is not actually doing anything. */
        int w2 = (f->w * 512) >> 8, w3 = (f->w * 768) >> 8;
        int wf = (f->w * 640) >> 8;
        CHECK(wf > w2 && wf < w3,
              "1.5x width %d should sit between %d and %d", wf, w2, w3);

        /* Clipping: drawing partly off each edge must not corrupt memory or
         * wrap onto the opposite side of the framebuffer. */
        memset(b, 0, sizeof b);
        epet_blit_q8(b, -30, -20, f, &sp->palette, 700);
        epet_blit_q8(b, EPET_W - 5, EPET_H - 5, f, &sp->palette, 700);
        for (int y = 0; y < EPET_H; y++) {
            CHECK(!(b[y * EPET_W + EPET_W - 1] && b[y * EPET_W]),
                  "row %d looks wrapped", y);
        }
    }

    printf("growth\n");
    {
        const epet_species_t *sp = epet_species_builtin(0);
        CHECK(sp->n_stages > 0, "a built-in species should declare stages");

        /* Stages must be ordered and in range, or resolution silently picks
         * the wrong one for part of the age range. */
        for (uint8_t i = 0; i < sp->n_stages; i++) {
            CHECK(sp->stages[i].from_age >= 1 &&
                  sp->stages[i].from_age <= EPET_AGE_MAX,
                  "stage %u starts at age %u, outside 1..%d",
                  i, sp->stages[i].from_age, EPET_AGE_MAX);
            if (i) CHECK(sp->stages[i].from_age > sp->stages[i - 1].from_age,
                         "stage %u starts before stage %u", i, i - 1);
        }

        /* Every age level resolves to something drawable, including the ends
         * and beyond the top. */
        for (int age = 0; age <= EPET_AGE_MAX + 5; age++) {
            const epet_pose_t *po =
                epet_species_pose_at(sp, (uint8_t)age, EPET_POSE_IDLE);
            CHECK(po && po->n_keys, "age %d has no idle pose", age);
            int q8 = epet_species_scale_q8_at(sp, (uint8_t)age);
            CHECK(q8 > 0, "age %d has a non-positive scale", age);
        }

        /* The size must actually change, and never shrink with age -- a ramp
         * that went backwards would read as the pet withering. */
        int first = epet_species_scale_q8_at(sp, 1);
        int last  = epet_species_scale_q8_at(sp, EPET_AGE_MAX);
        CHECK(last > first, "a grown pet should be bigger: %d -> %d", first, last);
        int prev = 0, distinct = 0;
        for (int age = 1; age <= EPET_AGE_MAX; age++) {
            int q8 = epet_species_scale_q8_at(sp, (uint8_t)age);
            if (age > 1) CHECK(q8 >= prev - 32,
                               "size fell sharply from age %d to %d", age - 1, age);
            if (q8 != prev) distinct++;
            prev = q8;
        }
        CHECK(distinct > sp->n_stages,
              "the ramp should give more sizes (%d) than there are stages (%u)",
              distinct, sp->n_stages);

        /* Growth stops at the top rather than running away. */
        CHECK(epet_species_scale_q8_at(sp, EPET_AGE_MAX) ==
              epet_species_scale_q8_at(sp, 250),
              "age past the maximum must not keep growing");

        /* An unknown pose still resolves, at every age. */
        CHECK(epet_species_pose_at(sp, 1, "nosuchpose") != NULL,
              "an unknown pose must fall back, not return NULL");

        /* A species with no stages at all keeps working at its base scale. */
        epet_species_t bare = *sp;
        bare.stages = NULL; bare.n_stages = 0;
        CHECK(epet_species_scale_q8_at(&bare, 1) == sp->scale * 256,
              "a species without stages should use its base scale");
        CHECK(epet_species_scale_q8_at(&bare, EPET_AGE_MAX) == sp->scale * 256,
              "a species without stages should never change size");
    }

    printf("ageing\n");
    {
        epet_init(&pet);
        pet.display_timeout_ms = 0;
        CHECK(epet_age_level(&pet) == 1, "a newborn is age 1, got %u",
              epet_age_level(&pet));

        /* The actor has to be TOLD the age; if the wiring in epet_update()
         * is lost the pet silently never grows, which is exactly the kind of
         * thing that looks fine in a screenshot. */
        uint8_t a0 = pet.actor.age;
        for (int i = 0; i < 600; i++) epet_update(&pet, 50, NONE, NONE);
        CHECK(pet.actor.age > a0,
              "the actor's age should follow the pet's, %u -> %u",
              a0, pet.actor.age);
        CHECK(pet.actor.age == epet_age_level(&pet),
              "actor age %u should match the pet's level %u",
              pet.actor.age, epet_age_level(&pet));

        /* It clamps rather than wrapping: age_ms is 32-bit milliseconds, so
         * a long-lived pet would otherwise index past the stage table. */
        pet.age_ms = 0xFFFFFFFFu;
        CHECK(epet_age_level(&pet) == EPET_AGE_MAX,
              "a very old pet should clamp to %d, got %u",
              EPET_AGE_MAX, epet_age_level(&pet));

        /* A rebirth starts over. */
        epet_init(&pet);
        CHECK(epet_age_level(&pet) == 1, "a new creature starts at age 1");
    }

    printf("growing mid-animation\n");
    {
        /* Crossing a stage boundary must not restart the animation: the pose
         * is re-resolved in the new stage but keeps its frame and phase. */
        epet_init(&pet);
        pet.display_timeout_ms = 0;
        const epet_species_t *sp = pet.species;
        uint8_t boundary = sp->n_stages > 1 ? sp->stages[1].from_age : 2;

        pet.actor.age = (uint8_t)(boundary - 1);
        epet_actor_play(&pet.actor, EPET_POSE_IDLE, 0);
        epet_actor_tick(&pet.actor, 950);          /* land on a later frame */
        uint8_t key_before = pet.actor.key;
        const epet_pose_t *pose_before = pet.actor.pose;

        epet_actor_set_age(&pet.actor, boundary);
        CHECK(pet.actor.key == key_before,
              "growing up mid-pose should keep the frame index, %u -> %u",
              key_before, pet.actor.key);
        CHECK(strcmp(epet_actor_pose_name(&pet.actor), EPET_POSE_IDLE) == 0,
              "growing up should keep playing the same pose");
        if (sp->n_stages > 1 && sp->stages[1].poses)
            CHECK(pet.actor.pose != pose_before,
                  "a stage with its own art should swap the pose data");
    }

    printf(failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
