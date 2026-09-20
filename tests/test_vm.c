/* The bytecode VM: what it refuses to load, and what it refuses to let a
 * loaded page do. Everything here is about containment -- a module is
 * untrusted code running on a device with no memory protection. */
#include <stdio.h>
#include <string.h>
#include "epet.h"
#include "epet_vm.h"
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

static uint16_t fb[EPET_W * EPET_H];
static const char POOL[] = "HI\0THERE";
static const uint16_t OFFS[] = { 0, 3 };

static epet_vm_image_t img_of(const uint8_t *code, uint32_t len)
{
    epet_vm_image_t i = { code, len, POOL, sizeof POOL, OFFS, 2 };
    return i;
}

/* opcodes, mirroring epet_vm.h */
#define P8(v)   OP_PUSH_I8, (uint8_t)(v)
#define CALL(f,n) OP_CALL, (uint8_t)(f), (uint8_t)(n)

static bool val(const uint8_t *code, uint32_t len, const char **why)
{
    epet_vm_image_t i = img_of(code, len);
    return epet_vm_validate(&i, 1, why);   /* entry 1: offset 0 is reserved */
}

int main(void)
{
    const char *why = 0;

    printf("validator accepts good code\n");
    {
        /* nop; push 1; fill; ret */
        const uint8_t ok[] = { OP_NOP, P8(1), CALL(SYS_FILL, 1), OP_DROP, OP_RET };
        CHECK(val(ok, sizeof ok, &why), "should accept, said '%s'", why);
    }

    printf("validator rejects unsafe code\n");
    {
        const uint8_t bad_op[] = { OP_NOP, 200, OP_RET };
        CHECK(!val(bad_op, sizeof bad_op, &why), "must reject an unknown opcode");

        /* This is the mistake that actually happened: a syscall whose
         * arguments were never pushed. It loaded fine and silently stopped
         * drawing halfway down the page. */
        const uint8_t no_args[] = { OP_NOP, CALL(SYS_FILL, 1), OP_DROP, OP_RET };
        CHECK(!val(no_args, sizeof no_args, &why),
              "must reject a syscall with no arguments pushed");
        CHECK(why && strstr(why, "argument") != NULL,
              "the reason should name the problem, got '%s'", why);

        const uint8_t bad_arity[] = { OP_NOP, P8(1), CALL(SYS_FILL, 3), OP_DROP, OP_RET };
        CHECK(!val(bad_arity, sizeof bad_arity, &why), "must reject wrong arity");

        const uint8_t bad_sys[] = { OP_NOP, P8(1), CALL(200, 1), OP_DROP, OP_RET };
        CHECK(!val(bad_sys, sizeof bad_sys, &why), "must reject an unknown syscall");

        const uint8_t bad_local[] = { OP_NOP, OP_LOAD_L, 99, OP_RET };
        CHECK(!val(bad_local, sizeof bad_local, &why), "must reject a bad local");

        const uint8_t bad_global[] = { OP_NOP, OP_STORE_G, 99, OP_RET };
        CHECK(!val(bad_global, sizeof bad_global, &why), "must reject a bad global");

        const uint8_t bad_arg[] = { OP_NOP, OP_LOAD_ARG, 9, OP_RET };
        CHECK(!val(bad_arg, sizeof bad_arg, &why), "must reject a bad hook arg");

        /* jump past the end of the code */
        const uint8_t far_jump[] = { OP_NOP, OP_JMP, 0x7F, 0x7F, OP_RET };
        CHECK(!val(far_jump, sizeof far_jump, &why), "must reject a wild jump");

        /* jump backwards out of the code */
        const uint8_t back_jump[] = { OP_NOP, OP_JMP, 0x00, 0x80, OP_RET };
        CHECK(!val(back_jump, sizeof back_jump, &why), "must reject a negative wild jump");

        /* truncated operand at the end of the buffer */
        const uint8_t truncated[] = { OP_NOP, OP_PUSH_I32, 1, 2 };
        CHECK(!val(truncated, sizeof truncated, &why), "must reject a truncated instruction");

        /* popping from an empty stack */
        const uint8_t underflow[] = { OP_NOP, OP_ADD, OP_RET };
        CHECK(!val(underflow, sizeof underflow, &why), "must reject stack underflow");

        /* a conditional jump with nothing to test */
        const uint8_t empty_jz[] = { OP_NOP, OP_JZ, 0x01, 0x00, OP_RET, OP_RET };
        CHECK(!val(empty_jz, sizeof empty_jz, &why), "must reject jz with an empty stack");

        /* growing the stack without bound */
        static uint8_t deep[256];
        deep[0] = OP_NOP;
        for (int i = 0; i < 120; i++) { deep[1 + i*2] = OP_PUSH_I8; deep[2 + i*2] = 1; }
        deep[241] = OP_RET;
        CHECK(!val(deep, 242, &why), "must reject code that overruns the stack");

        /* entry point outside the code */
        epet_vm_image_t i = img_of(deep, 242);
        CHECK(!epet_vm_validate(&i, 9999, &why), "must reject a wild entry point");
    }

    printf("runaway code cannot hang the device\n");
    {
        /* jmp to itself: validates fine, must still terminate when run */
        const uint8_t spin[] = { OP_NOP, OP_JMP, 0xFD, 0xFF, OP_RET };
        CHECK(val(spin, sizeof spin, &why), "an infinite loop is not itself unsafe");

        epet_vm_image_t i = img_of(spin, sizeof spin);
        epet_vmpage_t p;
        memset(&p, 0, sizeof p);
        snprintf(p.title, sizeof p.title, "SPIN");
        p.img = &i;
        p.pc_render = 1;
        epet_vmpage_bind(&p);

        epet_t pet; epet_init(&pet);
        /* If the budget did not stop it, this call would never return. */
        p.base.render(&p.base, &pet, fb);
        CHECK(p.faults == 1, "the budget should have aborted it, faults=%u", p.faults);
    }

    printf("a page that runs\n");
    {
        /* nop; push colour; fill; drop; push 1; ret */
        uint8_t prog[] = {
            OP_NOP,
            OP_PUSH_I32, 0xFF, 0xFF, 0x00, 0x00,   /* 0xFFFF = white */
            CALL(SYS_FILL, 1), OP_DROP,
            P8(1), OP_RET,
        };
        epet_vm_image_t i = img_of(prog, sizeof prog);
        CHECK(epet_vm_validate(&i, 1, &why), "should validate, said '%s'", why);

        epet_vmpage_t p;
        memset(&p, 0, sizeof p);
        snprintf(p.title, sizeof p.title, "FILL");
        p.img = &i;
        p.pc_render = 1;
        epet_vmpage_bind(&p);

        epet_t pet; epet_init(&pet);
        memset(fb, 0, sizeof fb);
        p.base.render(&p.base, &pet, fb);
        CHECK(p.faults == 0, "should not fault");
        CHECK(fb[0] == 0xFFFF && fb[EPET_W * EPET_H - 1] == 0xFFFF,
              "the page should have filled the screen, got %04X", fb[0]);
    }

    printf("hooks and absence\n");
    {
        const uint8_t prog[] = { OP_NOP, P8(0), OP_RET };   /* update returns 0 */
        epet_vm_image_t i = img_of(prog, sizeof prog);
        epet_vmpage_t p;
        memset(&p, 0, sizeof p);
        snprintf(p.title, sizeof p.title, "X");
        p.img = &i;
        p.pc_update = 1;
        epet_vmpage_bind(&p);

        CHECK(p.base.update != NULL, "update should be bound");
        CHECK(p.base.render == NULL,
              "an entry point of 0 means the hook is absent, so render must "
              "not be bound");

        epet_t pet; epet_init(&pet);
        epet_bus_t bus; epet_bus_init(&bus);
        epet_ui_t ui; epet_ui_init(&ui, &bus);
        bool none[EPET_BTN_COUNT] = {false};
        CHECK(!p.base.update(&p.base, &ui, &pet, 50, none),
              "returning 0 from update should close the page");
    }

    printf("state persists across calls\n");
    {
        /* nop; loadg 0; push 1; add; dup; storeg 0; ret */
        const uint8_t prog[] = {
            OP_NOP, OP_LOAD_G, 0, P8(1), OP_ADD, OP_DUP, OP_STORE_G, 0, OP_RET
        };
        /* Globals are module-wide now, shared by pages and the background
         * hook, so the image owns the storage. */
        static int32_t globals[EPET_VM_GLOBALS];
        memset(globals, 0, sizeof globals);
        epet_vm_image_t i = img_of(prog, sizeof prog);
        i.globals = globals;
        CHECK(epet_vm_validate(&i, 1, &why), "should validate, said '%s'", why);

        epet_vmpage_t p;
        memset(&p, 0, sizeof p);
        snprintf(p.title, sizeof p.title, "CNT");
        p.img = &i;
        p.pc_update = 1;
        epet_vmpage_bind(&p);

        epet_t pet; epet_init(&pet);
        epet_bus_t bus; epet_bus_init(&bus);
        epet_ui_t ui; epet_ui_init(&ui, &bus);
        bool none[EPET_BTN_COUNT] = {false};
        for (int k = 0; k < 5; k++) p.base.update(&p.base, &ui, &pet, 10, none);
        CHECK(globals[0] == 5, "globals should persist across calls, got %d",
              globals[0]);
        CHECK(p.faults == 0, "no faults");

        /* Entering a page must NOT clear them: a background hook may have
         * been counting while the page was closed. */
        if (p.base.enter) p.base.enter(&p.base, &ui, &pet);
        CHECK(globals[0] == 5,
              "opening a page must not wipe module state, got %d", globals[0]);
    }

    printf("background hooks\n");
    {
        /* count events into g0, and ask for the screen on a DIED */
        const uint8_t prog[] = {
            OP_NOP,
            OP_LOAD_G, 0, P8(1), OP_ADD, OP_STORE_G, 0,
            P8(1), OP_RET,
        };
        static int32_t globals[EPET_VM_GLOBALS];
        memset(globals, 0, sizeof globals);
        epet_vm_image_t i = img_of(prog, sizeof prog);
        i.globals = globals;
        CHECK(epet_vm_validate(&i, 1, &why), "hook should validate, said '%s'", why);

        epet_vmhook_t h = { .img = &i, .pc_event = 1,
                            .mask = EPET_EV_MASK(EPET_EV_POOPED) };
        epet_bus_t bus; epet_bus_init(&bus);
        epet_t pet; epet_init(&pet);
        epet_set_active(&pet);

        epet_vmhook_attach(&h, &bus);
        CHECK(bus.n_sub == 1, "attach should subscribe, got %d", bus.n_sub);

        epet_bus_publish(&bus, EPET_EV_POOPED, 0, 1, 0);
        epet_bus_publish(&bus, EPET_EV_FED, 0, 0, 0);   /* not subscribed */
        epet_bus_dispatch(&bus);
        CHECK(h.calls == 1, "only the subscribed event should run it, %u", h.calls);
        CHECK(globals[0] == 1, "the hook should have counted, got %d", globals[0]);
        CHECK(h.faults == 0, "no faults");

        /* A hook runs with no framebuffer, so drawing syscalls must be
         * harmless rather than writing somewhere arbitrary. */
        const uint8_t draws[] = {
            OP_NOP, OP_PUSH_I32, 0xFF, 0xFF, 0, 0,
            CALL(SYS_FILL, 1), OP_DROP, P8(1), OP_RET,
        };
        epet_vm_image_t di = img_of(draws, sizeof draws);
        di.globals = globals;
        epet_vmhook_t dh = { .img = &di, .pc_event = 1, .mask = EPET_EV_ALL };
        epet_bus_t b2; epet_bus_init(&b2);
        epet_vmhook_attach(&dh, &b2);
        memset(fb, 0, sizeof fb);
        epet_bus_publish(&b2, EPET_EV_MINUTE, 0, 0, 0);
        epet_bus_dispatch(&b2);
        CHECK(dh.calls == 1, "the drawing hook should have run");
        CHECK(fb[0] == 0 && fb[100] == 0,
              "a hook with no framebuffer must not paint anything");

        /* detach removes it */
        epet_vmhook_detach(&h, &bus);
        CHECK(bus.n_sub == 0, "detach should unsubscribe, got %d", bus.n_sub);
        uint32_t before = h.calls;
        epet_bus_publish(&bus, EPET_EV_POOPED, 0, 1, 0);
        epet_bus_dispatch(&bus);
        CHECK(h.calls == before, "a detached hook must not run");
    }

    printf(failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
