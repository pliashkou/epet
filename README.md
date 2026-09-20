# epet

A Tamagotchi-style virtual pet for the **Waveshare ESP32-S3-LCD-1.3**, with a
module system that lets you install new characters and sub-programs from a web
browser over USB or Bluetooth.

The pet lives on a 240×240 colour LCD, sleeps when you leave it alone, wakes
when you shake it, and keeps living across reboots.

---

## What it does

**The pet.** Hunger, happiness, energy, hygiene and health drift over time.
Neglect it and it sickens and dies; shake a dead one and a new creature
hatches with a randomly rolled class. Each class has its own sprites,
animations, home backdrop, temperament and even its own mess.

**It grows up.** A pet has an age level from 1 to 50, and a character can
change size and shape across that range. The built-in classes hatch small and
round, fill out, and end up broad and stooped; the size ramps smoothly
between stages, so every age level is a slightly different creature. Growing
is a main-screen thing — pages draw the pet at a fixed size so their layouts
stay put.

**Four buttons**, named by position rather than meaning: two down the left of
the screen, two down the right.

| Button | Menu hidden | Menu shown | Inside a page |
|---|---|---|---|
| LT | reveal menu | previous item (wraps) | page's choice |
| LB | reveal menu | next item (wraps) | page's choice |
| RT | reveal menu | select | page's choice |
| RB | — | — | back |

**A translucent icon menu** down the left edge that fades out when unused, so
the pet is unobstructed.

**Power.** The screen blanks after 30 s and the CPU drops into light sleep,
waking on a button, on serial traffic, or on a timer to keep the simulation
moving. Shake it and the screen lights.

**Modules.** Characters and sub-programs ship as `.epmod` packs installed from
a browser. They survive reboots. A pack can carry real bytecode, so a module
is a program, not just a screen.

**Firmware updates** over the same link, into a spare app slot with bootloader
rollback.

---

## Installation

You need [ESP-IDF v5.5](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/)
and an ESP32-S3-LCD-1.3.

```bash
git clone <this repo> epet && cd epet
. /path/to/esp-idf/export.sh
idf.py build
idf.py -p $PORT -b 460800 flash monitor
```

> **Flash over the CH343 UART bridge**, not the chip's native USB port. The
> bridge enumerates as `/dev/ttyUSB*` on Linux, `/dev/cu.wchusbserial*` on
> macOS and a `COM` port on Windows. The native USB port is not usable here:
> it is not wired to the Type-C connector, and the firmware switches its PHY
> off anyway to free GPIO20 for the backlight (see **Hardware notes**).

The partition table uses two 3 MB app slots for OTA, so if you are coming from
a different layout, erase first:

```bash
idf.py -p $PORT erase-flash
```

### The simulator

The whole pet — simulation, menu, pages, animations — is platform-independent
and builds against SDL2 on any desktop. It renders a mock device with
clickable buttons.

```bash
# install SDL2 with your package manager, then:
cd sim && make && ./epet_sim
```

Keys: `Q`/`A`/`P`/`L` for LT/LB/RT/RB, `R` reset, `S` screenshot, `[`/`]`
speed, `Esc` quit. `--save-dir DIR` persists the pet, `--verbose` logs events.

Headless rendering, useful in CI or for checking artwork:

```bash
./epet_sim --headless --shots 0,60000 --out /tmp/shots   # panel only
./epet_sim --headless --device --shots 0 --out /tmp      # whole mock device
./epet_sim --headless --page STATS --shots 300 --out /tmp
```

### Tests

```bash
cd sim && make test
```

Eight suites covering the simulation, the event bus, the menu, characters,
modules, persistence, the bytecode VM and the install protocol. No hardware
required.

---

## The module system

### Installing from a browser

```bash
python3 -m http.server -d web 8000     # then open http://localhost:8000
```

Chrome or Edge, over `localhost` or https — Web Serial and Web Bluetooth both
refuse `file://`.

- **By cable** — *Connect by cable*, pick the `wchusbserial` port.
- **By Bluetooth** — open **UPDATE** on the pet's own menu first. The radio is
  off until you do, and switches off when you leave that page, so the device
  is not discoverable while it is just being a pet.

The **Install** page takes an `.epmod`; **Manage** reads what is on the device
and removes packs; **Firmware** flashes a new `.bin`.

A sample pack is included: [`web/spark.epmod`](web/spark.epmod) adds a third
character (SPARK, a flame that burns bright and tires fast, with its own
hearth backdrop and ember droppings), a bytecode page, and a background event
handler.

