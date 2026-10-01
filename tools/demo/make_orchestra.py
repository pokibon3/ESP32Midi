#!/usr/bin/env python3
"""Generate an orchestral demo MIDI file that pushes the synth towards 128 voices.

    python3 tools/demo/make_orchestra.py [out.mid]
"""
import struct
import sys

TPQ = 480
BEAT = TPQ
BAR = BEAT * 4
TEMPO_BPM = 72

# channel: (GM program, name)
VLN1, VLN2, VLA, VC, CB, HARP, TIMP, ENS1, ENS2, DRUMS, CHOIR, TPT, TBN, HORN, FL, CL = range(16)
PROGRAMS = {
    VLN1: 40, VLN2: 40, VLA: 41, VC: 42, CB: 43, HARP: 46, TIMP: 47, ENS1: 48, ENS2: 49,
    CHOIR: 52, TPT: 56, TBN: 57, HORN: 60, FL: 73, CL: 71,
}
PAN = {VLN1: 40, VLN2: 52, VLA: 72, VC: 84, CB: 92, HARP: 30, TIMP: 64, ENS1: 48, ENS2: 80,
       CHOIR: 64, TPT: 74, TBN: 88, HORN: 44, FL: 56, CL: 68, DRUMS: 64}

# Chords: root pitch class, intervals
CHORDS = {
    "Cm": (0, (0, 3, 7)), "C": (0, (0, 4, 7)), "Ab": (8, (0, 4, 7)), "Eb": (3, (0, 4, 7)),
    "Bb": (10, (0, 4, 7)), "Fm": (5, (0, 3, 7)), "G": (7, (0, 4, 7)),
}

SECTIONS = [
    # (name, chords, dynamics 0..1)
    ("intro", ["Cm", "Ab", "Fm", "G"], 0.35),
    ("A", ["Cm", "Ab", "Eb", "Bb", "Fm", "Cm", "Ab", "G"], 0.65),
    ("B", ["Ab", "Bb", "Eb", "Cm", "Fm", "Bb", "Eb", "G"], 1.0),
    ("coda", ["Cm", "Ab", "G", "C"], 1.0),
]

N = {n: i for i, n in enumerate(["C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"])}


def p(name):
    """'Eb5' -> MIDI note number"""
    return N[name[:-1]] + 12 * (int(name[-1]) + 1)


MELODY_A = [
    [("G4", 2), ("Eb5", 1), ("D5", 1)], [("C5", 3), ("Eb5", 1)], [("Bb4", 2), ("G4", 1), ("Bb4", 1)],
    [("D5", 3), ("F5", 1)], [("Ab5", 2), ("G5", 1), ("F5", 1)], [("Eb5", 2), ("D5", 1), ("C5", 1)],
    [("C5", 2), ("Eb5", 1), ("Ab5", 1)], [("G5", 3), ("B4", 1)],
]
MELODY_B = [
    [("C5", 2), ("Eb5", 1), ("Ab5", 1)], [("Bb5", 2), ("Ab5", 1), ("F5", 1)], [("G5", 3), ("Eb5", 1)],
    [("G5", 2), ("F5", 1), ("Eb5", 1)], [("F5", 2), ("Ab5", 1), ("C6", 1)], [("Bb5", 2), ("D6", 1), ("Bb5", 1)],
    [("Eb6", 3), ("D6", 1)], [("D6", 2), ("B5", 1), ("G5", 1)],
]
MELODY_CODA = [[("C6", 4)], [("C6", 2), ("Eb6", 2)], [("D6", 2), ("B5", 2)], [("C6", 4)]]

events = []  # (tick, order, bytes)


def ev(t, data, order=1):
    events.append((int(t), order, bytes(data)))


def note(ch, t, pitch, dur, vel):
    vel = max(1, min(127, int(vel)))
    ev(t, [0x90 | ch, pitch, vel], 1)
    ev(t + max(1, int(dur)), [0x80 | ch, pitch, 0], 0)


def cc(ch, t, num, val):
    ev(t, [0xB0 | ch, num, max(0, min(127, int(val)))], 0)


def chord_notes(chord, lo, hi):
    root, ivs = CHORDS[chord]
    pcs = {(root + i) % 12 for i in ivs}
    return [n for n in range(lo, hi + 1) if n % 12 in pcs]


