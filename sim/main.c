/* macOS host simulator for epet.
 *
 *   ./epet_sim                    window with clickable buttons
 *   ./epet_sim --speed 4          run the clock faster
 *   ./epet_sim --timeout 30000    display blank timeout (0 = never)
 *   ./epet_sim --menu-hide 5000   menu overlay fade-out delay (0 = never)
 *   ./epet_sim --save-dir DIR     persist the pet and module list there
 *   ./epet_sim --headless --shots 0,5000,30000 --out dir
 *
 * Runs the same epet_core as the firmware. Only the framebuffer destination
 * and the button source differ. The window draws a mock device: the 240x240
 * panel in the middle with two buttons down each side, clickable with the
 * mouse or driven from the keyboard.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <errno.h>
#include <SDL.h>
#include "epet.h"
#include "epet_draw.h"
#include "epet_ui.h"
#include "epet_pages.h"
#include "epet_care.h"
#include "epet_module.h"
#include "epet_save.h"
#include "epet_store.h"
#include "epet_link.h"
#include "epet_modblob.h"
#include <unistd.h>
#include <fcntl.h>

#define SCALE     3
#define BEZEL_W   46                      /* button column width, 1x units */
#define CHROME_W  (EPET_W + 2 * BEZEL_W)
#define CHROME_H  EPET_H
#define WIN_W     (CHROME_W * SCALE)
#define WIN_H     (CHROME_H * SCALE)
#define SCREEN_X  BEZEL_W

#define STEP_MS   33

static uint16_t fb[EPET_W * EPET_H];        /* the pet's panel, panel-ready */
static uint16_t fb_native[EPET_W * EPET_H]; /* byte-swapped for SDL/BMP */

/* The core stores colours in the ST7789's byte order so the firmware never
 * reorders anything. Desktops want native order, so the swap happens here --
 * one pass over 57600 pixels, microseconds on a host CPU, against 9.5 ms on
 * the MCU. */
static void to_native(void)
{
    for (int i = 0; i < EPET_W * EPET_H; i++) {
        fb_native[i] = (uint16_t)((fb[i] >> 8) | (fb[i] << 8));
    }
}
static uint16_t chrome[CHROME_W * CHROME_H];/* whole mock device */

/* ---- chrome drawing (own stride, so epet_draw's helpers do not apply) -- */

static void ch_rect(int x, int y, int w, int h, uint16_t c)
{
    for (int row = y; row < y + h; row++) {
        if (row < 0 || row >= CHROME_H) continue;
        for (int col = x; col < x + w; col++) {
            if (col < 0 || col >= CHROME_W) continue;
            chrome[row * CHROME_W + col] = c;
        }
    }
}

static void ch_text(int x, int y, const char *s, uint16_t c, int scale)
{
    for (; *s; s++, x += 6 * scale) {
        const uint8_t *g = epet_font_glyph(*s);
        if (!g) continue;
        for (int col = 0; col < 5; col++) {
            for (int row = 0; row < 7; row++) {
                if (g[col] & (1 << row)) {
                    ch_rect(x + col * scale, y + row * scale, scale, scale, c);
                }
            }
        }
    }
}

/* ---- buttons ---------------------------------------------------------- */

typedef struct { int x, y, w, h; const char *label, *role; } btn_box_t;

static const btn_box_t BOXES[EPET_BTN_COUNT] = {
    [EPET_BTN_LT] = { 4,  40, BEZEL_W - 8, 58, "LT", "UP"   },
    [EPET_BTN_LB] = { 4, 142, BEZEL_W - 8, 58, "LB", "DOWN" },
    [EPET_BTN_RT] = { EPET_W + BEZEL_W + 4,  40, BEZEL_W - 8, 58, "RT", "SEL"  },
    [EPET_BTN_RB] = { EPET_W + BEZEL_W + 4, 142, BEZEL_W - 8, 58, "RB", "BACK" },
};

static bool box_hit(const btn_box_t *b, int x, int y)
{
    return x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h;
}

