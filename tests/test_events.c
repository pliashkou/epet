/* Contract tests for the event bus and the care module. */
#include <stdio.h>
#include <string.h>
#include "epet.h"
#include "epet_care.h"
#include "epet_species.h"

static int failures = 0;
#define CHECK(cond, ...) do {                           \
    if (!(cond)) {                                      \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);   \
        printf(__VA_ARGS__); printf("\n");              \
        failures++;                                     \
    }                                                   \
} while (0)

static const bool NONE[EPET_BTN_COUNT] = {false};

/* Press a button for real. If the display is blanked the first press is
 * swallowed as a wake press, so this sends a second one -- exactly what a
 * user does. */
static void press_awake(epet_t *p, epet_bus_t *bus, epet_action_t act)
{
    epet_apply_action(p, act);
    epet_update(p, 50, NONE, NONE);
    if (bus) epet_bus_dispatch(bus);
}

/* ---- recorder subscriber -------------------------------------------- */
typedef struct {
    int      n;
    epet_event_type_t seen[128];
} rec_t;

static void rec_fn(const epet_event_t *ev, void *ctx)
{
    rec_t *r = ctx;
    if (r->n < 128) r->seen[r->n++] = ev->type;
}

static int count_of(const rec_t *r, epet_event_type_t t)
{
    int n = 0;
    for (int i = 0; i < r->n; i++) if (r->seen[i] == t) n++;
    return n;
}

/* a handler that republishes, to prove queued dispatch handles it */
static void echo_fn(const epet_event_t *ev, void *ctx)
{
    epet_bus_t *bus = ctx;
    if (ev->type == EPET_EV_FED) {
        epet_bus_publish(bus, EPET_EV_PLAYED, ev->age_ms, 0, 0);
    }
}

/* a handler that republishes its own event forever */
static void loop_fn(const epet_event_t *ev, void *ctx)
{
    epet_bus_t *bus = ctx;
    epet_bus_publish(bus, ev->type, ev->age_ms, 0, 0);
}

