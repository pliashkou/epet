#!/usr/bin/env python3
"""Generate epet sprite data.

Frames are 8bpp palette-indexed, index 0 transparent. Both species share the
same index meanings so they can share sprite structure with different colours:

    1 outline      2 body        3 highlight   4 shade
    5 eye white    6 pupil       7 mouth/dark  8 accent   9 accent dark

Run:  python3 tools/gen_sprites.py > components/epet_core/epet_sprites.c
"""
import sys

W = 44
# Taller than wide: SPROUT's stem and leaf sit ABOVE the body, and in the
# tall poses (the happy bounce) they ran off the top of a 44x44 canvas and
# were clipped. BODY_DY pushes every body down into that headroom.
H = 56
BODY_DY = 9
SCALE = 2

BG_W = BG_H = 80          # backdrop source size
BG_SCALE = 3              # 80 * 3 = 240, exactly the panel


class Canvas:
    def __init__(self, w=W, h=H):
        self.w, self.h = w, h
        self.px = [0] * (w * h)

    def put(self, x, y, i):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y * self.w + x] = i

    def get(self, x, y):
        if 0 <= x < self.w and 0 <= y < self.h:
            return self.px[y * self.w + x]
        return 0

    def ellipse(self, cx, cy, rx, ry, idx):
        if rx <= 0 or ry <= 0:
            return
        for y in range(cy - ry, cy + ry + 1):
            for x in range(cx - rx, cx + rx + 1):
                dx, dy = (x - cx) / rx, (y - cy) / ry
                if dx * dx + dy * dy <= 1.0:
                    self.put(x, y, idx)

    def disc(self, cx, cy, r, idx):
        self.ellipse(cx, cy, r, r, idx)

    def rect(self, x, y, w, h, idx):
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                self.put(xx, yy, idx)

    def outline(self, idx=1):
        """Wrap every opaque region in a 1px border."""
        src = list(self.px)
        w, h = self.w, self.h
        for y in range(h):
            for x in range(w):
                if src[y * w + x] != 0:
                    continue
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    nx, ny = x + dx, y + dy
                    if 0 <= nx < w and 0 <= ny < h and src[ny * w + nx] not in (0, idx):
                        self.put(x, y, idx)
                        break

    def band(self, y0, y1, idx):
        for y in range(y0, y1):
            for x in range(self.w):
                self.put(x, y, idx)


def eyes(cv, cx, ey, state, spread=9):
    for side in (-1, 1):
        x = cx + side * spread
        if state == "open":
            cv.disc(x, ey, 5, 5)
            cv.disc(x + side, ey + 1, 3, 6)
            cv.put(x + side - 1, ey - 1, 5)
        elif state == "closed":
            cv.rect(x - 4, ey, 9, 2, 6)
        elif state == "happy":           # ^ ^
            for i in range(5):
                cv.put(x - 4 + i, ey + 2 - i, 6)
                cv.put(x - 4 + i, ey + 3 - i, 6)
                cv.put(x + 4 - i, ey + 2 - i, 6)
                cv.put(x + 4 - i, ey + 3 - i, 6)
        elif state == "dead":            # crossed out
            for i in range(-4, 5):
                cv.put(x + i, ey + i, 6)
                cv.put(x + i + 1, ey + i, 6)
                cv.put(x + i, ey - i, 6)
                cv.put(x + i + 1, ey - i, 6)
        elif state == "sad":             # drooping
            cv.disc(x, ey + 1, 4, 5)
            cv.disc(x + side, ey + 2, 2, 6)
            cv.rect(x - 5, ey - 4, 10, 2, 6)


def mouth(cv, cx, my, kind):
    if kind == "flat":
        cv.rect(cx - 5, my, 11, 2, 7)
    elif kind == "smile":
        for i in range(-6, 7):
            d = (6 - abs(i)) // 2
            cv.rect(cx + i, my + d, 1, 2, 7)
    elif kind == "grin":
        cv.ellipse(cx, my + 1, 7, 5, 7)
        cv.rect(cx - 7, my - 4, 15, 4, 0)
    elif kind == "frown":
        for i in range(-6, 7):
            d = (6 - abs(i)) // 2
            cv.rect(cx + i, my + 4 - d, 1, 2, 7)
    elif kind == "o":
        cv.disc(cx, my + 1, 3, 7)