static void draw_chrome(const bool down[EPET_BTN_COUNT], bool panel_lit)
{
    for (int i = 0; i < CHROME_W * CHROME_H; i++) chrome[i] = EPET_RGB565(30, 33, 40);

    /* panel recess */
    ch_rect(SCREEN_X - 2, -2, EPET_W + 4, EPET_H + 4, EPET_RGB565(10, 12, 16));

    for (int i = 0; i < EPET_BTN_COUNT; i++) {
        const btn_box_t *b = &BOXES[i];
        uint16_t face = down[i] ? EPET_RGB565(90, 150, 220) : EPET_RGB565(62, 68, 82);
        ch_rect(b->x, b->y, b->w, b->h, EPET_RGB565(16, 18, 24));
        ch_rect(b->x + 2, b->y + 2, b->w - 4, b->h - 4, face);
        if (!down[i]) {   /* subtle top highlight so it reads as a key */
            ch_rect(b->x + 2, b->y + 2, b->w - 4, 3, EPET_RGB565(96, 104, 122));
        }
        int lw = strlen(b->label) * 6 * 2;
        ch_text(b->x + (b->w - lw) / 2, b->y + 14, b->label, EPET_C_WHITE, 2);
        int rw = strlen(b->role) * 6;
        ch_text(b->x + (b->w - rw) / 2, b->y + 36, b->role,
                down[i] ? EPET_C_WHITE : EPET_RGB565(150, 160, 180), 1);
    }

    /* blit the pet panel into the recess */
    to_native();
    for (int y = 0; y < EPET_H; y++) {
        for (int x = 0; x < EPET_W; x++) {
            chrome[y * CHROME_W + SCREEN_X + x] =
                panel_lit ? fb_native[y * EPET_W + x] : 0;
        }
    }
}

/* ---- BMP writer (headless) -------------------------------------------- */

static int save_bmp(const char *path, const uint16_t *src, int w, int h, int stride)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    const int row_raw = w * 3, pad = (4 - (row_raw % 4)) % 4;
    const int img = (row_raw + pad) * h, total = 54 + img;
    uint8_t hdr[54] = {0};
    hdr[0]='B'; hdr[1]='M';
    hdr[2]=total&0xFF; hdr[3]=(total>>8)&0xFF; hdr[4]=(total>>16)&0xFF; hdr[5]=(total>>24)&0xFF;
    hdr[10]=54; hdr[14]=40;
    hdr[18]=w&0xFF; hdr[19]=(w>>8)&0xFF;
    hdr[22]=h&0xFF; hdr[23]=(h>>8)&0xFF;
    hdr[26]=1; hdr[28]=24;
    hdr[34]=img&0xFF; hdr[35]=(img>>8)&0xFF; hdr[36]=(img>>16)&0xFF; hdr[37]=(img>>24)&0xFF;
    fwrite(hdr, 1, 54, f);
    uint8_t *row = calloc(1, row_raw + pad);
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            uint16_t c = src[y * stride + x];
            row[x*3+0] = (uint8_t)((c & 0x1F) * 255 / 31);
            row[x*3+1] = (uint8_t)(((c >> 5) & 0x3F) * 255 / 63);
            row[x*3+2] = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
        }
        fwrite(row, 1, row_raw + pad, f);
    }
    free(row); fclose(f);
    return 0;
}

/* ---- event logging ----------------------------------------------------- */

static void log_event(const epet_event_t *ev, void *ctx)
{
    (void)ctx;
    printf("  [ev] %-12s t=%us a=%d b=%d\n",
           epet_event_name(ev->type), ev->age_ms / 1000, ev->a, ev->b);
}

typedef struct { epet_t *pet; int show_ms; } alert_ctx_t;

static void on_attention(const epet_event_t *ev, void *ctx)
{
    alert_ctx_t *a = ctx;
    printf("  [!!] ATTENTION at t=%us -- lighting the screen\n", ev->age_ms / 1000);
    epet_display_nudge(a->pet, (uint32_t)a->show_ms);
}

/* ---- persistence backend (files under --save-dir) ---------------------- */

static char g_save_dir[512] = "";