int main(void)
{
    printf("event bus\n");

    /* 1. mask filtering: a subscriber only sees what it asked for */
    epet_bus_t bus;
    epet_bus_init(&bus);
    rec_t only_fed = {0};
    epet_bus_subscribe(&bus, EPET_EV_MASK(EPET_EV_FED), rec_fn, &only_fed, "fed");
    epet_bus_publish(&bus, EPET_EV_FED, 0, 0, 0);
    epet_bus_publish(&bus, EPET_EV_POOPED, 0, 0, 0);
    epet_bus_dispatch(&bus);
    CHECK(only_fed.n == 1, "masked subscriber should see 1 event, saw %d", only_fed.n);
    CHECK(only_fed.seen[0] == EPET_EV_FED, "should be FED");

    /* 2. publishing from inside a handler works (queued, not recursive) */
    epet_bus_init(&bus);
    rec_t all = {0};
    epet_bus_subscribe(&bus, EPET_EV_ALL, echo_fn, &bus, "echo");
    epet_bus_subscribe(&bus, EPET_EV_ALL, rec_fn, &all, "rec");
    epet_bus_publish(&bus, EPET_EV_FED, 0, 0, 0);
    epet_bus_dispatch(&bus);
    CHECK(count_of(&all, EPET_EV_FED) == 1, "should see the FED");
    CHECK(count_of(&all, EPET_EV_PLAYED) == 1, "handler's PLAYED should be delivered");

    /* 3. a runaway handler cannot wedge dispatch */
    epet_bus_init(&bus);
    epet_bus_subscribe(&bus, EPET_EV_ALL, loop_fn, &bus, "loop");
    epet_bus_publish(&bus, EPET_EV_FED, 0, 0, 0);
    epet_bus_dispatch(&bus);      /* must return */
    CHECK(1, "dispatch returned from a self-republishing handler");

    /* 4. queue overflow is counted, not silently corrupting */
    epet_bus_init(&bus);
    rec_t sink = {0};
    epet_bus_subscribe(&bus, EPET_EV_ALL, rec_fn, &sink, "sink");
    for (int i = 0; i < EPET_EVENT_QUEUE_LEN + 5; i++) {
        epet_bus_publish(&bus, EPET_EV_POOPED, 0, i, 0);
    }
    CHECK(bus.dropped == 5, "expected 5 drops, got %u", bus.dropped);
    epet_bus_dispatch(&bus);
    CHECK(sink.n == EPET_EVENT_QUEUE_LEN,
          "should deliver a full queue, got %d", sink.n);

    /* 5. subscriber table is bounded and reports it */
    epet_bus_init(&bus);
    for (int i = 0; i < EPET_MAX_SUBSCRIBERS; i++) {
        CHECK(epet_bus_subscribe(&bus, EPET_EV_ALL, rec_fn, &sink, "x"),
              "subscribe %d should succeed", i);
    }
    CHECK(!epet_bus_subscribe(&bus, EPET_EV_ALL, rec_fn, &sink, "overflow"),
          "subscribing past the limit must fail, not overflow");

    printf("simulation events\n");

    /* 6. MINUTE fires exactly once per minute, including across a long gap */
    epet_bus_init(&bus);
    rec_t mins = {0};
    epet_bus_subscribe(&bus, EPET_EV_MASK(EPET_EV_MINUTE), rec_fn, &mins, "min");
    epet_t pet;
    epet_init(&pet);
    epet_attach_bus(&pet, &bus);
    epet_advance(&pet, 5 * 60000, NONE, NONE);   /* 5 minutes in one jump */
    epet_bus_dispatch(&bus);
    /* The pet starves partway through, and age_ms stops at death -- so the
     * contract is one MINUTE per elapsed minute of LIFE, not of wall clock. */
    int expect = (int)(pet.age_ms / 60000u);
    CHECK(expect >= 1, "pet should have lived at least a minute");
    CHECK(count_of(&mins, EPET_EV_MINUTE) == expect,
          "expected %d MINUTE events (age %u ms), got %d",
          expect, pet.age_ms, count_of(&mins, EPET_EV_MINUTE));

    /* and a pet kept alive really does get one per minute */
    epet_bus_init(&bus);
    rec_t mins2 = {0};
    epet_bus_subscribe(&bus, EPET_EV_MASK(EPET_EV_MINUTE), rec_fn, &mins2, "min2");
    epet_init(&pet);
    epet_attach_bus(&pet, &bus);
    for (int m = 0; m < 5; m++) {
        for (int i = 0; i < 20; i++) {          /* 60 s in 3 s slices */
            epet_advance(&pet, 3000, NONE, NONE);
            epet_bus_dispatch(&bus);
            /* Hunger is not the only killer: poop drives hygiene down and
             * that strains health, so keep it clean and entertained too. */
            press_awake(&pet, &bus, EPET_ACT_FEED);
            press_awake(&pet, &bus, EPET_ACT_CLEAN);
            press_awake(&pet, &bus, EPET_ACT_PLAY);
        }
    }
    CHECK(pet.alive, "fed pet should survive 5 minutes");
    CHECK(count_of(&mins2, EPET_EV_MINUTE) == 5,
          "a living pet should get 5 MINUTE events, got %d",
          count_of(&mins2, EPET_EV_MINUTE));

    /* 7. need events latch: HUNGRY fires once, not every frame */
    epet_bus_init(&bus);
    rec_t needs = {0};
    epet_bus_subscribe(&bus, EPET_EV_MASK(EPET_EV_HUNGRY), rec_fn, &needs, "need");
    epet_init(&pet);
    /* Pin the class: temperament changes the hunger rate, and this test is
     * about the latch, not about how fast a given species starves. */
    epet_set_species(&pet, epet_species_builtin(0), false);
    epet_attach_bus(&pet, &bus);
    for (int i = 0; i < 1200; i++) {             /* 60 s at 50 ms */
        epet_update(&pet, 50, NONE, NONE);
        epet_bus_dispatch(&bus);
    }
    CHECK(pet.hunger > 75.0f, "hunger should be past the threshold: %.0f", pet.hunger);
    CHECK(count_of(&needs, EPET_EV_HUNGRY) == 1,
          "HUNGRY must latch and fire once, got %d", count_of(&needs, EPET_EV_HUNGRY));

    /* 8. the bus survives death and rebirth without losing subscribers */
    epet_bus_init(&bus);
    rec_t life = {0};
    epet_bus_subscribe(&bus, EPET_EV_ALL, rec_fn, &life, "life");
    epet_init(&pet);
    epet_attach_bus(&pet, &bus);
    for (int i = 0; i < 6000; i++) { epet_update(&pet, 50, NONE, NONE); }
    epet_bus_dispatch(&bus);
    CHECK(!pet.alive, "pet should be dead after 5 min");
    CHECK(count_of(&life, EPET_EV_DIED) == 1, "should emit DIED once");

    CHECK(!pet.display_on, "display should be blanked by the time it dies");
    CHECK(!epet_apply_action(&pet, EPET_ACT_FEED),
          "a dead pet must refuse ordinary actions");
    CHECK(epet_apply_action(&pet, EPET_ACT_REVIVE), "REVIVE should succeed");
    epet_bus_dispatch(&bus);
    CHECK(pet.alive, "REVIVE should restart the pet");
    CHECK(pet.bus == &bus, "rebirth must not detach the bus");
    CHECK(count_of(&life, EPET_EV_REBORN) == 1, "should emit REBORN");

    printf("care module\n");

    /* 9. care raises ATTENTION on a fresh need, and rate-limits reminders */
    epet_bus_init(&bus);
    rec_t alerts = {0};
    epet_care_t care;
    epet_care_init(&care, &bus, 120000);         /* at most one per 2 min */
    epet_bus_subscribe(&bus, EPET_EV_MASK(EPET_EV_ATTENTION), rec_fn, &alerts, "alert");
    epet_init(&pet);
    epet_set_species(&pet, epet_species_builtin(0), false);
    epet_attach_bus(&pet, &bus);
    for (int i = 0; i < 1400; i++) {             /* 70 s: gets hungry */
        epet_update(&pet, 50, NONE, NONE);
        epet_bus_dispatch(&bus);
    }
    CHECK(alerts.n >= 1, "a fresh need should alert immediately");
    int after_first = alerts.n;
    CHECK(care.reasons & EPET_EV_MASK(EPET_EV_HUNGRY), "reason should be HUNGRY");
    CHECK(strstr(epet_care_reasons(&care), "HUNGRY") != NULL,
          "reason string should mention HUNGRY, got '%s'", epet_care_reasons(&care));

    /* feeding clears the reason, so no further alerts for it */
    press_awake(&pet, &bus, EPET_ACT_FEED);
    CHECK(!(care.reasons & EPET_EV_MASK(EPET_EV_HUNGRY)),
          "feeding should clear the HUNGRY reason");
    CHECK(alerts.n == after_first, "clearing a need should not alert");

    printf(failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
