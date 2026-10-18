#!/usr/bin/env python3
"""Shrink a SoundFont so it fits in the device's flash.

    python3 tools/sf2shrink.py IN.sf2|IN.sf3 OUT.sf2 [--max-size 13M] [--rate HZ] [--mono]

- SF3 (Ogg Vorbis) samples are decoded, so the output is always plain SF2.
- Samples above the target rate are resampled down. The synth runs at 32kHz, so
  content above 16kHz never reaches the output anyway. Each looped sample gets
  its own rate so the loop length stays a whole number of samples (no detune).
- --mono folds stereo pairs into one centered sample (half the data and voices).
- --max-size picks the highest rate (in 100Hz steps) that fits.
- --banks keeps only the listed banks (e.g. 0,128 for GM + drums).
- --keep exempts the samples of the listed bank 0 programs (e.g. 0-7 for the
  pianos) from --max-size / --rate; --keep-rate caps them separately.
- Instruments and samples no preset uses are dropped.

Needs sox (brew install sox) for decoding and resampling.
"""
import argparse
import array
import math
import os
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

MIN_RATE = 8000
PAD = 46  # zero samples after each sample (SF2 2.04 section 6.1)

# Generator numbers
START_OFS, END_OFS, LOOPSTART_OFS, LOOPEND_OFS, START_COARSE = 0, 1, 2, 3, 4
END_COARSE, LOOPSTART_COARSE, LOOPEND_COARSE = 12, 45, 50
PAN, INSTRUMENT, KEY_RANGE, VEL_RANGE, ATTENUATION, SAMPLE_ID = 17, 41, 43, 44, 48, 53
OFFSET_PAIRS = [(START_OFS, START_COARSE), (END_OFS, END_COARSE),
                (LOOPSTART_OFS, LOOPSTART_COARSE), (LOOPEND_OFS, LOOPEND_COARSE)]

MONO, RIGHT, LEFT, LINKED, VORBIS, ROM = 1, 2, 4, 8, 0x10, 0x8000


# ---- RIFF ----

def chunks(data, pos, end):
    while pos + 8 <= end:
        cid = data[pos:pos + 4].decode("latin-1")
        size = struct.unpack_from("<I", data, pos + 4)[0]
        yield cid, pos + 8, size
        pos += 8 + size + (size & 1)


def find_list(data, kind):
    for cid, pos, size in chunks(data, 12, len(data)):
        if cid == "LIST" and data[pos:pos + 4] == kind.encode():
            return {c: data[p:p + s] for c, p, s in chunks(data, pos + 4, pos + size)}, \
                   [(c, data[p:p + s]) for c, p, s in chunks(data, pos + 4, pos + size)]
    sys.exit(f"missing LIST {kind}")


def riff_chunk(cid, payload):
    out = cid.encode() + struct.pack("<I", len(payload)) + payload
    return out + (b"\0" if len(payload) & 1 else b"")


def riff_list(kind, subchunks):
    body = kind.encode() + b"".join(riff_chunk(c, p) for c, p in subchunks)
    return riff_chunk("LIST", body)


# ---- pdta ----

def s16(amount):
    return struct.unpack("<h", amount)[0]


def amount16(value):
    return struct.pack("<h", value)


class Zone:
    def __init__(self, gens, mods):
        self.gens = gens  # list of [oper, 2-byte amount]
        self.mods = mods  # raw 10-byte records

    def get(self, oper):
        for g in self.gens:
            if g[0] == oper:
                return g[1]
        return None

    def set(self, oper, amount):
        for g in self.gens:
            if g[0] == oper:
                g[1] = amount
                return
        # keyRange / velRange stay first, instrument / sampleID stay last
        at = len(self.gens) - 1 if self.gens and self.gens[-1][0] in (INSTRUMENT, SAMPLE_ID) else len(self.gens)
        self.gens.insert(at, [oper, amount])

    def remove(self, oper):
        self.gens = [g for g in self.gens if g[0] != oper]

    def last(self, oper):
        return self.gens[-1][0] == oper if self.gens else False