static void save_path(char *out, size_t cap, const char *key)
{
    snprintf(out, cap, "%s/epet_%s.bin", g_save_dir, key);
}

static bool file_read(void *ctx, const char *key, void *buf, size_t *len)
{
    (void)ctx;
    if (!g_save_dir[0]) return false;
    char path[600]; save_path(path, sizeof path, key);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t got = fread(buf, 1, *len, f);
    fclose(f);
    *len = got;
    return got > 0;
}

static bool file_write(void *ctx, const char *key, const void *buf, size_t len)
{
    (void)ctx;
    if (!g_save_dir[0]) return false;
    char path[600]; save_path(path, sizeof path, key);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t put = fwrite(buf, 1, len, f);
    fclose(f);
    return put == len;
}

static bool file_erase(void *ctx, const char *key)
{
    (void)ctx;
    if (!g_save_dir[0]) return false;
    char path[600]; save_path(path, sizeof path, key);
    return remove(path) == 0 || errno == ENOENT;
}

static const epet_store_t FILE_STORE = {
    .read = file_read, .write = file_write, .erase = file_erase, .ctx = NULL,
};

/* ---- world setup shared by both modes ---------------------------------- */

typedef struct {
    epet_t          pet;
    epet_bus_t      bus;
    epet_ui_t       ui;
    epet_care_t     care;
    epet_autosave_t autosave;
    alert_ctx_t     alert;
    bool            resumed;
    epet_link_t     link;
} world_t;

static int g_menu_hide_ms = EPET_MENU_HIDE_MS_DEFAULT;

static void link_write(void *ctx, const char *text)
{
    (void)ctx;
    fputs(text, stdout);
    fflush(stdout);
}

static void world_init(world_t *w, int timeout, bool verbose)
{
    epet_bus_init(&w->bus);

    epet_modules_provide(epet_module_core_get());
    uint8_t bad = 0;
    epet_provide_saved_packs(&bad);
    epet_restore_modules(&w->bus);
    if (!epet_modules_find("core")) {
        epet_modules_install(epet_module_core_get(), &w->bus);
    }
    epet_save_modules();

    epet_init(&w->pet);
    epet_attach_bus(&w->pet, &w->bus);

    epet_load_result_t lr = epet_load_pet(&w->pet);
    w->resumed = (lr == EPET_LOAD_OK);
    if (lr == EPET_LOAD_OK) {
        printf("resumed a %s, age %us, hp %.0f\n",
               w->pet.species->name, w->pet.age_ms / 1000u, w->pet.health);
    } else if (g_save_dir[0]) {
        printf("no saved pet (%s); hatching a new one\n",
               epet_load_result_name(lr));
    }
    w->pet.display_timeout_ms = (uint32_t)timeout;
    epet_set_active(&w->pet);

    epet_ui_init(&w->ui, &w->bus);
    w->ui.menu_hide_ms = (uint32_t)g_menu_hide_ms;
    epet_modules_populate_ui(&w->ui);

    epet_care_init(&w->care, &w->bus, 30000);
    epet_autosave_init(&w->autosave, &w->bus, 5000);
    epet_link_init(&w->link, link_write, NULL, &w->bus);
    w->alert = (alert_ctx_t){ .pet = &w->pet, .show_ms = 4000 };
    epet_bus_subscribe(&w->bus, EPET_EV_MASK(EPET_EV_ATTENTION),
                       on_attention, &w->alert, "alert");
    if (verbose) {
        epet_bus_subscribe(&w->bus, EPET_EV_ALL & ~EPET_EV_MASK(EPET_EV_BUTTON),
                           log_event, NULL, "log");
    }
}

/* ---- headless ---------------------------------------------------------- */