def build():
    for ch, prog in PROGRAMS.items():
        ev(0, [0xC0 | ch, prog], 0)
    for ch in range(16):
        cc(ch, 0, 7, 100)
        cc(ch, 0, 10, PAN[ch])
        cc(ch, 0, 91, 80)
        cc(ch, 0, 93, 40 if ch in (VLN1, VLN2, VLA, ENS1, ENS2, CHOIR) else 10)

    t = 0
    for name, chords, dyn in SECTIONS:
        melody = {"A": MELODY_A, "B": MELODY_B, "coda": MELODY_CODA}.get(name)
        for bar, chord in enumerate(chords):
            root, _ = CHORDS[chord]
            v = 50 + 70 * dyn
            last = name == "coda" and bar == len(chords) - 1
            span = BAR * 2 if last else BAR

            # crescendo through the bar via expression
            for step in range(4):
                for ch in (ENS1, ENS2, CHOIR, HORN, TBN):
                    cc(ch, t + step * BEAT, 11, 80 + 47 * dyn * (step + 1) / 4)

            # low strings: roots
            note(CB, t, 28 + (root - 4) % 12, span * 0.95, v)  # E1..Eb2
            note(VC, t, 36 + root, span * 0.95, v)
            note(VC, t, 36 + root + 7, span * 0.95, v * 0.9)

            # string pads: wide voicings
            for n in chord_notes(chord, 48, 76):
                note(ENS1, t, n, span * 0.98, v * 0.8)
            for n in chord_notes(chord, 55, 84):
                note(ENS2, t, n, span * 0.98, v * 0.75)

            # tremolo inner strings (16ths)
            trem = chord_notes(chord, 55, 72)[:4]
            for i in range(16 * (2 if last else 1)):
                for k, n in enumerate(trem):
                    ch = VLN2 if k % 2 else VLA
                    note(ch, t + i * BEAT // 4, n, BEAT // 4 * 0.9, v * (0.7 + 0.3 * (i % 4 == 0)))

            # harp: rising 16th arpeggio over four octaves
            arp = chord_notes(chord, 43, 91)
            for i in range(16):
                n = arp[(i * 2) % len(arp)] if name != "intro" else arp[i % len(arp)]
                note(HARP, t + i * BEAT // 4, n, BEAT * 2, v * 0.7)

            # clarinet: 8th-note broken chord
            cl = chord_notes(chord, 58, 79)
            for i in range(8):
                note(CL, t + i * BEAT // 2, cl[(i * 3) % len(cl)], BEAT // 2 * 0.9, v * 0.6)

            # timpani: roll on the root at section starts and in the climax
            if bar == 0 or name in ("B", "coda"):
                tn = 36 + (root + 7) % 12  # fifth
                for i in range(16 if not last else 32):
                    note(TIMP, t + i * BEAT // 8, 36 + root if i % 2 else tn, BEAT // 8, v * (0.5 + 0.5 * i / 16))

            if name in ("B", "coda"):
                for n in chord_notes(chord, 55, 79):
                    note(CHOIR, t, n, span * 0.95, v * 0.8)
                for n in chord_notes(chord, 48, 67)[:4]:
                    note(HORN, t, n, span * 0.9, v * 0.85)
                for n in chord_notes(chord, 40, 55)[:3]:
                    note(TBN, t + BEAT * 2, n, BEAT * 2 * 0.9, v * 0.9)
                note(DRUMS, t, 35, BEAT, v)
                note(DRUMS, t + BEAT * 2, 35, BEAT, v * 0.8)
            if name == "A" and bar % 2 == 0:
                for n in chord_notes(chord, 48, 67)[:3]:
                    note(HORN, t, n, BAR * 2 * 0.9, v * 0.6)

            if bar == 0 and name != "intro":
                note(DRUMS, t, 49, BEAT * 2, 120)
                note(DRUMS, t, 57, BEAT * 2, 110)
            if last:
                note(DRUMS, t, 49, BEAT * 4, 127)
                for i in range(24):
                    note(DRUMS, t + i * BEAT // 6, 51, BEAT // 6, 60 + i * 2)

            # melody
            if melody:
                mt = t
                for pitch, beats in melody[bar]:
                    n = p(pitch)
                    dur = beats * BEAT * (2 if last and beats == 4 else 1)
                    note(VLN1, mt, n, dur * 0.95, v)
                    note(VLN1, mt, n + 12, dur * 0.95, v * 0.8)
                    note(FL, mt, n + 12, dur * 0.95, v * 0.8)
                    if name in ("B", "coda"):
                        note(TPT, mt, n, dur * 0.9, v)
                    mt += beats * BEAT
            else:  # intro: sustained violins
                for n in chord_notes(chord, 67, 79)[:2]:
                    note(VLN1, t, n, BAR * 0.95, v * 0.8)
            t += span

    # release everything and let the reverb ring out
    ev(t + BEAT * 4, [0xFF, 0x2F, 0x00], 2)


def vlq(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.insert(0, (n & 0x7F) | 0x80)
        n >>= 7
    return bytes(out)


def write(path):
    build()
    events.sort(key=lambda e: (e[0], e[1]))
    tempo = 60_000_000 // TEMPO_BPM
    data = vlq(0) + b"\xFF\x51\x03" + tempo.to_bytes(3, "big")
    now = 0
    for tick, _, msg in events:
        data += vlq(tick - now) + msg
        now = tick
    trk = b"MTrk" + struct.pack(">I", len(data)) + data
    with open(path, "wb") as f:
        f.write(b"MThd" + struct.pack(">IHHH", 6, 0, 1, TPQ) + trk)
    print(f"wrote {path} ({now / TPQ * 60 / TEMPO_BPM:.0f} s)")


if __name__ == "__main__":
    write(sys.argv[1] if len(sys.argv) > 1 else "orchestra.mid")
