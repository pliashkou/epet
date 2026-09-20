#!/usr/bin/env python3
"""Build an .epmod pack: a third character with animations, plus an info page.

    python3 tools/make_module.py web/spark.epmod
"""
import struct
import sys
import os
from vmasm import Asm

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_sprites import Canvas, eyes, mouth, BODY_DY, W, H   # reuse the art helpers

MAGIC = b"EPMOD"
FMT_VERSION = 3
HEADER = 64

# row kinds / sources -- keep in step with epet_dynpage.h
GAP, TEXT, VALUE, BAR, STRING, SPRITE = range(6)
SRC = {
    "none": 0, "hunger": 1, "fed": 2, "happiness": 3, "energy": 4,
    "hygiene": 5, "health": 6, "age_s": 7, "poop": 8,
    "species_name": 9, "species_blurb": 10, "module_id": 11,
    "backdrop_name": 12, "pose_name": 13, "mood": 14,
    "temper_hunger": 15, "temper_happy": 16, "temper_energy": 17,
    "temper_hygiene": 18, "n_backdrops": 19, "n_poses": 20,
    "sprite_scale": 21, "species_count": 22, "module_count": 23,
}


def fnv1a(b):
    h = 2166136261
    for x in b:
        h = ((h ^ x) * 16777619) & 0xFFFFFFFF
    return h


def pad(s, n):
    b = s.encode("ascii")[: n - 1]
    return b + b"\0" * (n - len(b))


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


# ---- the character: SPARK, a little flame -------------------------------