static int run_headless(const char *shots, const char *outdir, int speed,
                        int timeout, bool full, const char *page)
{
    world_t w;
    world_init(&w, timeout, true);

    printf("modules: %d installed, %d characters\n",
           epet_modules_count(), epet_species_count());
    if (!w.resumed) {
        printf("hatched a %s -- %s\n", w.pet.species->name, w.pet.species->blurb);
    }

    if (page && !epet_ui_open_titled(&w.ui, &w.pet, page)) {
        fprintf(stderr, "no such page: %s\n", page);
        return 2;
    }

    uint32_t times[32]; int n = 0;
    for (const char *s = shots; *s && n < 32; ) {
        times[n++] = (uint32_t)strtoul(s, (char **)&s, 10);
        if (*s == ',') s++;
    }

    uint32_t sim_ms = 0; int next = 0;
    while (next < n) {
        if (sim_ms >= times[next]) {
            epet_ui_render(&w.ui, &w.pet, fb);
            char path[512];
            snprintf(path, sizeof(path), "%s/epet_%06ums.bmp", outdir, times[next]);
            int rc;
            if (full) {
                draw_chrome((bool[EPET_BTN_COUNT]){false}, w.pet.display_on);
                rc = save_bmp(path, chrome, CHROME_W, CHROME_H, CHROME_W);
            } else {
                to_native();
                rc = save_bmp(path, fb_native, EPET_W, EPET_H, EPET_W);
            }
            if (rc) { fprintf(stderr, "cannot write %s\n", path); return 1; }
            printf("%s  hunger=%.0f joy=%.0f pep=%.0f wsh=%.0f hp=%.0f %s%s%s\n",
                   path, w.pet.hunger, w.pet.happiness, w.pet.energy,
                   w.pet.hygiene, w.pet.health, epet_mood_name(epet_mood(&w.pet)),
                   w.pet.asleep ? " (asleep)" : "",
                   w.pet.display_on ? "" : " [screen off]");
            next++;
            continue;
        }
        uint32_t dt = STEP_MS * speed;
        uint32_t mask = epet_update(&w.pet, dt, (bool[EPET_BTN_COUNT]){false},
                                               (bool[EPET_BTN_COUNT]){false});
        /* keep a requested page pinned open for inspection */
        if (page && !w.ui.active) epet_ui_open_titled(&w.ui, &w.pet, page);
        epet_ui_handle(&w.ui, &w.pet, dt, mask);
        epet_bus_dispatch(&w.bus);
        epet_autosave_tick(&w.autosave, &w.pet, dt);
        sim_ms += dt;
    }

    if (epet_store_available()) {
        epet_save_pet(&w.pet);
        printf("saved on exit\n");
    }
    return 0;
}

/* ---- interactive ------------------------------------------------------- */

