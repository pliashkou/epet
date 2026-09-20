#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "epet.h"
#include "epet_ui.h"
#include "epet_dynpage.h"

/* A small stack VM so loaded modules can ship real behaviour.
 *
 * Native code cannot be loaded safely -- no dynamic linker, no memory
 * protection, and a bad pointer takes the whole device down. Bytecode gives
 * modules genuine logic while keeping every access checked:
 *
 *   - no raw memory access; the framebuffer is reachable only through
 *     drawing syscalls that clip
 *   - jumps, locals, globals, string ids and syscall ids are all validated
 *     at load time and bounds-checked again at run time
 *   - every hook runs under an instruction budget, so a loaded page cannot
 *     hang the device with a loop
 *
 * Integers are int32. Strings live in a per-module pool and are referenced
 * by index; bytecode never handles a pointer. */

#define EPET_VM_STACK     64
#define EPET_VM_LOCALS    16
#define EPET_VM_GLOBALS   16
#define EPET_VM_BUDGET    20000   /* instructions per hook call */

typedef enum {
    OP_NOP = 0,
    OP_PUSH_I32, OP_PUSH_I8, OP_DROP, OP_DUP, OP_SWAP,
    OP_LOAD_L, OP_STORE_L, OP_LOAD_G, OP_STORE_G, OP_LOAD_ARG,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_NEG,
    OP_AND, OP_OR, OP_XOR, OP_SHL, OP_SHR,
    OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE, OP_NOT,
    OP_JMP, OP_JZ, OP_JNZ,
    OP_CALL, OP_RET,
    OP_COUNT
} epet_op_t;

/* Syscalls. Argument order is left to right as pushed. */
typedef enum {
    SYS_FILL = 0,     /* (colour)                                  -> 0 */
    SYS_RECT,         /* (x,y,w,h,colour)                          -> 0 */
    SYS_DISC,         /* (cx,cy,r,colour)                          -> 0 */
    SYS_SHADE,        /* (x,y,w,h,colour,alpha)                    -> 0 */
    SYS_TEXT,         /* (x,y,strid,colour,scale)                  -> 0 */
    SYS_TEXTV,        /* (x,y,srcid,colour,scale)  dynamic string  -> 0 */
    SYS_NUMBER,       /* (x,y,value,colour,scale)                  -> 0 */
    SYS_BAR,          /* (x,y,w,h,pct,colour)                      -> 0 */
    SYS_SPRITE,       /* (cx,cy,scale)                             -> 0 */
    SYS_RGB,          /* (r,g,b)                             -> colour  */
    SYS_GET,          /* (srcid)        pet field, x100 for floats      */
    SYS_ACT,          /* (actionid)     feed/play/clean/revive -> 0|1   */
    SYS_EMOTE,        /* (strid)        play a pose                     */
    SYS_CLOSE,        /* ()             close this page                 */
    SYS_RAND,         /* (n)            0..n-1                          */
    SYS_TEXTW,        /* (strid,scale)  -> pixel width                  */
    SYS_LEVEL_COL,    /* (pct)          -> good/warn/bad colour         */
    SYS_NUDGE,        /* (ms)  light the screen for a while -- how a
                       *       background hook asks to be noticed         */
    SYS_EVENT_ARG,    /* (n)   argument n of the event being handled      */
    SYS_COUNT
} epet_sys_t;

typedef struct {
    const uint8_t *code;      /* shared by every page in the module */
    uint32_t       code_len;
    const char    *strings;   /* nul-separated pool */
    uint32_t       str_len;
    const uint16_t *str_off;  /* offset of each string */
    uint16_t       n_strings;
    /* Module-wide, shared by every page AND the background hook, so a page
     * can display what the hook counted while nobody was looking. Cleared
     * once when the module loads, never on page entry. */
    int32_t       *globals;
} epet_vm_image_t;

/* One running page. Globals persist for as long as the page object lives. */
typedef struct {
    epet_page_t     base;                 /* must be first */
    char            title[EPET_DYN_TITLE_MAX];
    uint8_t         icon;
    const epet_vm_image_t *img;
    uint32_t        pc_enter, pc_update, pc_render, pc_leave;  /* 0 = absent */
    /* set while a hook runs, so syscalls can reach the world */
    const epet_t   *pet;
    epet_ui_t      *ui;
    uint16_t       *fb;
    bool            want_close;
    uint32_t        faults;               /* hooks aborted, for diagnostics */
} epet_vmpage_t;

void epet_vmpage_bind(epet_vmpage_t *p);

/* A background handler: bytecode run when a subscribed event fires, whether
 * or not any of the module's pages are open.
 *
 * It runs with no framebuffer, so the drawing syscalls are no-ops; what it
 * CAN do is read the pet, apply actions, play a pose, and ask for the screen
 * with SYS_NUDGE. Hook args are: 0 = event type, 1 = payload a,
 * 2 = payload b, 3 = pet age in seconds. */
typedef struct {
    const epet_vm_image_t *img;
    uint32_t  pc_event;
    uint32_t  mask;            /* which event types */
    uint32_t  faults;
    uint32_t  calls;
} epet_vmhook_t;

/* Subscribes the hook. Uses epet_active() for the pet at dispatch time. */
void epet_vmhook_attach(epet_vmhook_t *h, epet_bus_t *bus);
void epet_vmhook_detach(epet_vmhook_t *h, epet_bus_t *bus);

/* Load-time validation. Returns false and leaves *why pointing at a static
 * reason string if the code could not be proven safe to run. */
bool epet_vm_validate(const epet_vm_image_t *img, uint32_t entry,
                      const char **why);