def spark_body(cv, cx, cy, rx, ry, feet=True):
    # teardrop flame body: wide at the base, tapering to a tip
    cv.ellipse(cx, cy + 2, rx, ry - 2, 2)
    for i in range(ry):
        w = max(1, int(rx * (1 - i / ry) ** 0.7))
        cv.rect(cx - w // 2, cy - ry + 2 + i - 6, w, 1, 2)
    cv.ellipse(cx - rx // 3, cy + 2, rx // 3, ry // 3, 3)      # inner glow
    cv.ellipse(cx, cy + ry - 3, rx - 4, 3, 4)                  # base shadow
    if feet:
        cv.ellipse(cx - rx // 2, cy + ry - 2, 4, 3, 2)
        cv.ellipse(cx + rx // 2, cy + ry - 2, 4, 3, 2)


def frame(cy=24, rx=15, ry=15, eye="open", mth="flat", spread=8, feet=True, fx=None):
    cy += BODY_DY
    cv = Canvas()
    spark_body(cv, 22, cy, rx, ry, feet)
    eyes(cv, 22, cy - 3, eye, spread)
    mouth(cv, 22, cy + 7, mth)
    if fx:
        fx(cv)
    cv.outline()
    return cv


def embers(cv):
    for (x, y, r) in ((8, 14 + BODY_DY, 1), (35, 17 + BODY_DY, 1), (31, 8 + BODY_DY, 2)):
        cv.disc(x, y, r, 8)


POSES = {
    "idle":  [(frame(ry=15), 700), (frame(cy=23, ry=16), 700),
              (frame(ry=15), 500), (frame(ry=15, eye="closed"), 130)],
    "birth": [(frame(cy=32, rx=4, ry=4, spread=3, feet=False, eye="closed"), 260),
              (frame(cy=29, rx=8, ry=8, spread=5, feet=False, eye="closed"), 260),
              (frame(cy=26, rx=12, ry=12, spread=7, eye="closed"), 280),
              (frame(cy=24, rx=15, ry=15, eye="open", mth="o", fx=embers), 380),
              (frame(cy=24, rx=15, ry=15, eye="happy", mth="smile", fx=embers), 460)],
    "happy": [(frame(cy=26, rx=16, ry=13, eye="happy", mth="smile"), 130),
              (frame(cy=19, rx=14, ry=18, eye="happy", mth="grin", fx=embers), 200),
              (frame(cy=23, rx=15, ry=15, eye="happy", mth="grin", fx=embers), 200),
              (frame(cy=25, rx=16, ry=14, eye="happy", mth="smile"), 200)],
    "sad":   [(frame(cy=27, rx=16, ry=12, eye="sad", mth="frown"), 700),
              (frame(cy=28, rx=17, ry=11, eye="sad", mth="frown"), 700)],
}
POSE_ORDER = ["idle", "birth", "happy", "sad"]

SPARK_PAL = [0, rgb565(60, 20, 10), rgb565(255, 140, 40), rgb565(255, 220, 110),
             rgb565(200, 80, 20), 0xFFFF, rgb565(40, 15, 10), rgb565(70, 25, 15),
             rgb565(255, 240, 180)] + [0] * 7


# ---- its home: a hearth ---------------------------------------------------

def bg_hearth():
    cv = Canvas(80, 80)
    cv.band(0, 66, 1)                                  # dark stone
    for y in range(4, 64, 9):                          # brick courses
        off = 0 if (y // 9) % 2 == 0 else 6
        cv.rect(0, y, 80, 1, 2)
        for x in range(off, 80, 12):
            cv.rect(x, y, 1, 9, 2)
    cv.ellipse(40, 70, 34, 12, 3)                      # hearth floor
    cv.band(66, 80, 3)
    for (x, w) in ((18, 22), (44, 26)):                # logs
        cv.rect(x, 62, w, 5, 4)
    for (x, y, r) in ((30, 60, 3), (46, 58, 4), (38, 55, 3)):
        cv.disc(x, y, r, 5)                            # embers
    return cv


HEARTH_PAL = [0, rgb565(48, 44, 52), rgb565(32, 30, 36), rgb565(70, 60, 58),
              rgb565(90, 60, 35), rgb565(255, 150, 50)] + [0] * 10


# ---- encoding -------------------------------------------------------------

def enc_frame(cv):
    return bytes([cv.w, cv.h]) + bytes(cv.px)


def enc_pose(name, loop, keys):
    out = pad(name, 12) + bytes([1 if loop else 0, len(keys)])
    for (fi, hold) in keys:
        out += bytes([fi]) + struct.pack("<H", hold)
    return out


def enc_backdrop(name, frame_idx, scale, night, night_alpha, pal):
    out = pad(name, 12) + bytes([frame_idx, scale])
    out += struct.pack("<H", night) + bytes([night_alpha])
    for c in (pal + [0] * 16)[:16]:
        out += struct.pack("<H", c)
    return out


def enc_species(name, blurb, pal, scale, temper, pose0, n_poses, bd0, n_bd,
                poop_frame=0xFF):
    out = pad(name, 12) + pad(blurb, 28)
    for c in (pal + [0] * 16)[:16]:
        out += struct.pack("<H", c)
    out += bytes([scale])
    out += struct.pack("<ffff", *temper)
    out += bytes([pose0, n_poses, bd0, n_bd, poop_frame])
    return out


# ---- the module's actual program ------------------------------------------
# Two tabs of live information plus a working Feed button, written as real
# bytecode: arithmetic, branching, per-page state and syscalls.

PROGRAM = """
; ============ on_event(type, a, b, age_s) ============
; Runs whether or not any of this module's pages are open. No framebuffer
; here, so drawing is a no-op; what it can do is watch, count, and ask to be
; seen. Globals are shared with the pages, so VITALS can display the tally.
on_event:
  arg 0
  push $EV_POOPED
  eq
  jz e_died
  loadg 3                ; g3: piles seen since the module was installed
  push 1
  add
  storeg 3
  jmp e_end
e_died:
  arg 0
  push $EV_DIED
  eq
  jz e_hungry
  loadg 4
  push 1
  add
  storeg 4               ; g4: deaths witnessed
  nudge 6000             ; light the screen: this one deserves attention
  jmp e_end
e_hungry:
  arg 0
  push $EV_HUNGRY
  eq
  jz e_end
  loadg 5
  push 1
  add
  storeg 5               ; g5: times it went hungry
e_end:
  ret 1

; ============ update(dt_ms, edge_mask) ============
update:
  arg 1
  push $BTN_LT
  and
  jz u_lb
  push 1
  loadg 0
  sub                    ; tab = 1 - tab
  storeg 0
u_lb:
  arg 1
  push $BTN_LB
  and
  jz u_rt
  push 1
  loadg 0
  sub
  storeg 0
u_rt:
  arg 1
  push $BTN_RT
  and
  jz u_tick
  sysv act $ACT_FEED     ; feed, and only count it if it was accepted
  jz u_tick
  loadg 1
  push 1
  add
  storeg 1
  emote "happy"
u_tick:
  loadg 2                ; a frame counter, for the blinking cursor
  arg 0
  add
  storeg 2
  ret 1

; ============ render() ============
render:
  fill $PANEL
  rect 0 0 $W 26 $ACCENT
  text 84 6 "VITALS" $WHITE 2

  sprite 34 74 1
  textv 66 40 $SRC_SPECIES_NAME $WHITE 1
  textv 66 54 $SRC_MOOD $GOOD 1
  text 66 68 "FED" $DIM 1
  number 96 68 0 $DIM 1

  loadg 0
  jnz traits

; ---- tab 0: live meters ----
  text 10 104 "FED" $WHITE 1
  push 58
  push 103
  push 150
  push 9
  sysv get $SRC_FED
  dup
  sysk levelcol
  sys bar

  text 10 120 "JOY" $WHITE 1
  push 58
  push 119
  push 150
  push 9
  sysv get $SRC_HAPPINESS
  dup
  sysk levelcol
  sys bar

  text 10 136 "PEP" $WHITE 1
  push 58
  push 135
  push 150
  push 9
  sysv get $SRC_ENERGY
  dup
  sysk levelcol
  sys bar

  text 10 152 "HP" $WHITE 1
  push 58
  push 151
  push 150
  push 9
  sysv get $SRC_HEALTH
  dup
  sysk levelcol
  sys bar

; vitality = (health + joy + energy) / 3, computed here rather than shipped
  text 10 174 "VITALITY" $WHITE 1
  sysv get $SRC_HEALTH
  sysv get $SRC_HAPPINESS
  add
  sysv get $SRC_ENERGY
  add
  push 3
  div
  storel 0
  push 84
  push 173
  push 124
  push 11
  loadl 0
  loadl 0
  sysk levelcol
  sys bar
  push 216
  push 174
  loadl 0
  push $WHITE
  push 1
  sys number
  jmp foot

; ---- tab 1: what this class is like ----
traits:
  text 10 104 "HOME" $WHITE 1
  textv 84 104 $SRC_BACKDROP_NAME $GOOD 1
  text 10 118 "POSE" $WHITE 1
  textv 84 118 $SRC_POSE_NAME $GOOD 1
  text 10 132 "FROM" $WHITE 1
  textv 84 132 $SRC_MODULE_ID $GOOD 1

  text 10 150 "APPETITE" $WHITE 1
  sysv get $SRC_TEMPER_HUNGER
  storel 0
  push 84
  push 150
  loadl 0
  push $WHITE
  push 1
  sys number
  loadl 0
  push 100
  gt
  jz ap_norm
  text 120 150 "HUNGRY SORT" $WARN 1
  jmp ap_done
ap_norm:
  text 120 150 "EASY EATER" $DIM 1
ap_done:

  text 10 164 "RESTLESS" $WHITE 1
  sysv get $SRC_TEMPER_ENERGY
  storel 1
  push 84
  push 164
  loadl 1
  push $WHITE
  push 1
  sys number

  text 10 182 "POSES" $WHITE 1
  push 84
  push 182
  sysv get $SRC_N_POSES
  push $DIM
  push 1
  sys number

; counts gathered by the background hook while this page was closed
  text 10 196 "SEEN" $WHITE 1
  push 66
  push 196
  loadg 3
  push $GOOD
  push 1
  sys number
  text 84 196 "POOP" $DIM 1
  push 132
  push 196
  loadg 5
  push $GOOD
  push 1
  sys number
  text 150 196 "HUNGRY" $DIM 1

; ---- shared footer ----
foot:
  rect 0 222 $W 18 $MENU
  loadg 0
  jnz f_two
  text 6 227 "TAB 1/2 STATS" $DIM 1
  jmp f_hint
f_two:
  text 6 227 "TAB 2/2 TRAITS" $DIM 1
f_hint:
  text 108 227 "LT LB TAB" $DIM 1
  text 174 227 "RT FEED" $GOOD 1

; blink a cursor, so it is obvious this is running code
  loadg 2
  push 1000
  mod
  push 500
  lt
  jz r_end
  rect 232 226 4 8 $WHITE
r_end:
  ret 1
"""


def build_program():
    a = Asm()
    code = a.assemble(PROGRAM)
    return a, code


def enc_hook(mask, pc):
    return struct.pack("<II", mask, pc)


def enc_vmpage(title, icon, entries):
    out = pad(title, 12) + bytes([icon, 0])
    for e in entries:                      # enter, update, render, leave
        out += struct.pack("<I", e)
    return out


def enc_page(title, icon, rows):
    out = pad(title, 12) + bytes([icon, len(rows)])
    for (kind, src, label) in rows:
        out += bytes([kind, src]) + pad(label, 14)
    return out


def spark_embers():
    """What a flame leaves behind: a little heap of glowing ash."""
    cv = Canvas(16, 14)
    cv.ellipse(8, 11, 7, 3, 7)          # ash heap
    cv.ellipse(6, 9, 3, 2, 4)
    cv.disc(10, 8, 2, 2)                # still glowing
    cv.put(5, 7, 8)
    cv.outline()
    return cv


def build():
    frames, poses = [], []

    for pname in POSE_ORDER:
        keys = []
        for cv, hold in POSES[pname]:
            keys.append((len(frames), hold))
            frames.append(cv)
        poses.append((pname, pname in ("idle", "sad"), keys))

    bg_idx = len(frames)
    frames.append(bg_hearth())

    poop_idx = len(frames)
    frames.append(spark_embers())

    payload = b""
    for cv in frames:
        payload += enc_frame(cv)
    for (n, loop, keys) in poses:
        payload += enc_pose(n, loop, keys)
    payload += enc_backdrop("HEARTH", bg_idx, 3, rgb565(20, 26, 70), 150, HEARTH_PAL)
    payload += enc_species(
        "SPARK", "BURNS BRIGHT. TIRES FAST.", SPARK_PAL, 2,
        (1.15, 0.9, 1.4, 0.8), 0, len(poses), 0, 1, poop_idx)

    rows = [
        (SPRITE, SRC["none"], ""),
        (STRING, SRC["species_name"], "CLASS"),
        (STRING, SRC["module_id"], "FROM"),
        (STRING, SRC["backdrop_name"], "HOME"),
        (STRING, SRC["mood"], "MOOD"),
        (STRING, SRC["pose_name"], "POSE"),
        (GAP, 0, ""),
        (VALUE, SRC["temper_hunger"], "APPETITE"),
        (VALUE, SRC["temper_energy"], "RESTLESS"),
        (VALUE, SRC["n_poses"], "POSES"),
        (VALUE, SRC["n_backdrops"], "HOMES"),
    ]
    payload += enc_page("TRAITS", 2, rows)

    # bytecode section
    asm, code = build_program()
    payload += struct.pack("<I", len(code)) + code
    payload += asm.string_section()
    payload += enc_vmpage("VITALS", 3, [0, asm.labels["update"],
                                        asm.labels["render"], 0])

    # background hook: the events this module watches while closed
    watched = (1 << 4) | (1 << 12) | (1 << 5)      # POOPED | DIED | HUNGRY
    payload += enc_hook(watched, asm.labels["on_event"])

    header = bytearray(HEADER)
    header[0:5] = MAGIC
    header[5] = FMT_VERSION
    header[8:20] = pad("spark", 12)
    header[20:44] = pad("SPARK PACK", 24)
    struct.pack_into("<H", header, 44, 1)
    header[46] = len(frames)
    header[47] = len(poses)
    header[48] = 1          # backdrops
    header[49] = 1          # species
    header[50] = 1          # declarative pages
    header[51] = 1          # bytecode pages
    header[60] = 1          # background hooks
    struct.pack_into("<I", header, 52, len(payload))
    struct.pack_into("<I", header, 56, fnv1a(payload))
    return bytes(header) + payload


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "spark.epmod"
    blob = build()
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "wb") as f:
        f.write(blob)
    print("wrote %s (%d bytes, %.1f KB base64)" % (out, len(blob), len(blob) * 4 / 3 / 1024))