def blob_body(cv, cx, cy, rx, ry, feet=True):
    cv.ellipse(cx, cy, rx, ry, 2)
    cv.ellipse(cx - rx // 3, cy - ry // 3, rx // 3, ry // 4, 3)
    cv.ellipse(cx, cy + ry - 2, rx - 3, 3, 4)
    if feet:
        cv.ellipse(cx - rx // 2, cy + ry - 1, 5, 3, 2)
        cv.ellipse(cx + rx // 2, cy + ry - 1, 5, 3, 2)


def sprout_body(cv, cx, cy, rx, ry, feet=True):
    # stem and leaf first, so the body overlaps them cleanly
    cv.rect(cx - 1, cy - ry - 7, 2, 8, 9)
    for i in range(7):
        cv.ellipse(cx + 5 + i // 2, cy - ry - 6 + i, 5 - i // 2, 2, 8)
    cv.ellipse(cx, cy, rx, ry, 2)
    cv.ellipse(cx - rx // 3, cy - ry // 3, rx // 3, ry // 4, 3)
    cv.ellipse(cx, cy + ry - 2, rx - 3, 3, 4)
    if feet:
        cv.ellipse(cx - rx // 2, cy + ry - 1, 4, 3, 2)
        cv.ellipse(cx + rx // 2, cy + ry - 1, 4, 3, 2)


def frame(body_fn, cx=22, cy=24, rx=16, ry=15, eye="open", mth="flat",
          eye_y=None, mouth_y=None, spread=9, feet=True, fx=None):
    # Every caller works in the old 44-tall coordinate space; shift here so
    # the extra headroom is applied in exactly one place.
    cy += BODY_DY
    if eye_y is not None:
        eye_y += BODY_DY
    if mouth_y is not None:
        mouth_y += BODY_DY

    cv = Canvas()
    body_fn(cv, cx, cy, rx, ry, feet)
    ey = eye_y if eye_y is not None else cy - 4
    my = mouth_y if mouth_y is not None else cy + 6
    eyes(cv, cx, ey, eye, spread)
    mouth(cv, cx, my, mth)
    if fx:
        fx(cv)
    cv.outline()
    return cv


def sparkle(cv):
    for (x, y, r) in ((6, 8 + BODY_DY, 2), (37, 11 + BODY_DY, 2),
                      (34, 33 + BODY_DY, 1)):
        cv.disc(x, y, r, 8)


def tear(cv):
    cv.ellipse(10, 24 + BODY_DY, 2, 3, 8)
    cv.ellipse(34, 26 + BODY_DY, 2, 3, 8)


# ---- menu icons ---------------------------------------------------------
# Drawn as pixel art rather than composed from circles: shape primitives give
# lumpy silhouettes at this size, and every pixel matters on a 20x20 glyph.
#   '#' the icon's body      '+' a secondary tone      'o' a highlight
# The three map to palette indices 1/2/3, coloured per selection state.

ICON_W = ICON_H = 20

ICONS = {
    "feed": [                       # bowl of something warm
        "....................",
        "........++..........",
        ".......+..+.........",
        "........++..........",
        "......+....+........",
        ".......+..+.........",
        "....................",
        "..................o.",
        ".oooooooooooooooooo.",
        ".####################"[:20],
        ".##################.",
        "..################..",
        "..##############....",
        "...############.....",
        "....##########......",
        ".....########.......",
        "......######........",
        ".......####.........",
        "....................",
        "....................",
    ],
    "play": [                       # ball with a clear seam and a shine
        "....................",
        "....................",
        "......######........",
        "....##########......",
        "...###oo########....",
        "..####oo#########...",
        "..###############...",
        ".#######++#########.",
        ".#####++++++#######.",
        ".####++####++######.",
        ".###++######++#####.",
        ".##++########++####.",
        ".#++##########++###.",
        "..###############...",
        "..###############...",
        "...#############....",
        "....###########.....",
        "......#######.......",
        "....................",
        "....................",
    ],
    "wash": [                       # droplet with a shine
        "....................",
        ".........#..........",
        "........###.........",
        "........###.........",
        ".......#####........",
        ".......#####........",
        "......#######.......",
        "......##oo###.......",
        ".....###oo#####.....",
        ".....####o######....",
        "....############....",
        "....############....",
        "....############....",
        ".....##########.....",
        ".....##########.....",
        "......########......",
        ".......######.......",
        "........####........",
        "....................",
        "....................",
    ],
    "stats": [                      # bar chart
        "....................",
        "....................",
        "................###.",
        "................###.",
        "................###.",
        ".........###....###.",
        ".........###....###.",
        ".........###....###.",
        "..###....###....###.",
        "..###....###....###.",
        "..###....###....###.",
        "..###....###....###.",
        "..###....###....###.",
        "..###....###....###.",
        "..###....###....###.",
        "++++++++++++++++++++",
        "....................",
        "....................",
        "....................",
        "....................",
    ],
    "sleep": [                      # crescent moon and a star
        "....................",
        ".......#####....++..",
        ".....#########..++..",
        "....####....##......",
        "...####......#..++..",
        "..####..............",
        "..####..............",
        ".#####..............",
        ".#####..............",
        ".#####..............",
        ".#####..........++..",
        ".#####.........++++.",
        "..####..........++..",
        "..####..............",
        "...####......#......",
        "....####....##......",
        ".....#########......",
        ".......#####........",
        "....................",
        "....................",
    ],
    "class": [                      # two figures, one in front
        "....................",
        "....................",
        "..++++......####....",
        ".++++++....######...",
        ".++++++....######...",
        "..++++......####....",
        "....................",
        ".++++++++..########.",
        "++++++++++##########",
        "++++++++++##########",
        "++++++++++##########",
        "++++++++++##########",
        ".++++++++..########.",
        "..++++++....######..",
        "....................",
        "....................",
        "....................",
        "....................",
        "....................",
        "....................",
    ],
    "home": [                       # a little house
        "....................",
        "....................",
        ".........##.........",
        "........####........",
        ".......######.......",
        "......########......",
        ".....##########.....",
        "....############....",
        "...##############...",
        "..################..",
        ".##################.",
        "...##############...",
        "...####......####...",
        "...####......####...",
        "...####.oooo.####...",
        "...####.oooo.####...",
        "...####.oooo.####...",
        "...##############...",
        "....................",
        "....................",
    ],
    "mods": [                       # stacked blocks
        "....................",
        "....................",
        "..################..",
        "..################..",
        "..#++++++++++++++#..",
        "..################..",
        "....................",
        "..################..",
        "..################..",
        "..#++++++++++++++#..",
        "..################..",
        "....................",
        "..################..",
        "..################..",
        "..#++++++++++++++#..",
        "..################..",
        "....................",
        "....................",
        "....................",
        "....................",
    ],
    "about": [                      # info badge
        "....................",
        "......########......",
        "....############....",
        "...##############...",
        "..################..",
        "..######oooo######..",
        ".#######oooo#######.",
        ".#######oooo#######.",
        ".##################.",
        ".######oooooo######.",
        ".########oooo######.",
        ".########oooo######.",
        "..#######oooo#####..",
        "..#####oooooooo###..",
        "...##############...",
        "....############....",
        "......########......",
        "....................",
        "....................",
        "....................",
    ],
    "update": [                     # a beacon: solid core, two clean arcs
        "....................",
        "....................",
        "..++............++..",
        ".++..............++.",
        "++....++####++....++",
        "+....++......++....+",
        "+...++........++...+",
        "+..++...####...++..+",
        "+..+...######...+..+",
        "+..+...######...+..+",
        "+..++...####...++..+",
        "+...++........++...+",
        "+....++......++....+",
        "++....++####++....++",
        ".++..............++.",
        "..++............++..",
        "....................",
        "....................",
        "....................",
        "....................",
    ],
}


def icon_canvas(rows):
    cv = Canvas(ICON_W, ICON_H)
    mapping = {"#": 1, "+": 2, "o": 3}
    for y, row in enumerate(rows[:ICON_H]):
        for x, ch in enumerate(row[:ICON_W]):
            if ch in mapping:
                cv.put(x, y, mapping[ch])
    return cv


# ---- what each creature leaves behind -----------------------------------
# Small: 16x14, drawn at the species' own scale beside the pet.

POOP_W, POOP_H = 16, 14


def poop_blob():
    cv = Canvas(POOP_W, POOP_H)
    cv.ellipse(8, 11, 7, 3, 7)          # base coil
    cv.ellipse(8, 8, 5, 3, 7)
    cv.ellipse(7, 5, 3, 2, 7)
    cv.put(6, 4, 3)                     # highlight
    cv.outline()
    return cv


def poop_sprout():
    # a seed husk rather than a coil: different creature, different mess
    cv = Canvas(POOP_W, POOP_H)
    cv.ellipse(8, 9, 6, 4, 9)
    cv.ellipse(6, 7, 3, 2, 8)
    cv.rect(8, 2, 1, 4, 9)              # little stalk
    cv.outline()
    return cv


POOPS = { "blob": poop_blob, "sprout": poop_sprout }


# ---- backdrops ----------------------------------------------------------
# 80x80, scaled 3x. Horizon at row 66 puts the ground line at screen y=198,
# where the pet's feet are. Indices are backdrop-local: each backdrop carries
# its own palette, unrelated to the creature's body colours.

def bg_meadow():
    cv = Canvas(BG_W, BG_H)
    cv.band(0, 30, 1)                       # high sky
    cv.band(30, 52, 2)                      # low sky
    cv.disc(62, 14, 8, 3)                   # sun
    for (x, y, r) in ((14, 16, 6), (22, 18, 8), (30, 15, 5),
                      (48, 30, 5), (56, 32, 6)):
        cv.ellipse(x, y, r, max(2, r - 3), 4)
    cv.band(52, 66, 5)                      # far field: fills to both edges
    cv.ellipse(18, 52, 30, 10, 5)           # hills bulging up into the sky
    cv.ellipse(60, 54, 26, 9, 5)
    cv.band(66, BG_H, 6)                    # grass
    for x in range(0, BG_W, 7):             # grass tufts
        cv.put(x, 66, 7); cv.put(x + 3, 67, 7)
    for (x, y) in ((8, 72), (26, 76), (44, 71), (66, 75)):
        cv.disc(x, y, 1, 8)                 # flowers
    return cv


def bg_beach():
    cv = Canvas(BG_W, BG_H)
    cv.band(0, 26, 1)
    cv.band(26, 46, 2)
    cv.disc(16, 12, 7, 3)
    cv.band(46, 62, 4)                      # sea
    for y in range(48, 62, 4):               # wave crests
        for x in range((y // 2) % 6, BG_W, 9):
            cv.put(x, y, 5); cv.put(x + 1, y, 5)
    cv.band(62, 66, 6)                      # wet sand
    cv.band(66, BG_H, 7)                    # dry sand
    for (x, y) in ((12, 72), (30, 77), (54, 70), (70, 76)):
        cv.put(x, y, 8)                     # shells
    return cv


def bg_forest():
    cv = Canvas(BG_W, BG_H)
    cv.band(0, 66, 1)                       # gloom
    for (x, w, top) in ((4, 5, 6), (20, 4, 14), (40, 6, 2), (58, 4, 12), (72, 5, 8)):
        cv.rect(x, top + 16, w, 66 - top - 16, 4)      # trunks
        cv.ellipse(x + w // 2, top + 14, 12, 12, 2)    # canopy
        cv.ellipse(x + w // 2 - 3, top + 10, 7, 6, 3)  # lit side
    cv.band(66, BG_H, 5)                    # mossy floor
    for x in range(0, BG_W, 5):
        cv.put(x, 66, 6); cv.put(x + 2, 68, 6)
    for (x, y) in ((16, 73), (50, 76), (68, 71)):
        cv.disc(x, y, 2, 7)                 # mushrooms
    return cv


def bg_greenhouse():
    cv = Canvas(BG_W, BG_H)
    cv.band(0, 66, 1)                       # glass
    for x in range(0, BG_W, 16):            # frame verticals
        cv.rect(x, 0, 2, 66, 2)
    for y in range(0, 66, 18):              # frame horizontals
        cv.rect(0, y, BG_W, 2, 2)
    for (x, y) in ((8, 8), (40, 26), (64, 12)):
        cv.ellipse(x, y, 7, 5, 3)           # condensation
    for (x, h) in ((6, 16), (24, 22), (56, 18), (72, 24)):   # potted plants
        cv.rect(x - 4, 66 - h, 8, h, 4)
        cv.ellipse(x, 66 - h, 7, 5, 5)
        cv.rect(x - 5, 60, 10, 6, 6)        # pot
    cv.band(66, BG_H, 7)                    # soil bed
    for x in range(0, BG_W, 6):
        cv.put(x, 68, 8); cv.put(x + 3, 72, 8)
    return cv


BACKDROPS = {
    "blob": [
        ("MEADOW", bg_meadow,
         [0, 0x5CFF, 0x9E7F, 0xFF40, 0xFFFF, 0x6C8C, 0x6E4C, 0x4DC8, 0xFFE0,
          0, 0, 0, 0, 0, 0, 0]),
        ("BEACH", bg_beach,
         [0, 0x4DFF, 0xAEFF, 0xFFC0, 0x1C9A, 0xBEDF, 0xD5B4, 0xF6D8, 0xFFFF,
          0, 0, 0, 0, 0, 0, 0]),
    ],
    "sprout": [
        ("FOREST", bg_forest,
         [0, 0x1985, 0x2C48, 0x4E0A, 0x4A24, 0x3327, 0x54EA, 0xEC68, 0,
          0, 0, 0, 0, 0, 0, 0]),
        ("GREENHOUSE", bg_greenhouse,
         [0, 0xBF3B, 0x8452, 0xDF7D, 0x4B08, 0x5E8A, 0xB2AA, 0x5B08, 0x7BAC,
          0, 0, 0, 0, 0, 0, 0]),
    ],
}


# ---- pose construction --------------------------------------------------

# ---- growth stages ------------------------------------------------------
#
# A creature's age level runs 1..50 (EPET_AGE_MAX). These are the stages the
# built-in species grow through: each sets a SIZE on screen and reshapes the
# body, so a baby reads as a baby rather than as a small adult.
#
# scale_pct is the on-screen size, 100 = one sprite pixel per screen pixel.
# The old fixed look was 200. The ceiling is set by the canvas and the ground
# line -- see assert_fits_panel() below, which fails the build rather than
# letting a big pet grow up into the status bar.
#
# The morph reshapes the SAME drawing code rather than requiring a second set
# of hand-drawn poses; every pose a stage owns comes out consistent for free.
GROWTH = [
    # key      from_age  scale_pct  morph
    ("baby",   1,        130,  dict(sx=0.80, sy=0.84, dy=4, spread=0.75,
                                    feet=False)),
    ("teen",   9,        205,  dict()),
    ("adult",  20,       290,  dict(sx=1.06, sy=1.04, spread=1.1)),
    ("elder",  38,       285,  dict(sx=1.08, sy=0.84, dy=3, spread=1.1)),
]

# Which stage's art a species reports as its own. Pages and the CLASS page's
# preview draw at the base scale, where the grown form is what reads as "this
# is a BLOB" -- a baby is the least recognisable thing a species has.
BASE_STAGE = "adult"


def morphed(body_fn, morph):
    """Wrap frame() so a stage reshapes every pose it owns identically."""
    sx     = morph.get("sx", 1.0)
    sy     = morph.get("sy", 1.0)
    dy     = morph.get("dy", 0)
    sspr   = morph.get("spread", 1.0)
    nofeet = morph.get("feet", True) is False

    def f(_body, cx=22, cy=24, rx=16, ry=15, eye_y=None, mouth_y=None,
          spread=9, feet=True, **kw):
        # Scale the radii rather than offsetting them, so the tiny early
        # frames of "birth" stay in proportion instead of collapsing.
        rx = max(2, int(round(rx * sx)))
        ry = max(2, int(round(ry * sy)))
        cy += dy
        # eye_y/mouth_y are absolute when given (the dead pose pins them), so
        # they have to follow the body down or the face detaches from it.
        if eye_y is not None:
            eye_y += dy
        if mouth_y is not None:
            mouth_y += dy
        spread = max(3, int(round(spread * sspr)))
        return frame(_body, cx=cx, cy=cy, rx=rx, ry=ry, eye_y=eye_y,
                     mouth_y=mouth_y, spread=spread,
                     feet=False if nofeet else feet, **kw)

    # build()'s pose definitions all call frame(body_fn, ...) with the body
    # function positional. The wrapper already closes over it, so accept and
    # discard that argument instead of editing thirty call sites.
    return lambda _bound_body=None, **kw: f(body_fn, **kw)


def build(body_fn, morph=None):
    """Return {pose_name: [(canvas, hold_ms), ...]}"""
    poses = {}
    frame = morphed(body_fn, morph or {})       # shadows the global on purpose

    # idle: breathe, breathe, blink
    poses["idle"] = [
        (frame(body_fn, ry=15, mth="flat"), 900),
        (frame(body_fn, cy=23, ry=16, mth="flat"), 900),
        (frame(body_fn, ry=15, mth="flat"), 600),
        (frame(body_fn, ry=15, eye="closed", mth="flat"), 130),
    ]

    # happy: bounce up, grin, sparkles
    poses["happy"] = [
        (frame(body_fn, cy=26, rx=17, ry=13, eye="happy", mth="smile"), 130),
        (frame(body_fn, cy=19, rx=15, ry=17, eye="happy", mth="grin", fx=sparkle), 200),
        (frame(body_fn, cy=22, rx=16, ry=15, eye="happy", mth="grin", fx=sparkle), 200),
        (frame(body_fn, cy=25, rx=17, ry=14, eye="happy", mth="smile"), 200),
    ]

    # sad: sink and droop
    poses["sad"] = [
        (frame(body_fn, cy=26, rx=17, ry=13, eye="sad", mth="frown"), 700),
        (frame(body_fn, cy=27, rx=18, ry=12, eye="sad", mth="frown", fx=tear), 700),
        (frame(body_fn, cy=26, rx=17, ry=13, eye="sad", mth="frown"), 500),
    ]

    # dead: slumped flat with crossed eyes, and a spirit drifting off.
    # A distinct pose rather than a grey tint on the idle frame -- the shape
    # is what reads as "gone" at this size, not the colour.
    def wisp(height, fade):
        def fx(cv):
            y = 16 - height + BODY_DY
            cv.disc(26, y, 3 - fade, 3)
            cv.disc(27, y - 4, max(1, 2 - fade), 3)
        return fx

    poses["dead"] = [
        (frame(body_fn, cy=30, rx=17, ry=9, eye="dead", mth="flat",
               eye_y=27, mouth_y=34, fx=wisp(0, 0)), 520),
        (frame(body_fn, cy=30, rx=17, ry=9, eye="dead", mth="flat",
               eye_y=27, mouth_y=34, fx=wisp(6, 0)), 520),
        (frame(body_fn, cy=31, rx=18, ry=8, eye="dead", mth="flat",
               eye_y=28, mouth_y=35, fx=wisp(12, 1)), 520),
        (frame(body_fn, cy=31, rx=18, ry=8, eye="dead", mth="flat",
               eye_y=28, mouth_y=35), 900),
    ]

    # birth: a speck that swells, wobbles and opens its eyes
    poses["birth"] = [
        (frame(body_fn, cy=32, rx=4, ry=4, eye="closed", mth="flat",
               eye_y=31, mouth_y=33, spread=3, feet=False), 260),
        (frame(body_fn, cy=30, rx=8, ry=7, eye="closed", mth="flat",
               eye_y=29, mouth_y=32, spread=5, feet=False), 260),
        (frame(body_fn, cy=27, rx=13, ry=11, eye="closed", mth="flat",
               eye_y=25, mouth_y=31, spread=7, feet=False), 260),
        (frame(body_fn, cy=24, rx=17, ry=16, eye="closed", mth="o",
               eye_y=20, mouth_y=30, spread=9), 300),
        (frame(body_fn, cy=24, rx=16, ry=15, eye="open", mth="o", fx=sparkle), 400),
        (frame(body_fn, cy=24, rx=16, ry=15, eye="happy", mth="smile", fx=sparkle), 500),
    ]
    return poses


# ---- emit ---------------------------------------------------------------

def assert_not_clipped(name, cv):
    """Opaque pixels on the outermost row/column mean the art ran off the
    canvas and was silently cut. Every sprite must keep a 1px margin."""
    bad = []
    for x in range(cv.w):
        if cv.get(x, 0):          bad.append(("top", x))
        if cv.get(x, cv.h - 1):   bad.append(("bottom", x))
    for y in range(cv.h):
        if cv.get(0, y):          bad.append(("left", y))
        if cv.get(cv.w - 1, y):   bad.append(("right", y))
    if bad:
        edges = sorted(set(e for e, _ in bad))
        raise SystemExit(
            "sprite %s touches the canvas edge (%s) -- %d pixels would be "
            "clipped; raise H/W or lower BODY_DY" % (name, ",".join(edges), len(bad)))


def assert_fits_panel(name, cv, scale_pct):
    """A stage sets its own on-screen size, and the main screen anchors the
    sprite by its feet on a fixed ground line. Too large and the creature
    grows up through the status bar -- which looks like a rendering bug, not
    like a big pet. Catch it here rather than on the device.

    Mirrors epet_render.c: PET_GROUND is the bottom edge, PANEL_H the bar."""
    PET_GROUND, PANEL_H, SCREEN_H = 212, 18, 240

    rows = [y for y in range(cv.h) if any(cv.get(x, y) for x in range(cv.w))]
    if not rows:
        return
    s = scale_pct / 100.0
    top    = PET_GROUND - cv.h * s + rows[0] * s
    bottom = PET_GROUND - cv.h * s + (rows[-1] + 1) * s
    if top < PANEL_H or bottom > SCREEN_H:
        raise SystemExit(
            "sprite %s at scale %d%% spans y=%.0f..%.0f, outside the "
            "%d..%d the main screen leaves for the pet -- lower scale_pct "
            "in GROWTH or raise PET_GROUND"
            % (name, scale_pct, top, bottom, PANEL_H, SCREEN_H))


def emit_frame(name, cv, out):
    n = cv.w * cv.h
    out.append("static const uint8_t %s[%d] = {" % (name, n))
    for y in range(cv.h):
        row = cv.px[y * cv.w:(y + 1) * cv.w]
        out.append("    " + ",".join(str(v) for v in row) + ("," if y < cv.h - 1 else ""))
    out.append("};")
    out.append("static const epet_frame_t F_%s = { %d, %d, %s };" % (name, cv.w, cv.h, name))
    out.append("")


def rgb565(r, g, b):
    """Native-order RGB565; swap16() puts it in panel order."""
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def swap16(v):
    """Palettes are stored panel-ready (MSB first), like every other colour;
    see EPET_RGB565 in epet_draw.h."""
    return ((v & 0xFF) << 8) | ((v >> 8) & 0xFF)


def main():
    out = []
    out.append("/* GENERATED by tools/gen_sprites.py -- do not edit by hand. */")
    out.append('#include "epet_species.h"')
    out.append('#include "epet_draw.h"')
    out.append("extern const epet_species_t *const epet_core_species[];")
    out.append("extern const uint8_t epet_core_species_count;")
    out.append("")

    species = [
        ("blob", blob_body, "BLOB", "A cheerful puddle. Eats anything.",
         [0, 0x18E3, 0xF6CB, 0xFF93, 0xC4E6, 0xFFFF, 0x1082, 0x2124,
          0xFE60, 0xC300, 0, 0, 0, 0, 0, 0],
         (1.0, 1.0, 1.0, 1.0)),
        ("sprout", sprout_body, "SPROUT", "Photosynthesises. Hates mess.",
         [0, 0x10A2, 0x8FEC, 0xCFF3, 0x5D4A, 0xFFFF, 0x1082, 0x18E3,
          0x3E68, 0x2C44, 0, 0, 0, 0, 0, 0],
         (0.75, 1.0, 0.85, 1.35)),
    ]

    pose_order = ["idle", "birth", "happy", "sad", "dead"]

    for ident, body_fn, disp, blurb, pal, temper in species:
        # backdrops first
        for bi, (bname, bfn, bpal) in enumerate(BACKDROPS[ident]):
            bcv = bfn()
            # A backdrop must be fully opaque: index 0 is transparent, so a
            # gap would leave the previous frame showing through on screen.
            holes = sum(1 for v in bcv.px if v == 0)
            if holes:
                raise SystemExit(
                    "backdrop %s/%s has %d transparent pixels" % (ident, bname, holes))
            emit_frame("bg_%s_%d" % (ident, bi), bcv, out)
            out.append("static const epet_palette_t BP_%s_%d = { { %s } };"
                       % (ident, bi,
                          ", ".join("0x%04X" % swap16(c) for c in bpal)))
            out.append("")
        out.append("static const epet_backdrop_t BD_%s[] = {" % ident)
        for bi, (bname, bfn, bpal) in enumerate(BACKDROPS[ident]):
            # Every colour is stored panel-ready, this one included -- it is
            # easy to miss because it is the only literal colour here that is
            # not part of a palette.
            out.append('    { "%s", &F_bg_%s_%d, &BP_%s_%d, %d, 0, 0, 198, '
                       '0x%04X, 150 },'
                       % (bname, ident, bi, ident, bi, BG_SCALE,
                          swap16(rgb565(24, 30, 72))))
        out.append("};")
        out.append("")

        emit_frame("poop_%s" % ident, POOPS[ident](), out)

        # One full pose set per growth stage. The morph reshapes the same
        # drawing code, so a stage costs authoring effort only where the
        # proportions actually differ.
        for skey, _from_age, spct, morph in GROWTH:
            poses = build(body_fn, morph)
            for pname in pose_order:
                for i, (cv, hold) in enumerate(poses[pname]):
                    fname = "%s_%s_%s_%d" % (ident, skey, pname, i)
                    assert_not_clipped(fname, cv)
                    assert_fits_panel(fname, cv, spct)
                    emit_frame(fname, cv, out)

            for pname in pose_order:
                keys = poses[pname]
                out.append("static const epet_key_t K_%s_%s_%s[] = {"
                           % (ident, skey, pname))
                for i, (_, hold) in enumerate(keys):
                    out.append("    { &F_%s_%s_%s_%d, %d },"
                               % (ident, skey, pname, i, hold))
                out.append("};")
            out.append("")

            out.append("static const epet_pose_t P_%s_%s[] = {" % (ident, skey))
            for pname in pose_order:
                loop = "true" if pname in ("idle", "sad", "dead") else "false"
                out.append('    { "%s", K_%s_%s_%s, %d, %s },'
                           % (pname, ident, skey, pname, len(poses[pname]), loop))
            out.append("};")
            out.append("")

        out.append("static const epet_growth_t G_%s[] = {" % ident)
        for skey, from_age, spct, _morph in GROWTH:
            out.append("    { %d, %d, P_%s_%s, %d },   /* %s */"
                       % (from_age, spct, ident, skey, len(pose_order), skey))
        out.append("};")
        out.append("")

        out.append("static const epet_species_t S_%s = {" % ident)
        out.append('    .name = "%s",' % disp)
        out.append('    .blurb = "%s",' % blurb)
        out.append("    .palette = { { %s } },"
                   % ", ".join("0x%04X" % swap16(c) for c in pal))
        out.append("    .poses = P_%s_%s," % (ident, BASE_STAGE))
        out.append("    .n_poses = %d," % len(pose_order))
        out.append("    .scale = %d," % SCALE)
        out.append("    .backdrops = BD_%s," % ident)
        out.append("    .n_backdrops = %d," % len(BACKDROPS[ident]))
        out.append("    .poop = &F_poop_%s," % ident)
        out.append("    .temper = { %.2ff, %.2ff, %.2ff, %.2ff }," % temper)
        out.append("    .stages = G_%s," % ident)
        out.append("    .n_stages = %d," % len(GROWTH))
        out.append("};")
        out.append("")

    # menu icons, shared by every page
    for name, rows in sorted(ICONS.items()):
        emit_frame("icon_%s" % name, icon_canvas(rows), out)
    out.append("const epet_icon_t EPET_ICONS[] = {")
    for name in sorted(ICONS):
        out.append('    { "%s", &F_icon_%s },' % (name, name))
    out.append("};")
    out.append("const uint8_t EPET_ICON_COUNT = %d;" % len(ICONS))
    out.append("")

    out.append("/* Consumed by the core module in epet_module_core.c. */")
    out.append("const epet_species_t *const epet_core_species[] = {")
    for ident, *_ in species:
        out.append("    &S_%s," % ident)
    out.append("};")
    out.append("const uint8_t epet_core_species_count = %d;" % len(species))

    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
