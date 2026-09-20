#include "epet_vm.h"
#include "epet_draw.h"
#include "epet_dynpage.h"
#include "epet_species.h"
#include <string.h>
#ifdef EPET_VM_TRACE
#include <stdio.h>
#endif

/* ---- operand sizes, used by both the validator and the interpreter ---- */

static int operand_bytes(uint8_t op)
{
    switch (op) {
    case OP_PUSH_I32:                       return 4;
    case OP_PUSH_I8:  case OP_LOAD_L: case OP_STORE_L:
    case OP_LOAD_G:   case OP_STORE_G: case OP_LOAD_ARG:
                                            return 1;
    case OP_JMP: case OP_JZ: case OP_JNZ:   return 2;
    case OP_CALL:                           return 2;   /* fn, argc */
    default:                                return 0;
    }
}

static int32_t rd_i32(const uint8_t *p)
{
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}
static int16_t rd_i16(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* Arguments each syscall consumes. -1 marks an unused slot. */
static const int8_t SYS_ARGC[SYS_COUNT] = {
    [SYS_FILL] = 1, [SYS_RECT] = 5, [SYS_DISC] = 4, [SYS_SHADE] = 6,
    [SYS_TEXT] = 5, [SYS_TEXTV] = 5, [SYS_NUMBER] = 5, [SYS_BAR] = 6,
    [SYS_SPRITE] = 3, [SYS_RGB] = 3, [SYS_GET] = 1, [SYS_ACT] = 1,
    [SYS_EMOTE] = 1, [SYS_CLOSE] = 0, [SYS_RAND] = 1, [SYS_TEXTW] = 2,
    [SYS_LEVEL_COL] = 1, [SYS_NUDGE] = 1, [SYS_EVENT_ARG] = 1,
};

/* ---- validation -------------------------------------------------------
 * Walks every reachable instruction once, checking that it decodes, that
 * jumps land on an instruction boundary inside the code, and that every
 * index is in range. Code that fails this is never run. */

bool epet_vm_validate(const epet_vm_image_t *img, uint32_t entry,
                      const char **why)
{
    static const char *ok = "ok";
    *why = ok;
    if (!img || !img->code) { *why = "no code"; return false; }
    if (entry >= img->code_len) { *why = "entry out of range"; return false; }

    /* Mark instruction starts so a jump into the middle of an operand is
     * rejected, and track the operand-stack depth at each one. Depth
     * checking is what catches the commonest authoring mistake: calling a
     * syscall without pushing its arguments. Without it such code loads
     * fine and then silently stops drawing halfway down the page. */
    static uint8_t seen[4096];
    static int16_t depth[4096];
    if (img->code_len > sizeof seen) { *why = "code too long"; return false; }
    memset(seen, 0, img->code_len);

    struct { uint32_t pc; int16_t d; } stack[64];
    uint8_t  sp = 0;
    stack[sp].pc = entry; stack[sp].d = 0; sp++;

    while (sp) {
        sp--;
        uint32_t pc = stack[sp].pc;
        int16_t  d  = stack[sp].d;
        while (pc < img->code_len) {
            if (seen[pc]) {
                if (depth[pc] != d) { *why = "inconsistent stack depth"; return false; }
                break;                          /* already walked from here */
            }
            seen[pc] = 1;
            depth[pc] = d;

            uint8_t op = img->code[pc];
            if (op >= OP_COUNT) { *why = "bad opcode"; return false; }
            int n = operand_bytes(op);
            if (pc + 1 + (uint32_t)n > img->code_len) {
                *why = "truncated instruction"; return false;
            }
            const uint8_t *a = img->code + pc + 1;
            uint32_t next = pc + 1 + (uint32_t)n;

            int16_t delta = 0;
            switch (op) {
            case OP_LOAD_L: case OP_STORE_L:
                if (a[0] >= EPET_VM_LOCALS) { *why = "local out of range"; return false; }
                delta = (op == OP_LOAD_L) ? 1 : -1;
                break;
            case OP_LOAD_G: case OP_STORE_G:
                if (a[0] >= EPET_VM_GLOBALS) { *why = "global out of range"; return false; }
                delta = (op == OP_LOAD_G) ? 1 : -1;
                break;
            case OP_LOAD_ARG:
                if (a[0] >= 4) { *why = "arg out of range"; return false; }
                delta = 1;
                break;
            case OP_PUSH_I32: case OP_PUSH_I8: case OP_DUP:
                delta = 1;
                break;
            case OP_DROP: case OP_STORE_L + 100:   /* unreachable, keeps gcc quiet */
                delta = -1;
                break;
            case OP_SWAP: case OP_NEG: case OP_NOT: case OP_NOP:
                delta = 0;
                break;
            case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
            case OP_AND: case OP_OR:  case OP_XOR: case OP_SHL: case OP_SHR:
            case OP_EQ:  case OP_NE:  case OP_LT:  case OP_LE:
            case OP_GT:  case OP_GE:
                delta = -1;
                break;
            case OP_CALL: {
                if (a[0] >= SYS_COUNT) { *why = "bad syscall"; return false; }
                if (SYS_ARGC[a[0]] != a[1]) { *why = "syscall arity"; return false; }
                if (d < a[1]) { *why = "syscall called without its arguments"; return false; }
                delta = (int16_t)(1 - a[1]);
                break;
            }
            case OP_JMP: case OP_JZ: case OP_JNZ: {
                int32_t t = (int32_t)next + rd_i16(a);
                if (t < 0 || (uint32_t)t >= img->code_len) {
                    *why = "jump out of range"; return false;
                }
                if (op != OP_JMP) {
                    if (d < 1) { *why = "conditional jump with empty stack"; return false; }
                    d--;
                }
                if (sp >= 64) { *why = "control flow too complex"; return false; }
                stack[sp].pc = (uint32_t)t; stack[sp].d = d; sp++;
                if (op == OP_JMP) { pc = (uint32_t)t; goto walked; }
                delta = 0;
                break;
            }
            case OP_RET:
                goto walked;
            default:
                break;
            }

            if (op == OP_DROP) delta = -1;
            d = (int16_t)(d + delta);
            if (d < 0)                { *why = "stack underflow"; return false; }
            if (d > EPET_VM_STACK - 8){ *why = "stack too deep"; return false; }
            pc = next;
        }
    walked: ;
    }
    return true;
}

/* ---- interpreter ------------------------------------------------------ */

typedef struct {
    /* Either a page is running (pg set, framebuffer maybe set) or a
     * background hook is (pg NULL). Syscalls that need a screen check fb. */
    epet_vmpage_t *pg;
    const epet_vm_image_t *img;
    const epet_t  *pet;
    uint16_t      *fb;
    int32_t        ev[4];
    int32_t  st[EPET_VM_STACK];
    uint8_t  sp;
    int32_t  loc[EPET_VM_LOCALS];
    int32_t  arg[4];
    bool     fault;
} vm_t;

static void push(vm_t *v, int32_t x)
{
    if (v->sp >= EPET_VM_STACK) { v->fault = true; return; }
    v->st[v->sp++] = x;
}
static int32_t pop(vm_t *v)
{
    if (v->sp == 0) { v->fault = true; return 0; }
    return v->st[--v->sp];
}

static const char *str_of(const epet_vm_image_t *img, int32_t id)
{
    if (id < 0 || id >= img->n_strings) return "";
    uint16_t off = img->str_off[id];
    if (off >= img->str_len) return "";
    return img->strings + off;
}

static int32_t syscall(vm_t *v, uint8_t fn, int32_t *a)
{
    uint16_t *fb = v->fb;
    const epet_t *pet = v->pet;

    switch (fn) {
    case SYS_FILL:   if (fb) epet_fill(fb, (uint16_t)a[0]); return 0;
    case SYS_RECT:   if (fb) epet_rect(fb, a[0], a[1], a[2], a[3], (uint16_t)a[4]); return 0;
    case SYS_DISC:   if (fb) epet_disc(fb, a[0], a[1], a[2], (uint16_t)a[3]); return 0;
    case SYS_SHADE:  if (fb) epet_shade(fb, a[0], a[1], a[2], a[3],
                                        (uint16_t)a[4], (uint8_t)a[5]); return 0;
    case SYS_TEXT:   if (fb) epet_text(fb, a[0], a[1], str_of(v->img, a[2]),
                                       (uint16_t)a[3], a[4] < 1 ? 1 : a[4]); return 0;
    case SYS_TEXTV:  if (fb) epet_text(fb, a[0], a[1],
                                       epet_src_string((epet_src_t)a[2], pet),
                                       (uint16_t)a[3], a[4] < 1 ? 1 : a[4]); return 0;
    case SYS_NUMBER: if (fb) epet_number(fb, a[0], a[1], a[2],
                                         (uint16_t)a[3], a[4] < 1 ? 1 : a[4]); return 0;
    case SYS_BAR:    if (fb) epet_bar(fb, a[0], a[1], a[2], a[3],
                                      (float)a[4], (uint16_t)a[5]); return 0;
    case SYS_SPRITE: if (fb && pet) epet_actor_draw_scaled(&pet->actor, fb,
                                        a[0], a[1], a[2] < 1 ? 1 : a[2]); return 0;
    case SYS_RGB:    return epet_mix(0, EPET_RGB565((uint8_t)a[0], (uint8_t)a[1],
                                                    (uint8_t)a[2]), 255);
    case SYS_LEVEL_COL: return epet_level_colour((float)a[0]);
    case SYS_TEXTW:  return epet_text_width(str_of(v->img, a[0]),
                                            a[1] < 1 ? 1 : a[1]);
    case SYS_GET: {
        float f = 0;
        if (pet && epet_src_number((epet_src_t)a[0], pet, &f)) return (int32_t)f;
        return 0;
    }
    case SYS_ACT:
        /* Actions mutate the pet, so the const is deliberate everywhere else
         * and dropped only here, where a syscall is meant to have effect. */
        if (pet && a[0] > 0 && a[0] < EPET_ACT_COUNT) {
            return epet_apply_action((epet_t *)pet, (epet_action_t)a[0]) ? 1 : 0;
        }
        return 0;
    case SYS_EMOTE:
        if (pet) epet_pet_emote((epet_t *)pet, str_of(v->img, a[0]));
        return 0;
    case SYS_CLOSE:  if (v->pg) v->pg->want_close = true; return 0;
    case SYS_NUDGE:
        /* How a background hook asks to be seen. */
        if (pet) epet_display_nudge((epet_t *)pet, (uint32_t)a[0]);
        return 0;
    case SYS_EVENT_ARG:
        return (a[0] >= 0 && a[0] < 4) ? v->ev[a[0]] : 0;
    case SYS_RAND:   return a[0] > 0 ? (int32_t)epet_random_below((uint32_t)a[0]) : 0;
    default: return 0;
    }
}

static int32_t run_image(const epet_vm_image_t *img, uint32_t pc,
                         epet_vmpage_t *pg, const epet_t *pet, uint16_t *fb,
                         const int32_t *args, int n_args, uint32_t *faults)
{
    if (!img || pc == 0) return 1;                /* absent hook: succeed */

    vm_t v;
    memset(&v, 0, sizeof v);
    v.pg = pg;
    v.img = img;
    v.pet = pet;
    v.fb = fb;
    for (int i = 0; i < 4; i++) {
        v.arg[i] = i < n_args ? args[i] : 0;
        v.ev[i]  = v.arg[i];
    }
    uint32_t budget = EPET_VM_BUDGET;
    bool     exhausted = false;
    int32_t  result = 1;

    while (pc < img->code_len) {
        /* Checked before decrementing: `budget--` in the loop condition
         * wraps to UINT32_MAX on the last iteration, so the exhaustion test
         * afterwards never fired and a runaway page recorded no fault. */
        if (budget == 0) { exhausted = true; break; }
        budget--;

        uint8_t op = img->code[pc];
        const uint8_t *a = img->code + pc + 1;
        uint32_t next = pc + 1 + (uint32_t)operand_bytes(op);

        switch (op) {
        case OP_NOP: break;
        case OP_PUSH_I32: push(&v, rd_i32(a)); break;
        case OP_PUSH_I8:  push(&v, (int8_t)a[0]); break;
        case OP_DROP: pop(&v); break;
        case OP_DUP:  { int32_t x = pop(&v); push(&v, x); push(&v, x); break; }
        case OP_SWAP: { int32_t b = pop(&v), c = pop(&v); push(&v, b); push(&v, c); break; }
        case OP_LOAD_L:  push(&v, v.loc[a[0]]); break;
        case OP_STORE_L: v.loc[a[0]] = pop(&v); break;
        case OP_LOAD_G:  push(&v, img->globals ? img->globals[a[0]] : 0); break;
        case OP_STORE_G: { int32_t g = pop(&v);
                           if (img->globals) img->globals[a[0]] = g; } break;
        case OP_LOAD_ARG: push(&v, v.arg[a[0]]); break;

        case OP_ADD: { int32_t y = pop(&v); push(&v, pop(&v) + y); break; }
        case OP_SUB: { int32_t y = pop(&v); push(&v, pop(&v) - y); break; }
        case OP_MUL: { int32_t y = pop(&v); push(&v, pop(&v) * y); break; }
        case OP_DIV: { int32_t y = pop(&v); int32_t x = pop(&v);
                       push(&v, y ? x / y : 0); break; }      /* no div by zero trap */
        case OP_MOD: { int32_t y = pop(&v); int32_t x = pop(&v);
                       push(&v, y ? x % y : 0); break; }
        case OP_NEG: push(&v, -pop(&v)); break;
        case OP_AND: { int32_t y = pop(&v); push(&v, pop(&v) & y); break; }
        case OP_OR:  { int32_t y = pop(&v); push(&v, pop(&v) | y); break; }
        case OP_XOR: { int32_t y = pop(&v); push(&v, pop(&v) ^ y); break; }
        case OP_SHL: { int32_t y = pop(&v) & 31; push(&v, (int32_t)((uint32_t)pop(&v) << y)); break; }
        case OP_SHR: { int32_t y = pop(&v) & 31; push(&v, (int32_t)((uint32_t)pop(&v) >> y)); break; }
        case OP_EQ:  { int32_t y = pop(&v); push(&v, pop(&v) == y); break; }
        case OP_NE:  { int32_t y = pop(&v); push(&v, pop(&v) != y); break; }
        case OP_LT:  { int32_t y = pop(&v); push(&v, pop(&v) <  y); break; }
        case OP_LE:  { int32_t y = pop(&v); push(&v, pop(&v) <= y); break; }
        case OP_GT:  { int32_t y = pop(&v); push(&v, pop(&v) >  y); break; }
        case OP_GE:  { int32_t y = pop(&v); push(&v, pop(&v) >= y); break; }
        case OP_NOT: push(&v, !pop(&v)); break;

        case OP_JMP: pc = (uint32_t)((int32_t)next + rd_i16(a)); continue;
        case OP_JZ:  if (!pop(&v)) { pc = (uint32_t)((int32_t)next + rd_i16(a)); continue; } break;
        case OP_JNZ: if ( pop(&v)) { pc = (uint32_t)((int32_t)next + rd_i16(a)); continue; } break;

        case OP_CALL: {
            uint8_t fn = a[0], argc = a[1];
            int32_t args2[8] = {0};
            if (argc > 8) { v.fault = true; break; }
            for (int i = argc - 1; i >= 0; i--) args2[i] = pop(&v);
            if (v.fault) break;
            push(&v, syscall(&v, fn, args2));
            break;
        }

        case OP_RET:
            result = v.sp ? pop(&v) : 1;
            goto done;

        default:
            v.fault = true;
            break;
        }

        if (v.fault) break;
        pc = next;
    }

done:
#ifdef EPET_VM_TRACE
    if (v.fault || exhausted) {
        fprintf(stderr, "[vm] fault at pc=%u op=%u sp=%u budget=%u\n",
                pc, pc < img->code_len ? img->code[pc] : 999, v.sp, budget);
    }
#endif
    if (v.fault || exhausted) {
        if (faults) (*faults)++;
        return 1;      /* a misbehaving hook must not also close the page */
    }
    return result;
}

static int32_t run(epet_vmpage_t *pg, uint32_t pc, const int32_t *args, int n)
{
    return run_image(pg->img, pc, pg, pg->pet, pg->fb, args, n, &pg->faults);
}

/* ---- epet_page_t hooks ------------------------------------------------ */

static void vm_enter(epet_page_t *self, epet_ui_t *ui, epet_t *pet)
{
    epet_vmpage_t *p = (epet_vmpage_t *)self;
    /* Globals are module-wide and deliberately NOT cleared here: a
     * background hook may have been counting while this page was closed. */
    p->want_close = false;
    p->pet = pet; p->ui = ui; p->fb = 0;
    run(p, p->pc_enter, 0, 0);
}

static bool vm_update(epet_page_t *self, epet_ui_t *ui, epet_t *pet,
                      uint32_t dt_ms, const bool edge[EPET_BTN_COUNT])
{
    epet_vmpage_t *p = (epet_vmpage_t *)self;
    int32_t mask = 0;
    for (int i = 0; i < EPET_BTN_COUNT; i++) if (edge[i]) mask |= 1 << i;

    p->pet = pet; p->ui = ui; p->fb = 0;
    int32_t args[2] = { (int32_t)dt_ms, mask };
    int32_t keep = run(p, p->pc_update, args, 2);
    return !p->want_close && keep != 0;
}

static void vm_render(epet_page_t *self, const epet_t *pet, uint16_t *fb)
{
    epet_vmpage_t *p = (epet_vmpage_t *)self;
    p->pet = pet; p->fb = fb;
    run(p, p->pc_render, 0, 0);
    p->fb = 0;
}

static void vm_leave(epet_page_t *self, epet_ui_t *ui, epet_t *pet)
{
    epet_vmpage_t *p = (epet_vmpage_t *)self;
    p->pet = pet; p->ui = ui; p->fb = 0;
    run(p, p->pc_leave, 0, 0);
}

static void vm_icon(epet_page_t *self, uint16_t *fb, int cx, int cy,
                    uint16_t tint, bool sel)
{
    (void)sel;
    epet_vmpage_t *p = (epet_vmpage_t *)self;
    /* Loaded pages choose from the same built-in icon set the declarative
     * pages use; drawing an icon in bytecode every frame is not worth it. */
    switch (p->icon) {
    case 1:
        epet_rect(fb, cx - 11, cy - 8, 22, 16, tint);
        epet_shade(fb, cx - 9, cy - 6, 18, 12, EPET_C_BLACK, 140);
        break;
    case 2:
        epet_disc(fb, cx - 2, cy - 2, 8, tint);
        epet_shade(fb, cx - 7, cy - 7, 10, 10, EPET_C_BLACK, 130);
        epet_rect(fb, cx + 3, cy + 3, 7, 3, tint);
        break;
    case 3:
        for (int i = -8; i <= 8; i++) {
            int h = 8 - (i < 0 ? -i : i);
            epet_rect(fb, cx + i, cy - h, 1, h * 2, tint);
        }
        epet_rect(fb, cx - 9, cy - 1, 19, 3, tint);
        break;
    default:
        epet_rect(fb, cx - 7, cy - 6, 14, 10, tint);
        epet_rect(fb, cx - 4, cy - 10, 3, 5, tint);
        epet_rect(fb, cx + 2, cy - 10, 3, 5, tint);
        epet_rect(fb, cx - 2, cy + 4, 5, 6, tint);
        break;
    }
}

void epet_vmpage_bind(epet_vmpage_t *p)
{
    p->base.title  = p->title;
    p->base.enter  = p->pc_enter  ? vm_enter  : 0;
    p->base.update = p->pc_update ? vm_update : 0;
    p->base.render = p->pc_render ? vm_render : 0;
    p->base.leave  = p->pc_leave  ? vm_leave  : 0;
    p->base.icon   = vm_icon;
    p->base.ctx    = 0;
}

/* ---- background hooks ------------------------------------------------- */

static void hook_dispatch(const epet_event_t *ev, void *ctx)
{
    epet_vmhook_t *h = ctx;
    if (!h->img || !h->pc_event) return;

    epet_t *pet = epet_active();
    int32_t args[4] = {
        (int32_t)ev->type, ev->a, ev->b, (int32_t)(ev->age_ms / 1000u),
    };
    h->calls++;
    /* No framebuffer: drawing syscalls become no-ops rather than writing
     * into whatever happens to be on screen. */
    run_image(h->img, h->pc_event, 0, pet, 0, args, 4, &h->faults);
}

void epet_vmhook_attach(epet_vmhook_t *h, epet_bus_t *bus)
{
    if (!h || !bus || !h->pc_event) return;
    epet_bus_subscribe(bus, h->mask ? h->mask : EPET_EV_ALL,
                       hook_dispatch, h, "module");
}

void epet_vmhook_detach(epet_vmhook_t *h, epet_bus_t *bus)
{
    if (h && bus) epet_bus_unsubscribe(bus, h);
}
