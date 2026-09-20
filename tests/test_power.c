/* Contract tests for the display-power policy in epet_core.
 * Built and run by `make test` in sim/. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "epet.h"

static int failures = 0;

#define CHECK(cond, ...) do {                                   \
    if (!(cond)) {                                              \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);           \
        printf(__VA_ARGS__); printf("\n");                      \
        failures++;                                             \
    }                                                           \
} while (0)

static const bool NONE[EPET_BTN_COUNT] = {false};

static void advance(epet_t *p, uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 50) {
        epet_update(p, 50, NONE, NONE);
    }
}

/* A raw button press: used to test the display wake/swallow contract. */
static uint32_t press(epet_t *p, epet_btn_t b)
{
    bool edge[EPET_BTN_COUNT] = {false};
    bool down[EPET_BTN_COUNT] = {false};
    edge[b] = down[b] = true;
    return epet_update(p, 50, down, edge);
}

int main(void)
{
    printf("display power policy\n");

    /* 1. starts on */
    epet_t p;
    epet_init(&p);
    CHECK(p.display_on, "display should start on");
    CHECK(p.display_timeout_ms == 30000, "default timeout should be 30000, got %u",
          p.display_timeout_ms);

    /* 2. stays on just under the timeout, blanks at/after it */
    advance(&p, 29000);
    CHECK(p.display_on, "should still be on at 29s idle");
    advance(&p, 2000);
    CHECK(!p.display_on, "should be blanked by 31s idle");

    /* 3. a press while blanked wakes, and its edge is SWALLOWED */
    uint32_t mask = press(&p, EPET_BTN_LT);
    CHECK(p.display_on, "press should wake the display");
    CHECK(mask == 0, "waking press must report no surviving edge, got 0x%x", mask);

    /* 4. a press while awake reports its edge to the UI */
    mask = press(&p, EPET_BTN_LT);
    CHECK(mask == EPET_BTN_BIT(EPET_BTN_LT),
          "second press should survive, got 0x%x", mask);

    /* 4b. actions are applied explicitly, not by buttons */
    float hunger_before = p.hunger;
    CHECK(epet_apply_action(&p, EPET_ACT_FEED), "feed should succeed");
    CHECK(p.hunger < hunger_before - 20.0f,
          "feeding should drop hunger: %.1f -> %.1f", hunger_before, p.hunger);

    /* 5. activity resets the idle timer */
    advance(&p, 25000);
    press(&p, EPET_BTN_LB);
    CHECK(p.idle_ms == 0, "press should reset idle timer, got %u", p.idle_ms);
    advance(&p, 25000);
    CHECK(p.display_on, "25s after a press the display should still be on");

    /* 6. timeout of 0 disables blanking entirely */
    epet_init(&p);
    p.display_timeout_ms = 0;
    advance(&p, 60000);
    CHECK(p.display_on, "timeout 0 should keep the display on forever");

    /* 7. the pet keeps living while the screen is off */
    epet_init(&p);
    float age_before = (float)p.age_ms;
    advance(&p, 50000);
    CHECK(!p.display_on, "screen should be off after 50s idle");
    CHECK((float)p.age_ms > age_before + 45000.0f,
          "pet should keep ageing while blanked: %u ms", p.age_ms);
    CHECK(p.hunger > 20.0f, "hunger should keep rising while blanked: %.1f", p.hunger);

    /* 8. epet_advance matches small-step integration over a long gap */
    printf("long-gap catch-up\n");
    /* Class is rolled at birth, so seed identically or these two pets are
     * different species with different decay rates. */
    epet_t stepped, jumped;
    epet_seed_random(42); epet_init(&stepped);
    epet_seed_random(42); epet_init(&jumped);
    CHECK(stepped.species == jumped.species,
          "same seed must give the same class");
    advance(&stepped, 60000);                        /* 1 min in 50 ms steps */
    epet_advance(&jumped, 60000, NONE, NONE);        /* same gap in one call */
    CHECK(fabsf(stepped.hunger - jumped.hunger) < 2.0f,
          "hunger drift: stepped %.1f vs advanced %.1f", stepped.hunger, jumped.hunger);
    CHECK(fabsf(stepped.health - jumped.health) < 3.0f,
          "health drift: stepped %.1f vs advanced %.1f", stepped.health, jumped.health);
    CHECK(stepped.asleep == jumped.asleep, "sleep state should agree");
    CHECK(stepped.poop == jumped.poop,
          "poop count should agree: %u vs %u", stepped.poop, jumped.poop);
    CHECK(jumped.age_ms == stepped.age_ms,
          "age should agree: stepped %u vs advanced %u", stepped.age_ms, jumped.age_ms);
    CHECK(jumped.alive && jumped.age_ms == 60000,
          "pet should survive 60s and age exactly the gap, got %u alive=%d",
          jumped.age_ms, jumped.alive);

    /* 8b. death timing agrees between the two integration paths */
    epet_t died_step, died_jump;
    epet_seed_random(7); epet_init(&died_step);
    epet_seed_random(7); epet_init(&died_jump);
    advance(&died_step, 200000);
    epet_advance(&died_jump, 200000, NONE, NONE);
    CHECK(!died_step.alive && !died_jump.alive, "pet should be dead after 200s");
    CHECK(died_step.age_ms == died_jump.age_ms,
          "death time should agree: %u vs %u", died_step.age_ms, died_jump.age_ms);

    /* 9. a single huge epet_update is what epet_advance protects against --
     *    confirm the naive path really does diverge, so the guard has a point */
    epet_t naive;
    epet_seed_random(42); epet_init(&naive);
    epet_update(&naive, 60000, NONE, NONE);
    CHECK(naive.poop != stepped.poop || naive.health != stepped.health,
          "expected one-shot update to diverge from stepped integration");

    /* 10. waking press after a long gap leaves the display ON, not re-blanked */
    epet_t woke;
    epet_init(&woke);
    advance(&woke, 35000);
    CHECK(!woke.display_on, "screen should be off before the wake test");
    bool edge[EPET_BTN_COUNT] = {false}, held[EPET_BTN_COUNT] = {false};
    edge[EPET_BTN_LT] = held[EPET_BTN_LT] = true;
    uint32_t m = epet_advance(&woke, 600000, held, edge);  /* 10 min, then press */
    CHECK(woke.display_on, "display must be ON after the waking press");
    CHECK(woke.idle_ms == 0, "idle timer should be reset, got %u", woke.idle_ms);
    CHECK(m == 0, "waking press must still be swallowed after a long gap, got 0x%x", m);

    printf(failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