---

## Writing a pack

A pack is a binary file containing **characters** (sprites, animations,
backdrops, temperament), **pages** (sub-programs) and **hooks** (background
event handlers). Build one with the Python tools in `tools/`:

```bash
python3 tools/make_module.py my.epmod
```

`tools/make_module.py` is a worked example — read it alongside this section.

### Characters

A character is data. Frames are 8bpp palette-indexed bitmaps where **index 0
is transparent**; indices 1–15 look up in the species palette, so two species
can share sprite structure and look completely different.

A **pose** is a named sequence of frames with per-frame hold times. Poses are
resolved **by name**, so adding one needs no firmware change:

| Pose | Loops | When |
|---|---|---|
| `idle` | yes | default |
| `birth` | no | a new pet hatches |
| `happy` | no | fed, played with |
| `sad` | yes | mood is sad or sick |
| `dead` | yes | the pet has died |

Unknown names fall back to `idle`, so an older character can never crash on a
pose added for a newer one.

A character also carries:

- a **palette** (16 RGB565 entries, index 0 unused)
- one or more **backdrops** — 80×80 images scaled 3×, each with its own
  palette and a night tint; which one a pet gets is rolled at birth
- a **poop sprite**, drawn in the body palette
- a **temperament**: per-stat decay multipliers, so a class is a behaviour and
  not just a recolour
- **growth stages**, below

#### Growth stages

A pet's age level runs 1–50. A character may declare stages across that
range, each giving a size and, optionally, its own artwork:

| Field | Meaning |
|---|---|
| `from_age` | first age level this stage covers, 1–50 |
| `scale_pct` | on-screen size; 100 is one sprite pixel per screen pixel |
| `pose0`, `n_poses` | this stage's poses, or `0, 0` to inherit |

Two things make this cheap. **Size is interpolated between stages**, so a
character that declares four stages still looks slightly different at all
fifty ages — you do not need fifty sets of artwork to get fifty sizes. And
**a stage with no artwork costs six bytes**, so a growth spurt with no new
art is nearly free; a character *can* declare all fifty stages.

A stage may also define only *some* poses. What it leaves out falls back to
the character's own poses, drawn at the stage's size — so a baby that only
draws `idle` and `birth` still bounces for `happy`, just small.

The sample pack does one of each: `sparklet` draws two poses, `spark` draws
the full set, and the last stage carries no artwork and only grows. See
`SPARK_GROWTH` in [tools/make_module.py](tools/make_module.py).

Sizes are capped by the panel — the sprite is anchored by its feet on a fixed
ground line, and the build fails rather than letting a character grow up into
the status bar. With the standard 44×56 canvas the ceiling is about 350%.

### Pages

Two kinds.

**Declarative** — a title, an icon id, and rows naming what to show. Enough
for an information screen, no logic:

```python
rows = [
    (SPRITE, SRC["none"], ""),
    (STRING, SRC["species_name"], "CLASS"),
    (BAR,    SRC["health"],       "HP"),
    (VALUE,  SRC["age_s"],        "AGE"),
]
```

**Bytecode** — a real program with branching, arithmetic, its own state and
input handling, assembled by `tools/vmasm.py`:

```
update:                  ; args: 0 = dt_ms, 1 = button edge mask
  arg 1
  push $BTN_RT
  and
  jz done
  sysv act $ACT_FEED     ; push args, call, keep the result
  jz done
  emote "happy"
done:
  ret 1                  ; non-zero keeps the page open

render:
  fill $PANEL
  text 84 6 "VITALS" $WHITE 2
  sprite 34 74 1
  push 58
  push 103
  push 150
  push 9
  sysv get $SRC_FED      ; push a value...
  dup
  sysk levelcol          ; ...and derive a colour from it
  sys bar
  ret 1
```

Three call forms, and mixing them up is the easy mistake:

| form | pushes args | keeps result |
|---|---|---|
| `text 8 8 "HI" $WHITE 2` | yes | no (dropped) |
| `sysv get $SRC_HEALTH` | yes | yes |
| `sysk levelcol` | no, already on the stack | yes |

Page hooks are `enter`, `update(dt, buttons)`, `render` and `leave`. Any may
be omitted. `RB` always closes a page before the page sees it, so no page can
trap the user.

### Background hooks

A pack may declare handlers that run **whether or not any of its pages are
open** — this is what makes a pack a module rather than a screen:

```
on_event:                ; args: 0 = event type, 1 = a, 2 = b, 3 = age_s
  arg 0
  push $EV_POOPED
  eq
  jz done
  loadg 3                ; module globals, shared with this pack's pages
  push 1
  add
  storeg 3
done:
  ret 1
```

