#!/usr/bin/env python3
"""Assembler for epet module bytecode.

Source is one instruction per line, `;` starts a comment, `label:` defines a
label. Strings are written inline in double quotes and pooled automatically.

    push 10          ; a literal
    text 8 8 "HELLO" $WHITE 2
    ret 1

Mnemonics map to opcodes; a handful of pseudo-instructions expand to a
syscall with its arguments pushed in order, which is what you actually want
to write.
"""
import struct
import re

OPS = {
    "nop": 0, "push_i32": 1, "push_i8": 2, "drop": 3, "dup": 4, "swap": 5,
    "loadl": 6, "storel": 7, "loadg": 8, "storeg": 9, "arg": 10,
    "add": 11, "sub": 12, "mul": 13, "div": 14, "mod": 15, "neg": 16,
    "and": 17, "or": 18, "xor": 19, "shl": 20, "shr": 21,
    "eq": 22, "ne": 23, "lt": 24, "le": 25, "gt": 26, "ge": 27, "not": 28,
    "jmp": 29, "jz": 30, "jnz": 31,
    "call": 32, "ret": 33,
}

SYS = {
    "fill": (0, 1), "rect": (1, 5), "disc": (2, 4), "shade": (3, 6),
    "text": (4, 5), "textv": (5, 5), "number": (6, 5), "bar": (7, 6),
    "sprite": (8, 3), "rgb": (9, 3), "get": (10, 1), "act": (11, 1),
    "emote": (12, 1), "close": (13, 0), "rand": (14, 1), "textw": (15, 2),
    "levelcol": (16, 1), "nudge": (17, 1), "evarg": (18, 1),
}

# pet field ids -- must match epet_src_t
SRC = {
    "none": 0, "hunger": 1, "fed": 2, "happiness": 3, "energy": 4,
    "hygiene": 5, "health": 6, "age_s": 7, "poop": 8,
    "species_name": 9, "species_blurb": 10, "module_id": 11,
    "backdrop_name": 12, "pose_name": 13, "mood": 14,
    "temper_hunger": 15, "temper_happy": 16, "temper_energy": 17,
    "temper_hygiene": 18, "n_backdrops": 19, "n_poses": 20,
    "sprite_scale": 21, "species_count": 22, "module_count": 23,
}
ACT = {"none": 0, "feed": 1, "play": 2, "clean": 3, "revive": 4}
BTN = {"lt": 1, "lb": 2, "rt": 4, "rb": 8}


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


# event type ids -- must match epet_event_type_t
EV = {
    "minute": 0, "fed": 1, "played": 2, "cleaned": 3, "pooped": 4,
    "hungry": 5, "sad": 6, "dirty": 7, "sick": 8, "recovered": 9,
    "fell_asleep": 10, "woke": 11, "died": 12, "reborn": 13,
    "display_on": 14, "display_off": 15, "attention": 16, "button": 17,
}

CONSTS = {
    "$WHITE": 0xFFFF, "$BLACK": 0x0000,
    "$PANEL": rgb565(16, 22, 34), "$DIM": rgb565(120, 132, 150),
    "$GOOD": rgb565(90, 210, 110), "$WARN": rgb565(240, 190, 70),
    "$BAD": rgb565(230, 80, 80), "$MENU": rgb565(22, 30, 46),
    "$ACCENT": rgb565(90, 160, 220), "$W": 240, "$H": 240,
}
for k, v in BTN.items():
    CONSTS["$BTN_" + k.upper()] = v
for k, v in SRC.items():
    CONSTS["$SRC_" + k.upper()] = v
for k, v in ACT.items():
    CONSTS["$ACT_" + k.upper()] = v
for k, v in EV.items():
    CONSTS["$EV_" + k.upper()] = v          # the event id
    CONSTS["$M_" + k.upper()] = 1 << v      # its subscription mask bit


class Asm:
    def __init__(self):
        # Offset 0 is reserved: a vmpage entry point of 0 means "this hook is
        # absent", so no real hook may start there. A leading NOP guarantees
        # the first label lands at 1 or later.
        self.code = bytearray([OPS["nop"]])
        self.labels = {}
        self.fixups = []          # (pos, label, end_of_instruction)
        self.strings = []
        self.str_index = {}

    # ---- primitives ----
    def str_id(self, s):
        if s not in self.str_index:
            self.str_index[s] = len(self.strings)
            self.strings.append(s)
        return self.str_index[s]

    def emit(self, op, operand=b""):
        self.code.append(OPS[op] if isinstance(op, str) else op)
        self.code += operand

    def push(self, v):
        if -128 <= v <= 127:
            self.emit("push_i8", struct.pack("<b", v))
        else:
            self.emit("push_i32", struct.pack("<i", v))

    def label(self, name):
        self.labels[name] = len(self.code)

    def jump(self, kind, target):
        self.emit(kind, b"\0\0")
        self.fixups.append((len(self.code) - 2, target, len(self.code)))

    def syscall(self, name):
        fn, argc = SYS[name]
        self.emit("call", bytes([fn, argc]))

    # ---- source form ----
    def value(self, tok):
        if tok in CONSTS:
            return CONSTS[tok]
        if tok.startswith("'") and tok.endswith("'"):
            return ord(tok[1])
        return int(tok, 0)

    def line(self, raw):
        line = raw.split(";")[0].strip()
        if not line:
            return
        if line.endswith(":"):
            self.label(line[:-1])
            return

        # pull quoted strings out first so spaces inside them survive
        strs = []
        def grab(m):
            strs.append(m.group(1))
            return "\x00%d" % (len(strs) - 1)
        line = re.sub(r'"([^"]*)"', grab, line)
        parts = line.split()
        op, args = parts[0].lower(), parts[1:]

        def resolve(a):
            if a.startswith("\x00"):
                return self.str_id(strs[int(a[1:])])
            return self.value(a)

        if op in SYS:
            for a in args:
                self.push(resolve(a))
            self.syscall(op)
            self.emit("drop")            # syscalls always push a result
            return
        if op in ("callv", "sysv"):       # keep the syscall result on the stack
            name = args[0]
            for a in args[1:]:
                self.push(resolve(a))
            self.syscall(name)
            return
        if op == "sys":                   # args already on the stack; drop result
            self.syscall(args[0])
            self.emit("drop")
            return
        if op == "sysk":                  # args already on the stack; keep result
            self.syscall(args[0])
            return
        if op == "push":
            self.push(resolve(args[0]))
            return
        if op in ("jmp", "jz", "jnz"):
            self.jump(op, args[0])
            return
        if op in ("loadl", "storel", "loadg", "storeg", "arg"):
            self.emit(op, bytes([self.value(args[0])]))
            return
        if op == "ret":
            if args:
                self.push(resolve(args[0]))
            self.emit("ret")
            return
        if op in OPS:
            self.emit(op)
            return
        raise SystemExit("unknown instruction: %s" % op)

    def assemble(self, text):
        for raw in text.splitlines():
            self.line(raw)
        for pos, target, end in self.fixups:
            if target not in self.labels:
                raise SystemExit("undefined label: %s" % target)
            rel = self.labels[target] - end
            struct.pack_into("<h", self.code, pos, rel)
        return bytes(self.code)

    def string_section(self):
        pool = b""
        offs = []
        for s in self.strings:
            offs.append(len(pool))
            pool += s.encode("ascii") + b"\0"
        out = struct.pack("<HH", len(offs), len(pool))
        for o in offs:
            out += struct.pack("<H", o)
        return out + pool