def read_items(hdr, bag, mod, gen, rec, name_len=20):
    """Returns [(header bytes, [Zone])] without the terminal record."""
    n = len(hdr) // rec
    bags = [struct.unpack_from("<HH", bag, i * 4) for i in range(len(bag) // 4)]
    mods = [mod[i * 10:i * 10 + 10] for i in range(len(mod) // 10)]
    gens = [[struct.unpack_from("<H", gen, i * 4)[0], gen[i * 4 + 2:i * 4 + 4]] for i in range(len(gen) // 4)]
    bag_ofs = 24 if rec == 38 else 20  # phdr: name, preset, bank, bag; inst: name, bag
    items = []
    for i in range(n - 1):
        h = hdr[i * rec:(i + 1) * rec]
        b0 = struct.unpack_from("<H", h, bag_ofs)[0]
        b1 = struct.unpack_from("<H", hdr, (i + 1) * rec + bag_ofs)[0]
        zones = []
        for b in range(b0, min(b1, len(bags) - 1)):
            g0, m0 = bags[b]
            g1, m1 = bags[b + 1]
            zones.append(Zone([list(x) for x in gens[g0:g1]], mods[m0:m1]))
        items.append((h, zones))
    return items


def write_items(items, rec, terminal_name):
    bag_ofs = 24 if rec == 38 else 20  # phdr: name, preset, bank, bag; inst: name, bag
    hdr, bag, mod, gen = bytearray(), bytearray(), bytearray(), bytearray()
    nb = ng = nm = 0
    for h, zones in items:
        hdr += h[:bag_ofs] + struct.pack("<H", nb) + h[bag_ofs + 2:]
        for z in zones:
            bag += struct.pack("<HH", ng, nm)
            for oper, amount in z.gens:
                gen += struct.pack("<H", oper) + amount
            for m in z.mods:
                mod += m
            nb += 1
            ng += len(z.gens)
            nm += len(z.mods)
    term = bytearray(rec)
    term[:len(terminal_name)] = terminal_name.encode()
    struct.pack_into("<H", term, bag_ofs, nb)
    hdr += term
    bag += struct.pack("<HH", ng, nm)
    mod += bytes(10)
    gen += bytes(4)
    return bytes(hdr), bytes(bag), bytes(mod), bytes(gen)


# ---- samples ----

class Sample:
    def __init__(self, rec, index):
        (self.name, self.start, self.end, self.loop_start, self.loop_end, self.rate,
         self.pitch, self.correction, self.link, self.type) = struct.unpack("<20sIIIIIBbHH", rec)
        self.index = index
        self.data = None  # array('h'), loop points relative to it
        self.orig_rate = self.new_rate = None
        self.comp_cb = 0  # attenuation to remove after folding to mono

    @property
    def has_loop(self):
        return 0 <= self.loop_start and self.loop_start + 2 <= self.loop_end <= len(self.data)


def sox(args, data):
    p = subprocess.run(["sox", "-V1", "-D"] + args, input=data, capture_output=True, check=True)
    return array.array("h", p.stdout)


def resample(data, rate_in, rate_out):
    raw = ["-t", "raw", "-e", "signed", "-b", "16", "-c", "1", "-L"]
    return sox(raw + ["-r", str(rate_in), "-"] + raw + ["-", "rate", "-v", str(rate_out)], data.tobytes())


def load_samples(shdr, smpl):
    samples = [Sample(shdr[i * 46:(i + 1) * 46], i) for i in range(len(shdr) // 46 - 1)]

    def load(s):
        if s.type & ROM:
            s.data = array.array("h")
        elif s.type & VORBIS:
            # SF3: start/end are byte offsets of an Ogg stream, loop points are relative
            s.data = sox(["-t", "ogg", "-", "-t", "raw", "-e", "signed", "-b", "16", "-c", "1", "-L", "-"],
                         smpl[s.start:s.end])
        else:
            s.data = array.array("h", smpl[s.start * 2:s.end * 2])
            s.loop_start -= s.start
            s.loop_end -= s.start
        if s.rate < 400:
            s.rate = 44100
        s.orig_rate = s.new_rate = s.rate

    with ThreadPoolExecutor() as ex:
        list(ex.map(load, samples))
    return samples


def fold_stereo(instruments, samples):
    """Mixes each stereo pair into its left sample and drops the right zones."""
    pairs = {}
    for s in samples:
        if s.type & (LEFT | RIGHT) and s.link < len(samples):
            left, right = (s, samples[s.link]) if s.type & LEFT else (samples[s.link], s)
            pairs[left.index] = right.index
            pairs[right.index] = right.index
    mixed = {}  # right index -> left index

    for li, ri in list(pairs.items()):
        if li == ri:
            continue
        left, right = samples[li], samples[ri]
        same = (len(left.data) == len(right.data) and left.loop_start == right.loop_start
                and left.loop_end == right.loop_end and left.rate == right.rate)
        if same:
            mix = [a + b for a, b in zip(left.data, right.data)]
            target = math.sqrt(0.5)
        else:
            mix = list(left.data)
            target = math.sqrt(2.0)
        peak = max((abs(v) for v in mix), default=0)
        g = min(target, 32767 / peak) if peak else target
        left.data = array.array("h", (int(round(v * g)) for v in mix))
        left.comp_cb = round(200 * math.log10(target / g)) if g < target else 0
        left.type, left.link = MONO, 0
        mixed[ri] = li

    for _, zones in instruments:
        globl = zones[0] if zones and not zones[0].last(SAMPLE_ID) else None
        kept = []
        for z in zones:
            if not z.last(SAMPLE_ID):
                kept.append(z)
                continue
            si = struct.unpack("<H", z.get(SAMPLE_ID))[0]
            if si in mixed:
                li = mixed[si]
                twin = any(o.last(SAMPLE_ID) and struct.unpack("<H", o.get(SAMPLE_ID))[0] == li
                           and o.get(KEY_RANGE) == z.get(KEY_RANGE) and o.get(VEL_RANGE) == z.get(VEL_RANGE)
                           for o in zones)
                if twin:
                    continue
                z.set(SAMPLE_ID, struct.pack("<H", li))
                si = li
            if si in pairs:
                # Stereo halves are hard-panned; the centered mono sample plays at -3dB
                z.set(PAN, amount16(0))
                att = z.get(ATTENUATION) or (globl.get(ATTENUATION) if globl else None) or amount16(0)
                z.set(ATTENUATION, amount16(max(0, s16(att) - 30 - samples[si].comp_cb)))
            kept.append(z)
        zones[:] = kept


def shrink_rate(s, target):
    """Per-sample output rate: close to target, with an integer loop length."""
    if s.rate <= target or not len(s.data):
        return s.rate
    if not s.has_loop:
        return target
    length = s.loop_end - s.loop_start
    new_len = max(1, round(length * target / s.rate))
    return min(s.rate, round(s.rate * new_len / length))


def apply_rate(s):
    if s.new_rate == s.rate:
        return
    ratio = s.new_rate / s.rate
    if s.has_loop:
        ls, le = s.loop_start, s.loop_end
        # Resample the loop as it is played (followed by itself), so the filter
        # sees the right continuation across the loop end.
        reps = math.ceil(1024 / (le - ls)) + 1
        looped = resample(s.data[:le] + s.data[ls:le] * reps, s.rate, s.new_rate)
        new_ls = round(ls * ratio)
        new_le = new_ls + round((le - ls) * ratio)
        out = looped[:new_le + 1]
        if len(s.data) > le + 1:
            out += resample(s.data, s.rate, s.new_rate)[new_le + 1:]
        s.data, s.loop_start, s.loop_end = out, new_ls, new_le
    else:
        s.data = resample(s.data, s.rate, s.new_rate)
        s.loop_start = round(s.loop_start * ratio)
        s.loop_end = round(s.loop_end * ratio)
    s.rate = s.new_rate


def scale_offsets(zone, ratio):
    for fine, coarse in OFFSET_PAIRS:
        f, c = zone.get(fine), zone.get(coarse)
        if f is None and c is None:
            continue
        total = (s16(f) if f else 0) + (s16(c) if c else 0) * 32768
        total = round(total * ratio)
        c_new = int(total / 32768)
        f_new = total - c_new * 32768
        zone.set(fine, amount16(f_new))
        if c is not None or c_new:
            zone.set(coarse, amount16(c_new))


def prune_instruments(presets, instruments):
    """Empties instruments no preset uses and drops the unused ones off the end."""
    used = {struct.unpack("<H", z.get(INSTRUMENT))[0] for _, zones in presets for z in zones if z.last(INSTRUMENT)}
    remap = {old: new for new, old in enumerate(sorted(i for i in used if i < len(instruments)))}
    for _, zones in presets:
        zones[:] = [z for z in zones if not z.last(INSTRUMENT) or struct.unpack("<H", z.get(INSTRUMENT))[0] in remap]
        for z in zones:
            if z.last(INSTRUMENT):
                z.set(INSTRUMENT, struct.pack("<H", remap[struct.unpack("<H", z.get(INSTRUMENT))[0]]))
    instruments[:] = [instruments[i] for i in sorted(remap)]


def parse_programs(text):
    progs = set()
    for part in text.split(","):
        lo, _, hi = part.partition("-")
        progs.update(range(int(lo), int(hi or lo) + 1))
    return progs


def kept_samples(presets, instruments, programs):
    """Sample indices used by the bank 0 presets with the given programs."""
    out = set()
    for h, zones in presets:
        program, bank = struct.unpack_from("<HH", h, 20)
        if bank or program not in programs:
            continue
        for z in zones:
            if not z.last(INSTRUMENT):
                continue
            for y in instruments[struct.unpack("<H", z.get(INSTRUMENT))[0]][1]:
                if y.last(SAMPLE_ID):
                    out.add(struct.unpack("<H", y.get(SAMPLE_ID))[0])
    return out


def count_regions(presets, instruments):
    """Approximates the region count the firmware builds (Region is 32 bytes)."""
    def rng(z, g, oper):
        a = (z.get(oper) if z else None) or (g.get(oper) if g else None) or b"\x00\x7f"
        return a[0], a[1]

    total = 0
    for _, pz in presets:
        pg = pz[0] if pz and not pz[0].last(INSTRUMENT) else None
        for z in pz:
            if not z.last(INSTRUMENT):
                continue
            ii = struct.unpack("<H", z.get(INSTRUMENT))[0]
            if ii >= len(instruments):
                continue
            pk, pv = rng(z, pg, KEY_RANGE), rng(z, pg, VEL_RANGE)
            iz = instruments[ii][1]
            ig = iz[0] if iz and not iz[0].last(SAMPLE_ID) else None
            for y in iz:
                if not y.last(SAMPLE_ID):
                    continue
                ik, iv = rng(y, ig, KEY_RANGE), rng(y, ig, VEL_RANGE)
                if max(pk[0], ik[0]) <= min(pk[1], ik[1]) and max(pv[0], iv[0]) <= min(pv[1], iv[1]):
                    total += 1
    return total


def parse_size(text):
    mult = {"K": 1 << 10, "M": 1 << 20}.get(text[-1:].upper(), 1)
    return int(float(text.rstrip("KkMm")) * mult)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--max-size", type=parse_size, help="output size limit, e.g. 13M (picks the rate)")
    ap.add_argument("--rate", type=int, help="maximum sample rate (default: keep, or from --max-size)")
    ap.add_argument("--mono", action="store_true", help="fold stereo pairs into mono samples")
    ap.add_argument("--banks", help="comma-separated banks to keep, e.g. 0,128")
    ap.add_argument("--keep", help="bank 0 programs whose samples keep their rate, e.g. 0-7,11")
    ap.add_argument("--keep-rate", type=int, help="maximum sample rate of the --keep samples")
    ap.add_argument("--name", help="INAM (bank name) of the output")
    args = ap.parse_args()

    with open(args.input, "rb") as f:
        data = f.read()
    if data[:4] != b"RIFF" or data[8:12] != b"sfbk":
        sys.exit("not a SoundFont")
    info, info_list = find_list(data, "INFO")
    sdta, _ = find_list(data, "sdta")
    pdta, _ = find_list(data, "pdta")

    presets = read_items(pdta["phdr"], pdta["pbag"], pdta["pmod"], pdta["pgen"], 38)
    instruments = read_items(pdta["inst"], pdta["ibag"], pdta["imod"], pdta["igen"], 22)
    print(f"{args.input}: {len(presets)} presets, {len(instruments)} instruments", file=sys.stderr)
    if args.banks:
        keep = {int(b) for b in args.banks.split(",")}
        presets = [p for p in presets if struct.unpack_from("<H", p[0], 22)[0] in keep]
        print(f"banks {sorted(keep)}: {len(presets)} presets", file=sys.stderr)
    prune_instruments(presets, instruments)
    samples = load_samples(pdta["shdr"], sdta.get("smpl", b""))

    if args.mono:
        fold_stereo(instruments, samples)

    used = sorted({struct.unpack("<H", z.get(SAMPLE_ID))[0]
                   for _, zones in instruments for z in zones if z.last(SAMPLE_ID)})
    used = [i for i in used if i < len(samples) and not samples[i].type & ROM]
    print(f"samples: {len(used)} used of {len(samples)}", file=sys.stderr)

    keep = kept_samples(presets, instruments, parse_programs(args.keep)) if args.keep else set()
    if keep:
        kept_bytes = sum(len(samples[i].data) * 2 for i in keep)
        print(f"--keep: {len(keep)} samples, {kept_bytes / 1e6:.2f}MB before resampling", file=sys.stderr)

    def cap(i, target):
        return args.keep_rate if i in keep else target

    overhead = 1024 + len(pdta["phdr"]) + len(pdta["pbag"]) + len(pdta["pmod"]) + len(pdta["pgen"]) + \
        len(pdta["inst"]) + len(pdta["ibag"]) + len(pdta["imod"]) + len(pdta["igen"]) * 2 + 46 * len(used)

    def estimate(target):
        return overhead + sum((len(samples[i].data) * min(1.0, (cap(i, target) or samples[i].rate) / samples[i].rate)
                               + PAD) * 2 for i in used)

    target = args.rate
    if args.max_size and not target:
        if estimate(MIN_RATE) > args.max_size * 0.98:
            sys.exit(f"does not fit in {args.max_size} bytes even at {MIN_RATE}Hz")
        target = max(r for r in range(MIN_RATE, 48001, 100) if estimate(r) <= args.max_size * 0.98)
    if target or args.keep_rate:
        if target:
            print(f"target rate: {target}Hz (estimate {estimate(target) / 1e6:.2f}MB)", file=sys.stderr)
        for i in used:
            rate = cap(i, target)
            if rate:
                samples[i].new_rate = shrink_rate(samples[i], rate)
        with ThreadPoolExecutor() as ex:
            list(ex.map(apply_rate, (samples[i] for i in used)))

    # Remap sample indices and rebuild smpl / shdr
    remap = {old: new for new, old in enumerate(used)}
    smpl = array.array("h")
    shdr = bytearray()
    for i in used:
        s = samples[i]
        start = len(smpl)
        smpl += s.data
        smpl += array.array("h", bytes(PAD * 2))
        link = remap.get(s.link, 0) if s.type & (LEFT | RIGHT | LINKED) else 0
        stype = s.type & ~VORBIS
        if s.type & (LEFT | RIGHT) and s.link not in remap:
            stype, link = MONO, 0
        shdr += struct.pack("<20sIIIIIBbHH", s.name, start, start + len(s.data),
                            start + s.loop_start, start + s.loop_end, s.rate, s.pitch, s.correction, link, stype)
    shdr += struct.pack("<20sIIIIIBbHH", b"EOS", 0, 0, 0, 0, 0, 0, 0, 0, 0)

    for _, zones in instruments:
        kept = []
        for z in zones:
            if z.last(SAMPLE_ID):
                old = struct.unpack("<H", z.get(SAMPLE_ID))[0]
                if old not in remap:
                    continue
                s = samples[old]
                if s.rate != s.orig_rate:
                    scale_offsets(z, s.rate / s.orig_rate)
                z.set(SAMPLE_ID, struct.pack("<H", remap[old]))
            kept.append(z)
        zones[:] = kept

    phdr, pbag, pmod, pgen = write_items(presets, 38, "EOP")
    inst, ibag, imod, igen = write_items(instruments, 22, "EOI")

    info_out = []
    for cid, payload in info_list:
        if cid == "ifil":
            payload = struct.pack("<HH", 2, 4)
        elif cid == "INAM" and args.name:
            payload = args.name.encode() + b"\0"
        elif cid == "ICMT":
            continue
        info_out.append((cid, payload))
    note = f"Converted by sf2shrink.py from {os.path.basename(args.input)}"
    if target:
        note += f", resampled to <= {target}Hz"
    if args.mono:
        note += ", stereo folded to mono"
    old_cmt = info.get("ICMT", b"").rstrip(b"\0")
    cmt = (old_cmt + b"\n\n" if old_cmt else b"") + note.encode()
    info_out.append(("ICMT", cmt[:65535 - 2] + (b"\0" if len(cmt) % 2 else b"\0\0")))

    body = b"sfbk" + riff_list("INFO", info_out) + riff_list("sdta", [("smpl", smpl.tobytes())]) + \
        riff_list("pdta", [("phdr", phdr), ("pbag", pbag), ("pmod", pmod), ("pgen", pgen), ("inst", inst),
                           ("ibag", ibag), ("imod", imod), ("igen", igen), ("shdr", bytes(shdr))])
    out = riff_chunk("RIFF", body)
    with open(args.output, "wb") as f:
        f.write(out)

    regions = count_regions(presets, instruments)
    print(f"{args.output}: {len(out):,} bytes ({len(out) / (1 << 20):.2f}MiB), "
          f"{len(used)} samples, about {regions} regions (~{regions * 32 // 1024}KB + params in RAM)",
          file=sys.stderr)
    if args.max_size and len(out) > args.max_size:
        sys.exit(f"output exceeds --max-size ({args.max_size:,} bytes)")


if __name__ == "__main__":
    main()
