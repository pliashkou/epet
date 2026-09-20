# epet — Tamagotchi clone for Waveshare ESP32-S3-LCD-1.3

## Hardware

Waveshare ESP32-S3-LCD-1.3 (https://www.waveshare.com/esp32-s3-lcd-1.3.htm)

- ESP32-S3 (QFN56) rev v0.2, dual core, 160 MHz
- 16 MB Winbond flash, 8 MB **octal** PSRAM (AP_3v3, 80 MHz)
- **240x240 ST7789VW IPS LCD** — an LCD, not an OLED
- QMI8658 6-axis IMU on I2C
- TF card slot, Li-ion charger

### Pin map

Taken from the vendor factory program, not guessed. These differ from the
ESP32-S3-LCD-1.47 board's pins — do not copy pinouts between Waveshare models.

| Signal | GPIO |
|---|---|
| LCD MOSI | 41 |
| LCD SCLK | 40 |
| LCD CS | 39 |
| LCD DC | 38 |
| LCD RST | 42 |
| LCD backlight (BL_PWM) | 20 |
| I2C SDA / SCL (IMU) | 47 / 48 |
| BOOT button (active-low) | 0 |

Free for expansion: GPIO 1-3, 5-18, 21. **GPIO 19 and 20 are not free** --
the schematic uses them for `RST_Control` and `BL_PWM`. GPIO4 is unused --
the vendor factory program calls it the backlight and is simply wrong. The
same vendor header's other claim, `lcd_bl 20`, is the correct one; it was
dismissed here as a typo for a long time. No user buttons are fitted on the
board; only BOOT and RESET exist.

### ST7789 quirks

Both are required or the image is wrong, and both come from the factory source:

- **Y gap of 80.** The 240x240 window sits 80 rows into the controller's
  240x320 GRAM — `esp_lcd_panel_set_gap(panel, 0, 80)`.
- **Colour inversion on** (`0x21`) — `esp_lcd_panel_invert_color(panel, true)`.
- MADCTL `0xC0` (MX+MY) — `esp_lcd_panel_mirror(panel, true, true)`.
- ST7789 wants RGB565 most-significant byte first, so native-endian
  framebuffer data must be byte-swapped before `draw_bitmap`.

## Flashing — use the UART port, not USB-JTAG

Flash and monitor over the **CH343 UART bridge** (`/dev/ttyUSB*` on Linux,
`/dev/cu.wchusbserial*` on macOS, a `COM` port on Windows). It works at
460800 with no issues.

The native USB-Serial-JTAG port detects the chip but fails every write:
`Failed to write to target RAM (0107: Checksum error)` with the stub,
`Failed to write to target Flash after seq 0 (0105)` with `--no-stub`.
Lowering the baud rate does not help, and its console emits nothing once the
app runs.

The reason is now clear: **the chip's own D+/D- reach no connector.** There is
exactly one Type-C on the board and its data pins go to the CH343P; the
schematic reuses GPIO19/20 for `RST_Control` and `BL_PWM` instead. So there
was never a USB path to the ESP32 for that port to work over.

The firmware now turns the PHY off during `lcd_init()` to free GPIO20 for the
backlight, so the port does not appear at all. That is the correct behaviour
here, not a regression.

Keep `CONFIG_ESP_CONSOLE_UART_DEFAULT=y` in `sdkconfig.defaults` for the same
reason — the USB-JTAG console produces no output on this board.

## Layout

```
components/epet_core/   epet.h, epet_state.c, epet_render.c
                        All pet logic and drawing. No ESP-IDF, no SDL.
                        Framebuffer is native-endian RGB565, uint16_t[240*240].
main/main.c             ESP32 platform layer: ST7789 + GPIO buttons.
sim/main.c              host platform layer: SDL2 window + keyboard buttons.
sim/Makefile
```

The device firmware and the host simulator compile the *same* `epet_core`
sources, so they cannot drift. Only the framebuffer destination and the button
source differ. Put new logic or drawing in `epet_core`, never in a platform
layer.

## Build and run

ESP-IDF v5.5.1, toolchain installed for `esp32s3` only. `$PORT` is the CH343
UART bridge -- see **Flashing** below for why it must not be the native USB
port.

```bash
. /path/to/esp-idf/export.sh
idf.py build
idf.py -p $PORT -b 460800 flash monitor
```

Simulator (needs SDL2; built `-g -O0` for a debugger):

```bash
cd sim && make && ./epet_sim
```

The window draws a mock device: the 240x240 panel with two buttons down each
side, **clickable with the mouse**. Keyboard equivalents: `Q`/`1` = LT,
`A`/`2` = LB, `P`/`3` = RT, `L`/`4` = RB. Also `R` reset, `S` screenshot,
`[`/`]` speed, `Esc` quit. `--verbose` logs every event to the console.

Headless frame dumping, for checking rendering without a window or hardware:

```bash
./epet_sim --headless --shots 0,60000,140000 --out /tmp/shots
```

It writes `epet_<time>ms.bmp` per requested sim time and prints the stat line
for each. `--device` renders the whole mock device including buttons rather
than just the panel; `--page STATS` pins a page open so its layout can be
inspected. The output is BMP; convert to PNG with whatever the host has.

## Display power

Configured in `menuconfig` under **epet configuration**, or directly in
`sdkconfig.defaults`:

| Option | Default | Meaning |
|---|---|---|
| `CONFIG_EPET_DISPLAY_TIMEOUT_MS` | 30000 | Blank the panel after this idle time. 0 = never. |
| `CONFIG_EPET_LIGHT_SLEEP` | y | Halt the CPU while blanked, waking on a button GPIO. |
| `CONFIG_EPET_WAKE_INTERVAL_MS` | 60000 | Also wake on a timer this often to advance the sim. 0 = GPIO only. |
| `CONFIG_EPET_MENU_HIDE_MS` | 5000 | Fade the menu overlay out after this long unused. 0 = never. |
| `CONFIG_EPET_AUTOSAVE_MS` | 30000 | Minimum gap between automatic saves to NVS. |
| `CONFIG_EPET_AGE_LEVEL_MS` | 3000 | Wall time per age level. 50 levels, then growth stops. |
| `CONFIG_EPET_ALERT_SHOW_MS` | 4000 | How long an attention alert lights the screen. |
| `CONFIG_EPET_CARE_INTERVAL_MS` | 120000 | Minimum gap between attention alerts. |
| `CONFIG_EPET_IDLE_POLL_MS` | 120 | Fallback poll interval when light sleep is disabled. |

The **policy lives in `epet_core`** (`display_on`, `idle_ms`,
`display_timeout_ms`), so it is testable and reproducible in the simulator.
Platform layers only react to the transition:

- ESP32: backlight off, then `esp_lcd_panel_disp_on_off(false)` and
  `esp_lcd_panel_disp_sleep(true)` (SLPIN stops the panel's booster and
  oscillator — much better than DISPOFF alone). Waking needs a **120 ms delay
  after SLPOUT** before further commands, and that delay must not be billed to
  the simulation `dt`.
- Simulator: fills the window black, mirroring the backlight going off.

Two behaviours worth preserving:

- **The waking press is swallowed.** Pressing a button while blanked only
  wakes the display; it does not feed/play/clean. `epet_update()` clears the
  edge array for that frame.
- **The pet keeps living while blanked.** Only the output sleeps — hunger,
  happiness and age keep advancing. Do not gate `epet_update()` on
  `display_on`; gate only `epet_render()` and the SPI flush.

Both are covered by `tests/test_power.c` (`cd sim && make test`).

### Light sleep

While blanked the CPU halts in `esp_light_sleep_start()` and wakes on any
wired button GPIO going low (`gpio_wakeup_enable(pin, GPIO_INTR_LOW_LEVEL)`
plus `esp_sleep_enable_gpio_wakeup()`). Verified on hardware: wake cause 7 is
`ESP_SLEEP_WAKEUP_GPIO`.

Note the current draw has not been measured here — no instrumentation was
available. Light sleep is expected to cut idle current substantially, but
treat any specific figure as unverified until metered.

Details that matter:

- **`gpio_wakeup_enable()` needs the pin already configured as an input**, so
  it must run after `buttons_init()`.
- **Flush the UART first** with `esp_rom_output_tx_wait_idle()`, or the log
  line being transmitted is cut mid-character when the UART clock stops.
- **`esp_timer` is compensated across light sleep; the FreeRTOS tick is not.**
  So `esp_timer_get_time()` gives the true elapsed time, while `esp_log`
  timestamps freeze for the duration of the sleep. Reading a log, a frozen
  timestamp across a sleep is expected, not a hang.
- **Replay the gap with `epet_advance()`, never one giant `epet_update()`.**
  A single huge `dt` skips poop events, coarsens sleep transitions and
  mis-integrates health. `epet_advance()` steps in `EPET_MAX_STEP_MS` chunks
  and applies the button edges to the *final* step, because the gap elapsed
  before the press. Applying them first would let the display re-blank during
  the catch-up. Both properties are asserted in `tests/test_power.c`.

Possible next tier: deep sleep, which would need the pet state persisted to
RTC memory or NVS and a full re-init on wake.

## Architecture

Everything that is not hardware lives in `components/epet_core` and is
compiled unchanged by both the firmware and the host simulator.

```
epet_module.[ch]  module registry: bundles of characters + sub-programs
epet_module_core.c the default module: everything shipped in the firmware
epet_store.[ch]   platform storage hook (3 blob ops, keyed by string)
epet_save.[ch]    save format, autosave, module-list persistence
epet_event.[ch]   publish/subscribe bus (no allocation, fixed tables)
epet_sprite.[ch]  frame format + transparent blitter
epet_species.[ch] character classes, pose lookup, animation player
epet_sprites.c    GENERATED artwork -- see tools/gen_sprites.py
epet_state.[ch]   the simulation: needs, health, sleep, display power
epet_draw.[ch]    framebuffer primitives + 5x7 font, public so pages can draw
epet_ui.[ch]      menu, scrolling, page lifecycle
epet_pages.[ch]   built-in sub-programs
epet_care.[ch]    example module: decides when to ask for attention
epet_render.c     the main screen (status panel + menu + pet)
```

### Buttons

Named by physical position, two down each side. What each one does is decided
in `epet_ui.c`, not scattered through the simulation.

| Button | Menu hidden | Menu shown | Inside a page |
|---|---|---|---|
| LT | reveal menu | menu up (wraps) | page's choice |
| LB | reveal menu | menu down (wraps) | page's choice |
| RT | reveal menu | SELECT: run the item | page's choice |
| RB | -- | -- | BACK: always closes the page |

### The menu

A translucent overlay down the left edge showing four icons at a time, over an
unobstructed full-width scene. It fades out after `menu_hide_ms` and comes
back on LT, LB or RT.

**The revealing press is consumed**, exactly like the display wake press: it
brings the menu back without also moving the cursor or launching a page. There
are now two such swallows in a row from a fully idle device -- one to wake the
screen, one to reveal the menu -- which is deliberate: neither should ever
trigger an action the user could not see coming.

Closing a page re-shows the menu, so you can always see where you are.

Icons are drawn by the page itself via the optional `icon` hook; a page with no
icon falls back to the first letter of its title. Icons must not stamp opaque
colour to "punch" a hole -- the menu is translucent, so a flat fill shows as a
hard dark hole. Use `epet_shade()` to darken, or plot the shape directly (see
`icon_sleep`'s crescent).

BACK is handled by the UI before the page sees it, so no page can trap the
user. GPIOs are `CONFIG_EPET_BTN_*_GPIO`; `CONFIG_EPET_BOOT_AS_LT` aliases
the onboard BOOT button to LT so the board is usable before buttons are wired.

Buttons no longer map to actions. `epet_update()` returns a bitmask of the
edges that SURVIVED the display wake-swallow, and the UI acts only on those.
Pages call `epet_apply_action(pet, EPET_ACT_FEED)` and friends.

### Characters

A **frame** is an 8bpp palette-indexed bitmap; **index 0 is transparent**, so
sprites composite over the scene. Indices 1..15 look up in the species
palette, which is why two species can share frame structure and still look
entirely different.

A **pose** is a named sequence of frames with per-frame hold times. Poses are
resolved **by name**, never by enum:

```c
const epet_pose_t *p = epet_species_pose(sp, "happy");
```

So **adding a pose later needs no code changes anywhere** -- append an entry
to the species' `poses` array. `epet_species_pose_or_idle()` falls back to
idle for a name a species does not define, so an older species can never
crash on a pose added for a newer one.

Built-in poses: `idle` (loops), `birth` (one-shot), `happy` (one-shot),
`sad` (loops).

The player (`epet_actor_t`) runs one pose and optionally resumes another when
a one-shot ends. Looping poses follow the mood automatically; explicit plays
override:

```c
epet_pet_emote(pet, EPET_POSE_HAPPY);   /* returns to the mood loop after */
```

#### Growth

A pet has an AGE LEVEL from 1 to `EPET_AGE_MAX` (50), derived from `age_ms`
at `CONFIG_EPET_AGE_LEVEL_MS` per level. Age itself keeps climbing past the
top level; it just stops changing how the pet looks.

A species declares `stages[]`, each `{ from_age, scale_pct, poses, n_poses }`:

- **`scale_pct` is the on-screen size**, interpolated from one stage to the
  next across the ages between them. That is what makes all fifty levels a
  different size instead of the size stepping three or four times.
- **`poses` may be NULL**, which inherits the stage before it. A stage that
  only grows costs six bytes in a pack, so a character *can* declare all
  fifty.
- A stage may define only SOME poses; what it leaves out falls through.

The built-ins grow through baby, teen, adult and elder, generated by morphing
the same drawing code rather than by hand-drawing four sets (`GROWTH` and
`morphed()` in `tools/gen_sprites.py`). That quadrupled the sprite data:
820 KB of firmware became 1.13 MB, still 64% of a 3 MB slot free.

Things that are easy to get wrong here:

- **Only the main screen grows.** Pages draw the pet at the species' base
  scale so their layouts stay put. The stage still chooses WHICH artwork is
  drawn there, because it is the same creature either way.
- **The sprite is anchored by its FEET**, not its centre (`PET_GROUND` in
  `epet_render.c`, `epet_blit_bottom_q8()`). Scaling about the middle sinks a
  growing pet through the floor.
- **Integer scaling was not enough.** Anchored on the ground line with the
  44x56 canvas, only scales 2 and 3 fit between the status bar and the
  bottom -- two sizes, not "many". `epet_blit_q8()` walks the DESTINATION
  and samples back into the source, and at a whole-number scale it is
  pixel-identical to `epet_blit()`, so art that was crisp stays crisp.
- **That sampling must be Bresenham, not a fixed-point step.** The first
  version precomputed `(w << 16) / dw` and accumulated it, which is exact
  only when `dw` divides 65536. At scale 2 it does; at scale 3 it does not,
  and the sample point drifts a whole source row within three pixels. It
  looked almost right. `tests/test_character.c` now compares the two
  blitters at every whole scale, which is how it was caught.
- **A stage that is too big is a build error**, not a surprise on the device:
  `assert_fits_panel()` in the generator mirrors `PET_GROUND` and `PANEL_H`
  and refuses art that would grow up into the status bar. The ceiling with
  the current canvas is about 350%.
- **Pose fallback order is deliberate**: this stage (walking back through
  earlier ones), then the SPECIES' own pose, then this stage's idle. The
  species comes before the stage's idle because a pose is about what the
  creature is DOING -- a baby with no "happy" of its own should still bounce,
  drawn from the base art at the baby's size, rather than stand there.
- **Growing up mid-animation keeps the frame index and phase**
  (`epet_actor_set_age()`), or a pet that grows during a blink snaps back to
  frame 0.
- **Packs got bigger, so the ceiling moved.** `EPET_PACK_MAX_BYTES` went from
  64 KB to 192 KB; with `CONFIG_SPIRAM_USE_MALLOC` and a 16 KB internal
  threshold that buffer lands in PSRAM, not the internal heap. The sample
  pack is 78 KB and takes about 40 s over the UART.

#### What it leaves behind

`species->poop` is a sprite drawn in the body palette, so each class makes a
different mess: BLOB a coil, SPROUT a seed husk, SPARK a heap of embers.
NULL falls back to a generic brown pile, so a species without art still works.

#### Backdrops

Each class has **one or more** main-screen backgrounds (`species->backdrops`,
`n_backdrops`); which one a pet gets is rolled at birth alongside the class,
and the HOME page cycles through them.

A backdrop is a low-resolution indexed image (80x80) scaled 3x to fill the
panel -- about 6 KB each, against ~58 KB for a full 240x240 frame. It carries
its **own palette**, unrelated to the creature's body colours, and a night
tint blended over it while the pet sleeps.

**A backdrop must be fully opaque.** Index 0 means transparent, so a single
gap leaves the previous frame showing through on screen. MEADOW shipped with
10 such pixels (90 on screen) before the test caught it;
`tools/gen_sprites.py` now refuses to emit a backdrop containing index 0, and
`tests/test_character.c` asserts the drawn result leaves no pixel unpainted.

Set `image` to NULL and the flat `sky`/`ground` colours are used instead,
which is what a species with no art gets.

**Temperament** (`epet_temperament_t`) scales each decay rate, so a class is a
behaviour and not just a recolour. SPROUT eats 25% slower but gets dirty 35%
faster.

**The class is rolled randomly at every birth**, including rebirth after
death. The CLASS page overrides it for the current pet; the next birth rolls
again.

#### Sprite canvas

Frames are 44 wide by **56 tall**, not square. SPROUT's stem and leaf are
drawn ABOVE the body, and in the tall poses (the happy bounce, `cy=19 ry=17`)
they ran off the top of a 44x44 canvas and were silently sliced off -- which
looked like the character's top line being cut, intermittently, depending on
the pose. `BODY_DY` in the generator pushes every body down into that
headroom, applied in one place so all call sites keep the old coordinates.

Because the canvas has headroom above the body, `PET_CY` in `epet_render.c`
sits lower than the visual middle to keep the feet on the ground line.

Two guards: `tools/gen_sprites.py` refuses to emit a frame with an opaque
pixel on the outermost row or column, and `tests/test_character.c` asserts
the same for every frame of every pose. If you enlarge a pose and the build
stops with "touches the canvas edge", raise `H` or lower `BODY_DY`.

#### Colour storage

Colours are stored **panel-ready**: RGB565, most-significant byte first, the
order the ST7789 reads. `EPET_RGB565` byte-swaps at compile time.

The framebuffer used to hold native-endian values and the firmware swapped all
57600 pixels before every transfer -- 9.5 ms per frame, 38% of the frame
budget, purely reordering bytes. Storing them panel-ready deletes that pass and
lets the device render straight into the DMA buffer, which also merged two
115 KB framebuffers into one. The simulator pays the swap instead, once per
frame when uploading to SDL, where it is free.

Only code that DECOMPOSES a colour cares: `epet_mix()` unpacks and repacks.
Everything else just moves opaque values around.

Two traps this created:

- **Every colour literal must go through the macro.** The night tint in
  `gen_sprites.py` was a raw `0x18CE` and stayed in the old order -- the only
  colour that did, and it only showed while the pet slept.
- **Packs built before the change render with red and blue transposed.**
  Orange reads back as light cyan. There is deliberately no version check for
  this, so rebuild packs with `tools/make_module.py` after pulling.

#### Artwork

`components/epet_core/epet_sprites.c` is GENERATED. Edit the shapes in
`tools/gen_sprites.py` and regenerate:

```bash
python3 tools/gen_sprites.py > components/epet_core/epet_sprites.c
```

#### Randomness

`epet_random()` is a xorshift32 in the core, so device and simulator agree.
It starts from a FIXED seed to keep tests reproducible; platforms seed it with
real entropy at startup.

Two traps, both hit and fixed here:

- **Small sequential seeds leaked into the output.** Raw xorshift32 seeded
  with 1,2,3,4 alternated classes perfectly, because `% n` took the weakest
  bit. `epet_seed_random()` now mixes and warms up the state, and
  `epet_random_below()` takes the high bits.
- **`esp_random()` is not random with the radio off.** With WiFi and BT
  disabled it can return the same value after every reset, so every boot
  hatched the same class. The firmware now rolls a seed forward in NVS: each
  boot consumes the stored value and writes the next.

Tests that compare two pets must seed identically first, or they get two
different classes with different decay rates:

```c
epet_seed_random(42); epet_init(&a);
epet_seed_random(42); epet_init(&b);
```

A test about a rate (how fast hunger climbs) should pin the class with
`epet_set_species(&pet, epet_species_builtin(0), false)`.

### Modules

A module bundles **characters** and **menu items (sub-programs)**. Everything
shipped in the firmware is the `core` module; nothing about it is privileged
beyond being installed first. Installing adds its characters to the pool that
birth rolls from and its pages to the menu, in install order.

```c
epet_modules_provide(epet_module_core_get());   /* make it known */
epet_restore_modules(&bus);                     /* install what was installed last run */
epet_modules_install(&my_module, &bus);         /* or install directly */
epet_modules_populate_ui(&ui);                  /* its pages join the menu */
```

`provide` and `install` are separate on purpose: a saved module list is a list
of **ids**, so a module must be known by id before it can be re-installed on
boot. A module parsed from a download would `provide` itself once decoded.

The MODS page lists what is installed. `tests/test_modules.c` builds a whole
module outside the core -- character, backdrop, page, lifecycle hooks -- which
is the real check that the abstraction holds.

#### Loading modules from a browser

`web/index.html` is a two-page Web Serial app: **Install** (drop an `.epmod`,
send it) and **Manage** (read the device's module list, remove one). Serve it
over http://localhost or https -- Web Serial refuses `file://` -- and use
Chrome or Edge.

```bash
python3 -m http.server -d web 8000     # then open http://localhost:8000
python3 tools/make_module.py web/spark.epmod   # rebuild the sample pack
```

**Modules ship code.** A pack can carry two kinds of sub-program:

- **Declarative pages** (`epet_dynpage.h`) -- a title, an icon id and rows
  naming what to show. Enough for an information screen, no logic.
- **Bytecode pages** (`epet_vm.h`) -- real programs with branching,
  arithmetic, per-page state and input handling, run on a small stack VM.

Native code is the thing that cannot be loaded: there is no dynamic linker,
no memory protection, and one bad pointer takes the device down. Bytecode
gives modules genuine behaviour while keeping every access checked.

**What contains a loaded program** (`tests/test_vm.c` asserts all of it):

- No raw memory access. The framebuffer is reachable only through drawing
  syscalls, which clip.
- Every jump target, local, global, string id, syscall id and arity is
  verified at load; code that fails is never run.
- **Stack depth is tracked through every path** at validation time. This
  catches the commonest authoring mistake -- calling a syscall without
  pushing its arguments -- which otherwise loads fine and silently stops
  drawing halfway down the page. It happened while building the sample.
- An instruction budget per hook, so a runaway page cannot hang the device.
  Note the budget must be tested *before* decrementing: `while (budget--)`
  wraps to `UINT32_MAX` on the last iteration and the exhaustion check never
  fires.
- **Entry point 0 means "hook absent"**, so no real hook may live at offset
  0. `vmasm.py` emits a leading `nop` to guarantee that. Without it the
  first label lands at 0 and its hook is silently never bound.

#### Writing a module program

`tools/vmasm.py` is the assembler; `tools/make_module.py` shows a working
page (two tabs, live meters, a computed vitality score, a Feed button).

```
update:
  arg 1                  ; button edge mask
  push $BTN_RT
  and
  jz skip
  sysv act $ACT_FEED     ; push args, call, keep the result
  jz skip
  emote "happy"
skip:
  ret 1                  ; non-zero keeps the page open
```

Three call forms, and mixing them up is the easy mistake:

| form | pushes args | keeps result |
|---|---|---|
| `text 8 8 "HI" $WHITE 2` | yes | no (dropped) |
| `sysv get $SRC_HEALTH` | yes | yes |
| `sysk levelcol` | no, already on the stack | yes |

**Background hooks.** A pack may declare event handlers that run whether or
not any of its pages are open -- that is what makes it a module rather than a
screen. A hook is a bytecode entry point plus a subscription mask:

```
on_event:            ; args: 0=event type, 1=payload a, 2=payload b, 3=age_s
  arg 0
  push $EV_POOPED
  eq
  jz done
  loadg 3
  push 1
  add
  storeg 3           ; module globals, shared with this pack's pages
done:
  ret 1
```

- **Globals are module-wide**, shared by every page and the hook, and are NOT
  cleared on page entry -- a page can display what the hook counted while it
  was closed.
- **A hook runs with no framebuffer**, so the drawing syscalls are no-ops
  rather than painting over whatever is on screen. What it can do is read the
  pet, apply actions, play a pose, and call `nudge <ms>` to ask for the
  screen.
- **`on_install` subscribes and `on_remove` unsubscribes**, so a handler
  lives exactly as long as its module. Removal frees the handler's memory, so
  `epet_bus_unsubscribe()` must run first or the bus keeps a dangling
  pointer.
- Hooks need `epet_set_active(&pet)` to have been called: a loaded module has
  no pet pointer of its own.

**Pack format** (`epet_modblob.h`): 64-byte header plus frames, poses,
backdrops, species and pages, little-endian and unaligned -- every field is
read byte-wise because Xtensa faults on unaligned loads. FNV-1a over the
payload, and every index is bounds-checked before use.

**Wire protocol** (`epet_link.h`): `#`-prefixed text lines so it shares the
console UART with log output. Two things it needs that a first attempt
lacked:

- **Per-chunk acknowledgement.** The device reads far slower than 115200
  delivers; streaming freely overran its UART buffer and silently truncated
  the pack, which then failed to parse.
- **Sequence numbers and retry.** Even with acks, ~5 of 326 lines were
  dropped. Chunks are numbered, the device reports the next one it expects,
  and the host re-sends. A 44 KB pack transfers in ~25 s with a handful of
  retries.

**Partition table.** `partitions.csv` gives NVS **512 KB**. The default 24 KB
cannot hold a pack -- installs failed with `#ERR storage` -- because a single
NVS blob is capped near the partition size. Changing this moves partition
offsets, so flash must be erased once (`idf.py erase-flash`).

**When a module is removed**, its characters go with it. If the pet *is* one
of them, `epet_validate_species()` starts a new creature from what remains,
and a save naming the vanished class is rejected with `EPET_LOAD_NO_SPECIES`
rather than resurrecting the wrong thing. `core` cannot be removed: without
it there is no content at all.

### Persistence

Two records, both versioned, length-checked and checksummed (FNV-1a):

- `pet` -- class (BY NAME), home, stats, age, mess, alive/asleep
- `mods` -- the ids of installed modules, so sub-programs return after a restart

The core owns the format; a platform supplies three blob operations via
`epet_store_set()`. The firmware backs them with NVS, the simulator with files
under `--save-dir`. So both persist identically and the tests can use an
in-memory store.

Decisions worth keeping:

- **The class is stored by name, not index or pointer.** Indices shift when a
  module is installed or removed, and pointers mean nothing across a reboot.
  If the saved class is gone (its module was removed) the load fails with
  `EPET_LOAD_NO_SPECIES` rather than resurrecting the wrong creature.
- **A resumed pet does not replay birth**, and `ev_minute` is restored so it
  does not replay every minute it ever lived as events.
- **Autosave is rate-limited, not delayed.** `epet_autosave_tick()` writes
  when something changed AND the interval has passed since the last write --
  so a change after a long quiet spell writes immediately. The firmware also
  flushes just before entering light sleep, since the next wake may be
  minutes away.

### The FreeRTOS tick is 10 ms, and it has bitten three times

`CONFIG_FREERTOS_HZ=100`, so **any delay under 10 ms rounds to zero ticks**.
Three separate bugs here came from that, each presenting as something else:

- `vTaskDelay(pdMS_TO_TICKS(2))` in the module transfer loop yielded nothing,
  spun CPU0 for the whole transfer and starved IDLE0, tripping the task
  watchdog every 5 s. Looked like slow flash writes; the NVS write actually
  takes 506 ms. Use `vTaskDelay(1)` when you mean "yield for a moment".
- `vTaskDelay(pdMS_TO_TICKS(15))` between IMU burst samples collapsed the
  burst into a few milliseconds, so a shaken board measured 5-60 mg. Use
  `esp_rom_delay_us()` when the spacing itself matters.
- `pdMS_TO_TICKS()` wrapped around an I2C timeout that was already in
  milliseconds double-converted to 0 ticks, so every register read failed.

When something times out, stalls or samples nothing, check the tick maths
before suspecting the hardware.

### The backlight is GPIO20, behind the USB PHY

`BL_PWM` on GPIO20 goes through a 1K resistor into Q5 (an MMBT3904) whose
collector feeds the panel's `LEDA`. **Active high.** Confirmed in the vendor
[schematic](https://files.waveshare.com/wiki/ESP32-S3-LCD-1.3/ESP32S3_1.3inch.pdf).

The catch, and it is a good one: **GPIO19 and 20 are the ESP32-S3's native
USB D-/D+**, and the ROM leaves `USB_SERIAL_JTAG_CONF0_REG.USB_PAD_ENABLE`
set, which lets the USB PHY drive both pads through the GPIO matrix. Until
that bit is cleared, every `gpio_set_level()` on GPIO20 is accepted and
silently discarded. So:

```c
usb_serial_jtag_ll_phy_enable_pad(false);   /* hal/usb_serial_jtag_ll.h */
gpio_reset_pin(PIN_BL);
```

Clearing it costs nothing here: the Type-C port is wired to the CH343P
bridge, not to the chip's own USB, and flashing goes over the UART anyway.

This wasted an afternoon, and the reason is worth remembering. A sweep of 18
pins in both polarities "proved" the backlight was hardwired to the rail --
and **GPIO20 was in that sweep**. A negative result from a sweep only rules a
pin out if the writes actually reach it; a peripheral holding the pad looks
exactly like a pin that does nothing. The vendor wiki said outright that the
factory program turns the backlight off when the board is stood upright,
which was flat evidence against "not switchable" and should have forced a
re-check much sooner. When a measurement contradicts the documentation,
suspect the measurement.

`lcd_set_power(false)` now drops the backlight first, then blanks GRAM (so a
wake shows black rather than a stale pet for one frame), then sleeps the
controller.

### Shake to wake

The QMI8658 IMU is on **I2C SDA 47 / SCL 48, address 0x6B**. Configured by
`CONFIG_EPET_SHAKE_*`: threshold 300 mg, hold 2000 ms, idle poll 400 ms,
confirm poll 120 ms.

Four things here are non-obvious, and each cost real time:

- **`i2c_master_transmit_receive()` takes milliseconds, not ticks.** Wrapping
  it in `pdMS_TO_TICKS()` double-converts: at `FREERTOS_HZ=100`,
  `pdMS_TO_TICKS(50)` is 5, the driver reads that as 5 ms, converts it to 0
  ticks, and gives up before the ~650 us transfer finishes. It returns
  `ESP_ERR_INVALID_STATE` and logs nothing, so it looks exactly like absent
  hardware. This is why the IMU appeared dead for a long time.
- **`CTRL1` must be 0x40, not 0x60.** Bit 5 selects big-endian output while
  the parser here is little-endian.
- **Measure change between samples, not deviation from gravity.** During a
  shake the magnitude keeps sweeping back through 1 g as direction reverses,
  so `||a| - 1g|` peaked at 331 mg across 30 s of hard shaking -- below any
  usable threshold. Sample-to-sample delta reads thousands of mg.
- **One sample per wake aliases.** A shake is periodic at a few Hz; sampling
  once per 400 ms keeps catching it at a similar phase and reads 0-4 mg while
  the board is being thrown about. `imu_shaken()` takes a burst of 6 samples
  15 ms apart and uses the largest delta.

Sustained-shake logic lives in the main loop: credit accrues while moving and
drains at half rate while still, so a knock fades but a deliberate shake
accumulates through its own pauses. Measured: a knock reached 1460/2000 and
decayed; continuous shaking crossed 2000 and woke. At rest the delta is
6-25 mg against a 300 mg threshold.

Shaking a **dead** pet revives it with a freshly rolled class.

The IMU also **stops updating across light sleep**, so `imu_resume()`
rewrites CTRL1/2/7 after each wake.

> Do not add an I2C pin scanner. The one used to find this part drove GPIO 15,
> where the board has a WS2812, and latched the LED on -- a WS2812 keeps its
> colour until new data arrives, so `led_off()` now sends an all-zero frame at
> boot to undo it.

Known limitation: **the pet does not age while powered off.** There is no
battery-backed clock here, so elapsed wall time across a power cycle is
unknown. Storing a timestamp would need an RTC or a network time source.

### Adding a sub-program

Write an `epet_page_t` and register it. Nothing else changes -- not the menu,
not the input handling, not the simulation:

```c
static void my_render(epet_page_t *self, const epet_t *pet, uint16_t *fb) {
    epet_fill(fb, EPET_C_PANEL);
    epet_text(fb, 10, 10, "HELLO", EPET_C_WHITE, 2);
}
static epet_page_t my_page = { .title = "MINE", .render = my_render };
epet_ui_register(&ui, &my_page);
```

`update()` returning false closes the page. A page keeps private state by
embedding `epet_page_t` as its first member and casting `self` (see the
action pages in `epet_pages.c`).

### Adding a module

Subscribe to a mask of event types; keep state in your own ctx. `epet_care`
is the worked example -- it watches need events, clears them when the need is
met, and publishes `EPET_EV_ATTENTION` (rate limited) which the platform turns
into a screen wake.

```c
epet_bus_subscribe(&bus, EPET_EV_MASK(EPET_EV_POOPED), on_poop, &my_state, "mine");
```

Things to know about the bus:

- **Events are queued, not dispatched inline**, so a handler may publish
  without recursion. Call `epet_bus_dispatch()` once per loop.
- **A runaway handler cannot wedge dispatch** -- `EPET_DISPATCH_BUDGET` caps
  the drain, and the remainder stays queued.
- **Queue overflow drops the NEWEST event** and counts it in `bus.dropped`.
  Dropping the newest keeps already-queued work intact. Check that counter if
  events go missing.
- **Need events latch with hysteresis** so HUNGRY fires once per episode, not
  every frame. See `cross()` in `epet_state.c`.
- **`epet_advance()` publishes one MINUTE per elapsed minute**, so a long
  light sleep replays them all rather than collapsing to one.

## Tests

```bash
cd sim && make test
```

Three suites, all against `epet_core` directly -- no SDL, no hardware:

- `tests/test_power.c` -- display blanking, wake-swallow, long-gap catch-up
- `tests/test_events.c` -- bus semantics, event latching, care module
- `tests/test_ui.c` -- menu scrolling and wrapping, page lifecycle
- `tests/test_character.c` -- sprite transparency, pose lookup and fallback,
  the animation player, temperament divergence, random birth
- `tests/test_modules.c` -- install/remove, a module built outside the core,
  menu and character-pool effects, registry limits
- `tests/test_save.c` -- save round-trip, corruption and truncation, a class
  removed between runs, module-list restore, autosave timing
- `tests/test_character.c` also covers the dead pose, and growth: stage
  ordering, that every age 0..55 resolves to a drawable pose and a positive
  size, that the ramp gives more distinct sizes than there are stages, that
  size never falls with age, that a species with no stages is unaffected,
  that the actor's age actually follows the pet's (lose that wiring and the
  pet silently never grows), and that crossing a stage boundary keeps the
  animation's frame and phase
- `tests/test_vm.c` -- what the VM refuses to load and what it refuses to let
  a loaded page do: bad opcodes, wild jumps, stack underflow, missing syscall
  arguments, runaway loops
- `tests/test_link.c` -- pack parsing and every way it can be malformed, the
  install protocol end to end, surviving a restart, a loaded bytecode page
  actually running, and a pet whose class is removed under it. Needs
  `web/spark.epmod`, which `make test` builds.

A timer wake must NOT light the screen; only a press or an alert does.

## Gotchas

- Include SDL as `<SDL.h>`, not `<SDL2/SDL.h>` — `sdl2-config --cflags`
  already adds the SDL2 directory.
- Decay rates at the top of `epet_state.c` are tuned **fast** for development
  (full life cycle in ~2.5 min). Slow them down substantially for real play.
- The ESP-IDF install is ~3.4 GB. `~/.espressif/dist` holds the downloaded
  archives and is safe to delete once the toolchain is installed.
- The ST7789 retains its last frame in GRAM across an ESP32 reset. A stale
  image on screen is not evidence that a flash succeeded — check the serial log.
- Adding a `Kconfig.projbuild` does not take effect on an incremental build;
  run `idf.py reconfigure` or the new `CONFIG_*` symbols come back undeclared.