Subscribe by mask when you emit the hook. Available events:

`MINUTE` · `FED` · `PLAYED` · `CLEANED` · `POOPED` · `HUNGRY` · `SAD` ·
`DIRTY` · `SICK` · `RECOVERED` · `FELL_ASLEEP` · `WOKE` · `DIED` · `REBORN` ·
`DISPLAY_ON` · `DISPLAY_OFF` · `ATTENTION` · `BUTTON`

Need events latch with hysteresis, so `HUNGRY` fires once per episode rather
than every frame.

A hook runs with **no framebuffer**, so drawing syscalls are no-ops. What it
can do is read the pet, apply actions, play a pose, and call `nudge <ms>` to
ask for the screen. Globals are module-wide and shared with the pack's pages,
so a page can display what the hook counted while it was closed.

### Sandboxing

Loaded bytecode is untrusted code on a device with no memory protection, so:

- no raw memory access — the framebuffer is reachable only through drawing
  syscalls, which clip
- every jump target, local, global, string id, syscall id and arity is
  verified at load; code that fails is never run
- **operand-stack depth is tracked through every path** at load time, which
  catches the commonest authoring mistake: calling a syscall without pushing
  its arguments
- an instruction budget per hook, so a runaway page cannot hang the device

Native code cannot be loaded. There is no dynamic linker and one bad pointer
takes the board down; bytecode gives modules genuine behaviour without that
risk.

---

## Layout

```
components/epet_core/   everything platform-independent
  epet_state.c            the simulation: needs, health, sleep, display power
  epet_event.[ch]         publish/subscribe bus, no allocation
  epet_draw.[ch]          framebuffer primitives and a 5x7 font
  epet_sprite.[ch]        transparent blitter
  epet_species.[ch]       character classes, poses, animation player
  epet_ui.[ch]            menu, scrolling, page lifecycle
  epet_pages.c            built-in sub-programs
  epet_vm.[ch]            bytecode VM and its validator
  epet_modblob.[ch]       .epmod parser
  epet_link.[ch]          install protocol (transport agnostic)
  epet_save.[ch]          persistence
main/                   ESP32 platform: LCD, buttons, IMU, BLE, OTA, storage
sim/                    host platform: SDL2 window, file storage
tools/                  sprite generator, bytecode assembler, pack builder
web/                    the browser installer
tests/                  eight suites, host-run
```

The firmware and the simulator compile the **same** `epet_core` sources, so
they cannot drift. New behaviour belongs in `epet_core`, never in a platform
layer.

---

## Hardware notes

Pins, taken from the board's
[schematic](https://files.waveshare.com/wiki/ESP32-S3-LCD-1.3/ESP32S3_1.3inch.pdf)
rather than from the vendor's demo code, which is wrong about the backlight:

| Signal | GPIO |
|---|---|
| LCD MOSI / SCLK / CS / DC / RST | 41 / 40 / 39 / 38 / 42 |
| LCD backlight (`BL_PWM`) | 20 |
| IMU I²C SDA / SCL | 47 / 48 |
| BOOT button (active low) | 0 |

**The backlight is GPIO20, and it hides behind the USB PHY.** `BL_PWM` drives
a 1 kΩ resistor into Q5, an MMBT3904 whose collector feeds the panel's `LEDA`,
so the pin is **active high**. But GPIO19 and GPIO20 are also the ESP32-S3's
native USB D− and D+, and the ROM leaves `USB_SERIAL_JTAG_CONF0_REG`'s
`USB_PAD_ENABLE` bit set — which lets the USB PHY drive both pads through the
GPIO matrix. Until that bit is cleared, every write to GPIO20 is accepted and
silently discarded, and the backlight appears to be hardwired on:

```c
usb_serial_jtag_ll_phy_enable_pad(false);   /* hal/usb_serial_jtag_ll.h */
gpio_reset_pin(20);
```

That costs nothing on this board, because the Type-C connector goes to the
CH343P bridge rather than to the chip's own USB. The vendor factory program
names GPIO4 as the backlight; GPIO4 is not in the backlight net at all.

Other details that cost real debugging time are in [CLAUDE.md](CLAUDE.md): the
LCD's 80-row GRAM offset, the IMU's I²C timeout trap, why shake detection
measures sample-to-sample change rather than deviation from gravity, and why
nothing here may delay for less than one 10 ms FreeRTOS tick.

## Licence

MIT