static int run_window(int speed, int timeout, bool verbose)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *win = SDL_CreateWindow("epet",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WIN_W, WIN_H, SDL_WINDOW_SHOWN);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565,
        SDL_TEXTUREACCESS_STREAMING, CHROME_W, CHROME_H);

    world_t w;
    world_init(&w, timeout, verbose);

    printf("epet simulator -- click the buttons, or use the keyboard\n"
           "  LT  Q / 1   scroll menu up      RT  P / 3   select\n"
           "  LB  A / 2   scroll menu down    RB  L / 4   back\n"
           "  R reset   S screenshot   [ ] speed (now %dx)   Q-hold/Esc quit\n"
           "  screen blanks after %d ms idle; any press wakes it\n", speed, timeout);
    if (!w.resumed) {
        printf("  hatched a %s -- %s\n", w.pet.species->name, w.pet.species->blurb);
    }

    bool running = true, down[EPET_BTN_COUNT] = {false};
    int mouse_btn = -1, shot = 0;
    uint32_t prev = SDL_GetTicks();

    while (running) {
        bool edge[EPET_BTN_COUNT] = {false};
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;

            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                int mx = e.button.x / SCALE, my = e.button.y / SCALE;
                for (int i = 0; i < EPET_BTN_COUNT; i++) {
                    if (box_hit(&BOXES[i], mx, my)) {
                        if (!down[i]) edge[i] = true;
                        down[i] = true;
                        mouse_btn = i;
                    }
                }
            }
            if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
                if (mouse_btn >= 0) { down[mouse_btn] = false; mouse_btn = -1; }
            }

            if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
                bool press = (e.type == SDL_KEYDOWN);
                int b = -1;
                switch (e.key.keysym.sym) {
                case SDLK_q: case SDLK_1: b = EPET_BTN_LT; break;
                case SDLK_a: case SDLK_2: b = EPET_BTN_LB; break;
                case SDLK_p: case SDLK_3: b = EPET_BTN_RT; break;
                case SDLK_l: case SDLK_4: b = EPET_BTN_RB; break;
                default: break;
                }
                if (b >= 0) {
                    if (press && !down[b]) edge[b] = true;
                    down[b] = press;
                } else if (press) {
                    switch (e.key.keysym.sym) {
                    case SDLK_ESCAPE: running = false; break;
                    case SDLK_r: world_init(&w, timeout, verbose); break;
                    case SDLK_LEFTBRACKET:  if (speed > 1) speed--; printf("speed %dx\n", speed); break;
                    case SDLK_RIGHTBRACKET: speed++; printf("speed %dx\n", speed); break;
                    case SDLK_s: {
                        char path[256];
                        snprintf(path, sizeof(path), "epet_shot_%02d.bmp", shot++);
                        draw_chrome(down, w.pet.display_on);
                        save_bmp(path, chrome, CHROME_W, CHROME_H, CHROME_W);
                        printf("wrote %s\n", path);
                        break;
                    }
                    default: break;
                    }
                }
            }
        }

        uint32_t now = SDL_GetTicks();
        uint32_t dt = (now - prev) * speed;
        prev = now;
        if (dt > 250 * (uint32_t)speed) dt = 250 * (uint32_t)speed;

        bool was_on = w.pet.display_on;
        uint32_t mask = epet_update(&w.pet, dt, down, edge);
        epet_ui_handle(&w.ui, &w.pet, dt, mask);
        epet_bus_dispatch(&w.bus);
        epet_autosave_tick(&w.autosave, &w.pet, dt);
        if (was_on != w.pet.display_on) {
            printf("display %s (idle %ums)\n",
                   w.pet.display_on ? "wake" : "sleep", w.pet.idle_ms);
        }

        if (w.pet.display_on) epet_ui_render(&w.ui, &w.pet, fb);
        draw_chrome(down, w.pet.display_on);

        SDL_UpdateTexture(tex, NULL, chrome, CHROME_W * sizeof(uint16_t));
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
        SDL_Delay(STEP_MS);
    }

    if (epet_store_available()) {
        epet_save_pet(&w.pet);
        printf("saved on exit\n");
    }

    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

int main(int argc, char **argv)
{
    bool headless = false, full = false, verbose = false;
    const char *shots = "0", *outdir = ".", *page = 0;
    int speed = 1, timeout = EPET_DISPLAY_TIMEOUT_MS_DEFAULT;
    uint32_t seed = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless")) headless = true;
        else if (!strcmp(argv[i], "--device")) full = true;
        else if (!strcmp(argv[i], "--verbose")) verbose = true;
        else if (!strcmp(argv[i], "--shots")   && i + 1 < argc) shots = argv[++i];
        else if (!strcmp(argv[i], "--out")     && i + 1 < argc) outdir = argv[++i];
        else if (!strcmp(argv[i], "--page")    && i + 1 < argc) page = argv[++i];
        else if (!strcmp(argv[i], "--speed")   && i + 1 < argc) speed = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) timeout = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--menu-hide") && i + 1 < argc) g_menu_hide_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--save-dir") && i + 1 < argc) {
            snprintf(g_save_dir, sizeof g_save_dir, "%s", argv[++i]);
            epet_store_set(&FILE_STORE);
        }
        else { fprintf(stderr, "unknown option: %s\n", argv[i]); return 2; }
    }
    if (speed < 1) speed = 1;
    /* Fixed seed unless asked otherwise, so headless renders are repeatable. */
    if (seed == 0) seed = headless ? 1u : (uint32_t)time(NULL);
    epet_seed_random(seed);

    return headless ? run_headless(shots, outdir, speed, timeout, full, page)
                    : run_window(speed, timeout, verbose);
}
