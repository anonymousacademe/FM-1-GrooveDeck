#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
r"""Check the trace of tests/midi.txt: the TRS jack's MIDI in (running status, real-time bytes inside messages, SysEx
   ignored, velocity 0, channels, General MIDI notes, program change, CC 120/123, note repeat, live record) and the notes that
   go out over USB-MIDI (channel 10).
   .\fm1.ps1 host groovedeck tests/midi.txt > build\host\midi-trace.txt ; python tests\check_midi.py build\host\midi-trace.txt"""
import re, sys

hits, out, prog, silence, rec = [], [], [], [], []
for line in open(sys.argv[1]):
    m = re.match(r"\s*(\d+) ms  hit (\d+) vel\s+(\d+)", line)
    if m: hits.append((int(m[1]), int(m[2]), int(m[3]))); continue
    m = re.match(r"\s*(\d+) ms  MIDI out ([0-9A-F]+) (\d+) (\d+)", line)
    if m: out.append((int(m[1]), int(m[2], 16), int(m[3]), int(m[4]))); continue
    m = re.match(r"\s*(\d+) ms  program (pattern|kit) (\d+)", line)
    if m: prog.append((int(m[1]), m[2], int(m[3]))); continue
    m = re.match(r"\s*(\d+) ms  silence", line)
    if m: silence.append(int(m[1])); continue
    m = re.match(r"\s*(\d+) ms  rec drum (\d+) step (\d+)", line)
    if m: rec.append((int(m[1]), int(m[2]), int(m[3])))


def h(a, b):                                    # (drum, velocity) of the hits in [a, b), in order
    return [(d, v) for t, d, v in hits if a <= t < b]


def o(a, b, st):                                # (drum, velocity) of the notes sent in [a, b) with that status
    return [(n - 36, v) for t, s, n, v in out if a <= t < b and s == st]


ons = [(t, n - 36, v) for t, s, n, v in out if s == 0x99]
offs = [(t, n - 36) for t, s, n, v in out if s == 0x89]
seq_hits = sorted((d, v) for t, d, v in hits if 6000 <= t < 8200 and d != 11)
seq_outs = sorted((n - 36, v) for t, s, n, v in out if s == 0x99 and 6000 <= t < 8200)
# every note on is followed by its own note off 30..40 ms later (the first off after it, for that drum)
ended = all(any(d2 == d1 and 30 <= t2 - t1 <= 40 for t2, d2 in offs) for t1, d1, v in ons)
off_j = [t for t, d in offs if d == 0 and 2200 <= t < 2300]
on_j = [t for t, d, v in ons if 2200 <= t < 2300]
rec_hit = [t for t, d, v in hits if d == 7 and v == 127 and 10800 <= t < 11300]
checks = [
    ("A: running status plays pads 1 3 5 at velocity 100 80 127", h(100, 300) == [(0, 100), (2, 80), (4, 127)]),
    ("B: note offs with clock bytes inside make no sound", h(300, 500) == []),
    ("C: a real-time byte inside a note on: RIM at 100; Start/Stop/Clock dropped", h(500, 700) == [(6, 100)]),
    ("D: SysEx ignored, the note after it plays (COWBL, 80)", h(700, 900) == [(7, 80)]),
    ("E: velocity 0 is a note off, no hit", h(900, 1000) == []),
    ("F: any channel: note 51 (ch 10) = ZAP, note 45 (ch 4) = CNG L", h(1000, 1200) == [(15, 112), (9, 64)]),
    ("G: General MIDI notes outside 36..51 pick their drums, unknown notes none",
        h(1200, 1500) == [(0, 80), (8, 80), (10, 80), (11, 80), (12, 80), (13, 80), (7, 80), (9, 80), (15, 80)]),
    ("H: program change 3 = pattern 4, 9 = kit 1, 12 ignored, 0 = pattern 1",
        [(k, n) for t, k, n in prog if 1500 <= t < 1900] == [("pattern", 3), ("kit", 1), ("pattern", 0)]),
    ("I: CC 120 and CC 123 silence the drums, CC 7 does not",
        [t // 50 for t in silence if 1900 <= t < 2200] == [39, 40] and len(h(1900, 1950)) == 2),
    ("I: ...and the sequencer is not started by it (no hits)", h(2000, 2200) == []),
    ("J: nothing from MIDI in is echoed out (no MIDI out before the panel key)", [e for e in out if e[0] < 2200] == []),
    ("J: a panel pad goes out: KICK = note 36 on channel 10, velocity 100", o(2200, 2300, 0x99) == [(0, 100)]),
    ("J: ...with its note off after 30 ms", len(off_j) == 1 and len(on_j) == 1 and 30 <= off_j[0] - on_j[0] <= 40),
    ("K: note repeat rolls the held external pad at velocity 80", len(h(2700, 3500)) >= 6 and all(x == (4, 80) for x in h(2700, 3500))),
    ("K: ...it stops with the note off", h(3600, 4200) == []),
    ("K: ...and none of it goes out", o(2600, 4200, 0x99) == []),
    ("L: note repeat on a panel pad goes out, a note per hit", len(h(4300, 5000)) >= 4 and sorted(o(4300, 5100, 0x99)) == sorted(h(4300, 5000))
        and all(x == (1, 100) for x in h(4300, 5000))),
    ("M: the sequencer's steps go out with the velocity they sound with", len(seq_hits) >= 15 and seq_hits == seq_outs),
    ("M: the first step is an accented kick (127)", (0, 127) in h(6000, 6100) and (0, 127) in o(6000, 6100, 0x99)),
    ("M: a MIDI hit while it runs sounds (SHAKE 99) but is not echoed", (11, 99) in h(6500, 6600) and not [1 for d, v in o(6000, 8300, 0x99) if d == 11]),
    ("M: CC 123 silences, the sequencer goes on", [t for t in silence if 7000 <= t < 7010] != [] and len(h(7050, 7600)) >= 5),
    ("M: it stops with PLAY", h(8300, 8500) == []),
    ("N: every note on has its note off 30 ms later", ended and len(ons) == len(offs)),
    ("N: a note played while recording is recorded", any(9050 <= t < 9060 and d == 7 for t, d, s in rec)),
    ("N: ...and plays in the next bar with the accent (127), and goes out", len(rec_hit) == 1 and (7, 127) in o(10800, 11300, 0x99)),
    ("N: ...but the note that recorded it did not go out", (7, 127) not in o(9050, 9150, 0x99)),
]
bad = 0
for name, ok in checks:
    print(("ok    " if ok else "FAIL  ") + name)
    bad += not ok
print(f"{len(checks) - bad} of {len(checks)} ok")
sys.exit(1 if bad else 0)
