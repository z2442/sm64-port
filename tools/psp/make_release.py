#!/usr/bin/env python3
"""make_release.py - split a finished PSP build into a ROM-free template and an install manifest.

The normal build is NOT touched. Run this AFTER a regular build, only when you want to prepare a
package that does not contain any data taken from the Nintendo ROM:

    python3 tools/psp/make_release.py --version us --verify

It reads (nothing is modified):
    build/<ver>_psp/mario64/SM64.PRX         the finished module
    build/<ver>_psp/sound/sound_data.{ctl,tbl}   the sound banks as built
    baserom.<lang>.z64, assets.json          the same inputs extract_assets.py uses

and writes (default: dist/psp_release/<ver>/):
    SM64.template.prx   the module with every byte that comes from the ROM set to zero
    manifest.bin        where each of those bytes comes from (ROM offsets, no ROM data)
    report.txt          what was found, what was not

An installer (psp/installer/, on the PSP; or `--install` on a PC) takes the user's own ROM, the
template and the manifest and writes back a byte-identical SM64.PRX.

With --pack the tool also builds that installer EBOOT (make -C psp/installer VERSION=<ver>, needs the
PSP toolchain) and assembles the folder to copy to the Memory Stick:
    <out>/PSP/GAME/SM64_<VER>/EBOOT.PBP, SM64.template.prx, manifest.bin, data/README.txt

Only the Python standard library is needed.

manifest.bin layout (little endian):
    header   '<4sIIIIII'  magic 'SMAN', format version (1), number of ROMs, number of entries,
                          size of the swap maps, template size, reserved
    per ROM  '<4s20s'     language tag ('us', 'jp', ...) and the SHA-1 the ROM must have
    '<20s20s'             SHA-1 of the template and SHA-1 of the final SM64.PRX
    entries  '<IIBBHIII'  prx_offset, length, rom_id, kind (0 = raw ROM bytes, 1 = MIO0 block),
                          reserved, mio0_offset, source_offset, map_offset (0xFFFFFFFF = none)
    maps                  2 bits per 32-bit word: 0 = copy, 1 = reverse the 4 bytes,
                          2 = reverse each 16-bit half
    For kind 1 the source is the decompressed MIO0 block found at mio0_offset in the ROM and
    source_offset is relative to that decompressed data; for kind 0 it is relative to the ROM.
"""
import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys

# Which ROMs a version needs. Must agree with EXTRACT_LANGS in the Makefile (Shindou: textures from
# the SH ROM, sound from the JP ROM).
VERSION_LANGS = {"us": ["us"], "jp": ["jp"], "eu": ["eu"], "sh": ["jp", "sh"]}

MAGIC = b"SMAN"
FORMAT_VERSION = 1
HDR = "<4sIIIIII"
ROMREC = "<4s20s"
SHAS = "<20s20s"
ENT = "<IIBBHIII"
NOMAP = 0xFFFFFFFF

WINDOW = 32          # size of the probe used to locate fragments of a blob
MAX_OCCURRENCES = 256


def die(msg):
    sys.exit("make_release: " + msg)


def sha1(data):
    return hashlib.sha1(data).digest()


# ---------------------------------------------------------------------------------------------
# ROM access (this part is what the on-device installer has to reimplement)
# ---------------------------------------------------------------------------------------------

def normalize_rom(data):
    """Return the ROM in big-endian (.z64) order. Accepts .z64, .v64 (16-bit swapped) and .n64."""
    magic = data[:4]
    if magic == b"\x80\x37\x12\x40":
        return data
    if magic == b"\x37\x80\x40\x12":      # .v64: every 16-bit pair is swapped
        out = bytearray(data)
        out[0::2], out[1::2] = data[1::2], data[0::2]
        return bytes(out)
    if magic == b"\x40\x12\x37\x80":      # .n64: every 32-bit word is reversed
        out = bytearray(data)
        out[0::4], out[1::4], out[2::4], out[3::4] = data[3::4], data[2::4], data[1::4], data[0::4]
        return bytes(out)
    die("not a Nintendo 64 ROM (unknown header)")


def mio0_decode(rom, off):
    """Decompress the MIO0 block at 'off'."""
    if rom[off:off + 4] != b"MIO0":
        die("no MIO0 block at ROM offset %d" % off)
    n, comp_off, raw_off = struct.unpack(">III", rom[off + 4:off + 16])
    out = bytearray()
    bits_pos, comp_pos, raw_pos = off + 16, off + comp_off, off + raw_off
    bits = mask = 0
    while len(out) < n:
        if mask == 0:
            bits = struct.unpack(">I", rom[bits_pos:bits_pos + 4])[0]
            bits_pos += 4
            mask = 0x80000000
        if bits & mask:
            out.append(rom[raw_pos])
            raw_pos += 1
        else:
            v = (rom[comp_pos] << 8) | rom[comp_pos + 1]
            comp_pos += 2
            length = (v >> 12) + 3
            dist = (v & 0xFFF) + 1
            for _ in range(length):
                out.append(out[-dist])
        mask >>= 1
    return bytes(out[:n])


class RomSet:
    """The ROMs of one version plus a cache of decompressed MIO0 blocks."""

    def __init__(self, roms):
        self.roms = roms            # list of big-endian ROM images, index = rom_id
        self.cache = {}

    def block(self, rom_id, mio0_off):
        key = (rom_id, mio0_off)
        if key not in self.cache:
            self.cache[key] = mio0_decode(self.roms[rom_id], mio0_off)
        return self.cache[key]

    def read(self, rom_id, kind, mio0_off, src_off, length):
        if kind == 0:
            return self.roms[rom_id][src_off:src_off + length]
        return self.block(rom_id, mio0_off)[src_off:src_off + length]


def apply_map(data, maps, map_off):
    out = bytearray()
    for w in range(len(data) // 4):
        code = (maps[map_off + w // 4] >> ((w % 4) * 2)) & 3
        x = data[w * 4:w * 4 + 4]
        out += x if code == 0 else x[::-1] if code == 1 else x[1::-1] + x[3:1:-1]
    return bytes(out)


# ---------------------------------------------------------------------------------------------
# Reference installer: ROM(s) + template + manifest -> SM64.PRX
# ---------------------------------------------------------------------------------------------

def install(template, manifest, rom_images):
    magic, fmt, nroms, nents, nmaps, tsize, _ = struct.unpack_from(HDR, manifest, 0)
    if magic != MAGIC or fmt != FORMAT_VERSION:
        die("unsupported manifest")
    pos = struct.calcsize(HDR)
    langs = []
    for _ in range(nroms):
        lang, h = struct.unpack_from(ROMREC, manifest, pos)
        langs.append((lang.rstrip(b"\0").decode(), h))
        pos += struct.calcsize(ROMREC)
    tsha, osha = struct.unpack_from(SHAS, manifest, pos)
    pos += struct.calcsize(SHAS)
    ents = [struct.unpack_from(ENT, manifest, pos + i * struct.calcsize(ENT)) for i in range(nents)]
    maps = manifest[pos + nents * struct.calcsize(ENT):pos + nents * struct.calcsize(ENT) + nmaps]
    if len(template) != tsize or sha1(template) != tsha:
        die("the template does not match the manifest")
    roms = []
    for lang, h in langs:
        img = rom_images.get(lang)
        if img is None:
            die("missing ROM for '%s'" % lang)
        img = normalize_rom(img)
        if sha1(img) != h:
            die("the '%s' ROM is not the expected version" % lang)
        roms.append(img)
    rs = RomSet(roms)
    out = bytearray(template)
    for prx_off, length, rom_id, kind, _r, mio0_off, src_off, map_off in ents:
        data = rs.read(rom_id, kind, mio0_off, src_off, length)
        if map_off != NOMAP:
            data = apply_map(data, maps, map_off)
        out[prx_off:prx_off + length] = data
    if sha1(out) != osha:
        die("the rebuilt module does not match the expected checksum")
    return bytes(out)


# ---------------------------------------------------------------------------------------------
# Generator
# ---------------------------------------------------------------------------------------------

def find_all(hay, needle, limit=MAX_OCCURRENCES):
    out = []
    i = hay.find(needle)
    while i >= 0 and len(out) <= limit:
        out.append(i)
        i = hay.find(needle, i + 1)
    return out


def classify_words(rom_part, built_part):
    """Return a list of swap codes, one per 32-bit word, or None if the two are not related."""
    codes = []
    for i in range(0, len(rom_part), 4):
        r, b = rom_part[i:i + 4], built_part[i:i + 4]
        if r == b:
            codes.append(0)
        elif r[::-1] == b:
            codes.append(1)
        elif r[1::-1] + r[3:1:-1] == b:
            codes.append(2)
        else:
            return None
    return codes


ROM_NAMES = {"us": "US (NTSC-U)", "jp": "Japan (NTSC-J)", "eu": "Europe (PAL)", "sh": "Shindou (Rumble Pak Edition)"}

# Text of data/README.txt, in the languages of the installer (English first). {roms} is replaced by the ROM(s) the package needs.
DATA_README = [
    ("English",
     "Copy your own original Super Mario 64 ROM into this folder (any file name; .z64, .v64 or .n64).\n"
     "ROM needed for this package: {roms}\n"
     "Start the game once: it prepares itself from your ROM (it can take a while, only the first time) and starts.\n"
     "After that, the ROM, SM64.template.prx and manifest.bin are no longer needed and can be deleted."),
    ("Français",
     "Copiez votre propre ROM originale de Super Mario 64 dans ce dossier (nom libre ; .z64, .v64 ou .n64).\n"
     "ROM nécessaire pour ce paquet : {roms}\n"
     "Lancez le jeu une fois : il se prépare à partir de votre ROM (cela peut prendre un moment, seulement la première fois) puis démarre.\n"
     "Ensuite, la ROM, SM64.template.prx et manifest.bin ne sont plus nécessaires et peuvent être supprimés."),
    ("Deutsch",
     "Kopiere deine eigene originale Super-Mario-64-ROM in diesen Ordner (beliebiger Dateiname; .z64, .v64 oder .n64).\n"
     "Für dieses Paket benötigte ROM: {roms}\n"
     "Starte das Spiel einmal: Es bereitet sich aus deiner ROM vor (das kann etwas dauern, nur beim ersten Mal) und startet.\n"
     "Danach werden die ROM, SM64.template.prx und manifest.bin nicht mehr gebraucht und können gelöscht werden."),
    ("Español",
     "Copia tu propia ROM original de Super Mario 64 en esta carpeta (cualquier nombre; .z64, .v64 o .n64).\n"
     "ROM necesaria para este paquete: {roms}\n"
     "Abre el juego una vez: se prepara solo con tu ROM (puede tardar un rato, solo la primera vez) y arranca.\n"
     "Después ya no hacen falta la ROM, SM64.template.prx ni manifest.bin, y se pueden borrar."),
    ("Italiano",
     "Copia la tua ROM originale di Super Mario 64 in questa cartella (qualsiasi nome; .z64, .v64 o .n64).\n"
     "ROM necessaria per questo pacchetto: {roms}\n"
     "Avvia il gioco una volta: si prepara da solo con la tua ROM (può richiedere un po' di tempo, solo la prima volta) e parte.\n"
     "Dopo, la ROM, SM64.template.prx e manifest.bin non servono più e si possono eliminare."),
    ("日本語",
     "お持ちのスーパーマリオ64のオリジナルROMをこのフォルダにコピーしてください（ファイル名は自由、.z64 / .v64 / .n64）。\n"
     "このパッケージに必要なROM：{roms}\n"
     "ゲームを一度起動すると、ROMから自動的に準備され（初回のみ、時間がかかることがあります）、そのまま始まります。\n"
     "その後は、ROM、SM64.template.prx、manifest.bin は不要なので削除してかまいません。"),
]


def pack_release(repo, ver, langs, out_dir, installer):
    """Assemble <out_dir>/PSP/GAME/SM64_<VER>/ with the installer EBOOT, the template, the manifest and data/."""
    if installer is None:
        subprocess.check_call(["make", "-C", os.path.join(repo, "psp", "installer"), "VERSION=" + ver])
        installer = os.path.join(repo, "build", ver + "_psp", "installer", "EBOOT.PBP")
    if not os.path.isfile(installer):
        die("installer EBOOT not found: " + installer)
    dst = os.path.join(out_dir, "PSP", "GAME", "SM64_" + ver.upper())
    os.makedirs(os.path.join(dst, "data"), exist_ok=True)
    shutil.copyfile(installer, os.path.join(dst, "EBOOT.PBP"))
    shutil.copyfile(os.path.join(out_dir, "SM64.template.prx"), os.path.join(dst, "SM64.template.prx"))
    shutil.copyfile(os.path.join(out_dir, "manifest.bin"), os.path.join(dst, "manifest.bin"))
    names = " + ".join(ROM_NAMES[l] for l in langs)
    with open(os.path.join(dst, "data", "README.txt"), "w", encoding="utf-8") as f:
        f.write("\n\n".join("== %s ==\n%s" % (lang, text.format(roms=names)) for lang, text in DATA_README) + "\n")
    return dst


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", choices=sorted(VERSION_LANGS), help="game version (needs its build)")
    ap.add_argument("--repo", default=".", help="repository root (default: current directory)")
    ap.add_argument("--out", help="output directory (default: dist/psp_release/<version>)")
    ap.add_argument("--verify", action="store_true", help="rebuild the module from ROM+template+manifest and compare")
    ap.add_argument("--leak-check", action="store_true", help="scan the template for data that still matches the ROM assets (slow)")
    ap.add_argument("--install", metavar="DIR", help="reference installer: DIR has SM64.template.prx and manifest.bin; "
                    "ROMs are given with --rom LANG=FILE; writes SM64.PRX next to them")
    ap.add_argument("--rom", action="append", default=[], metavar="LANG=FILE")
    ap.add_argument("--pack", action="store_true", help="also assemble the folder to copy to the Memory Stick (builds the installer EBOOT)")
    ap.add_argument("--installer", metavar="EBOOT.PBP", help="with --pack: use this already built installer EBOOT instead of running make")
    args = ap.parse_args()

    if args.install:
        roms = {}
        for item in args.rom:
            lang, _, path = item.partition("=")
            roms[lang] = open(path, "rb").read()
        tpl = open(os.path.join(args.install, "SM64.template.prx"), "rb").read()
        man = open(os.path.join(args.install, "manifest.bin"), "rb").read()
        prx = install(tpl, man, roms)
        dst = os.path.join(args.install, "SM64.PRX")
        open(dst, "wb").write(prx)
        print("wrote %s (%d bytes, sha1 %s)" % (dst, len(prx), hashlib.sha1(prx).hexdigest()))
        return
    if not args.version:
        ap.error("--version is required")

    ver = args.version
    repo = args.repo
    langs = VERSION_LANGS[ver]
    out_dir = args.out or os.path.join(repo, "dist", "psp_release", ver)

    prx_path = os.path.join(repo, "build", ver + "_psp", "mario64", "SM64.PRX")
    ctl_path = os.path.join(repo, "build", ver + "_psp", "sound", "sound_data.ctl")
    tbl_path = os.path.join(repo, "build", ver + "_psp", "sound", "sound_data.tbl")
    for p in (prx_path, ctl_path, tbl_path):
        if not os.path.isfile(p):
            die("%s not found; build version '%s' first" % (p, ver))
    prx = open(prx_path, "rb").read()
    ctl_built = open(ctl_path, "rb").read()
    tbl_built = open(tbl_path, "rb").read()
    with open(os.path.join(repo, "assets.json")) as f:
        asset_map = json.load(f)

    # ROMs, checked like extract_assets.py does
    images, rom_sha = [], []
    for lang in langs:
        path = os.path.join(repo, "baserom.%s.z64" % lang)
        if not os.path.isfile(path):
            die("%s not found" % path)
        img = normalize_rom(open(path, "rb").read())
        expected = open(os.path.join(repo, "sm64.%s.sha1" % lang)).read().split()[0]
        if hashlib.sha1(img).hexdigest() != expected:
            die("%s has the wrong hash" % path)
        images.append(img)
        rom_sha.append(sha1(img))
    rs = RomSet(images)

    # ---- candidate blobs: every asset the build could have taken from the ROM(s)
    # candidate = (rom_id, kind, mio0_off, src_off, length); several per asset because the same file
    # name has different data in each version
    blobs = []     # (name, [candidate, ...])
    for name, data in asset_map.items():
        if name.startswith("@"):
            continue
        length, positions = data[-2], data[-1]
        cands = []
        for lang, pos in positions.items():
            if lang not in langs:
                continue
            rid = langs.index(lang)
            if len(pos) == 1:
                cands.append((rid, 0, 0, pos[0], length))
            elif pos[0] != "@sound":       # sound samples are rebuilt through ctl/tbl, see below
                cands.append((rid, 1, pos[0], pos[1], length))
        if cands:
            blobs.append((name, cands))

    entries = []   # (prx_off, length, rom_id, kind, mio0_off, src_off, map_off)
    maps = bytearray()
    report = []
    unmapped = []

    def add_entry(prx_off, length, cand, delta=0, map_off=NOMAP):
        rid, kind, mo, so, _ = cand
        entries.append((prx_off, length, rid, kind, mo, so + delta, map_off))

    # ---- 1. whole blobs, then fragments of the ones that are split in the binary
    for name, cands in blobs:
        done = False
        for cand in cands:
            data = rs.read(*cand[:4], cand[4])
            if len(data) < 16 or len(set(data)) <= 1:
                done = True
                break
            occ = find_all(prx, data)
            if occ and (len(data) >= WINDOW or len(occ) == 1) and len(occ) <= MAX_OCCURRENCES:
                for o in occ:
                    add_entry(o, len(data), cand)
                done = True
                break
        if done:
            continue
        # fragments (skyboxes, the cake picture): locate unique WINDOW-byte probes and extend them
        best = None
        for cand in cands:
            data = rs.read(*cand[:4], cand[4])
            if len(data) < WINDOW:
                continue
            found = []
            run = None
            for i in range(0, len(data) - WINDOW + 1, WINDOW):
                w = data[i:i + WINDOW]
                if len(set(w)) <= 1:
                    continue
                occ = find_all(prx, w, 1)
                if len(occ) != 1:
                    continue
                delta = occ[0] - i
                if run and run[0] == delta and run[2] == i:
                    run[2] = i + WINDOW
                else:
                    if run:
                        found.append(run)
                    run = [delta, i, i + WINDOW]
            if run:
                found.append(run)
            ext = []
            for delta, s, e in found:
                while s > 0 and prx[delta + s - 1] == data[s - 1]:
                    s -= 1
                while e < len(data) and delta + e < len(prx) and prx[delta + e] == data[e]:
                    e += 1
                ext.append((delta + s, s, e - s))
            if ext and (best is None or sum(x[2] for x in ext) > sum(x[2] for x in best[1])):
                best = (cand, ext)
        if not best:
            unmapped.append((name, 0, 0))
            continue
        cand, ext = best
        data = rs.read(*cand[:4], cand[4])
        for prx_off, so, ln in ext:
            add_entry(prx_off, ln, cand, so)
        # fill the holes (solid or repeated tiles have no unique probe) with exact chunks
        covered = sorted((so, so + ln) for _, so, ln in ext)
        holes, p = [], 0
        for a, b in covered:
            if a > p:
                holes.append((p, a))
            p = max(p, b)
        if p < len(data):
            holes.append((p, len(data)))
        for ga, gb in holes:
            for cs in (2048, 1024, 512, 256, 128, 64):
                a = (ga + cs - 1) // cs * cs
                while a + cs <= gb:
                    chunk = data[a:a + cs]
                    if len(set(chunk)) > 1:
                        occ = find_all(prx, chunk, 64)
                        if 0 < len(occ) <= 64:
                            for o in occ:
                                add_entry(o, cs, cand, a)
                    a += cs
        unmapped.append((name, len(data), sum(x[2] for x in ext)))

    # ---- 2. sound banks: the port builds them byte-swapped for the little-endian PSP, so they are
    # produced from the ROM blob with a per-word swap map; the sample data itself is a plain copy.
    for label, built in (("ctl", ctl_built), ("tbl", tbl_built)):
        key = "@sound %s " % label
        done = False
        for lang in langs:
            ent = asset_map.get(key + lang)
            if not ent:
                continue
            size, locs = ent
            if lang not in locs or size != len(built):
                continue
            rid = langs.index(lang)
            romoff = locs[lang][0]
            src = images[rid][romoff:romoff + size]
            codes = classify_words(src, built)
            if codes is None:
                continue
            last = max((i for i, c in enumerate(codes) if c), default=-1)
            head = (last + 1) * 4          # bytes that differ (swapped); the rest is a plain copy
            if size - head < 1024:
                head = size                # a short tail is not worth (or able) to be located on its own
            if head:
                packed = bytearray((head // 4 + 3) // 4)
                for i, c in enumerate(codes[:head // 4]):
                    packed[i // 4] |= c << ((i % 4) * 2)
                occ = find_all(prx, built[:head], 2)
                if len(occ) != 1:
                    die("cannot locate the sound %s in the module" % label)
                entries.append((occ[0], head, rid, 0, 0, romoff, len(maps)))
                maps.extend(packed)
            if head < size:
                tail = built[head:]
                occ = find_all(prx, tail, 2)
                if len(occ) != 1:
                    die("cannot locate the sound %s data in the module" % label)
                entries.append((occ[0], len(tail), rid, 0, 0, romoff + head, NOMAP))
            report.append("sound %s: %d bytes from the %s ROM (%d swapped, %d copied)" % (label, size, lang, head, size - head))
            done = True
            break
        if not done:
            die("sound %s of the build does not derive from any ROM blob" % label)

    # ---- 3. merge overlapping entries, build the template
    entries.sort(key=lambda e: (e[0], -e[1]))
    kept, end = [], 0
    for prx_off, length, rid, kind, mo, so, mapoff in entries:
        if prx_off + length <= end:
            continue
        if prx_off < end:
            d = end - prx_off
            prx_off += d
            length -= d
            if mapoff != NOMAP:
                die("overlapping swapped entries")
            so += d
        kept.append((prx_off, length, rid, kind, mo, so, mapoff))
        end = prx_off + length
    template = bytearray(prx)
    for prx_off, length, *_ in kept:
        template[prx_off:prx_off + length] = bytes(length)
    template = bytes(template)

    header = struct.pack(HDR, MAGIC, FORMAT_VERSION, len(langs), len(kept), len(maps), len(template), 0)
    header += b"".join(struct.pack(ROMREC, l.encode(), h) for l, h in zip(langs, rom_sha))
    header += struct.pack(SHAS, sha1(template), sha1(prx))
    body = b"".join(struct.pack(ENT, p, l, r, k, 0, m, s, mp) for p, l, r, k, m, s, mp in kept)
    manifest = header + body + bytes(maps)

    os.makedirs(out_dir, exist_ok=True)
    open(os.path.join(out_dir, "SM64.template.prx"), "wb").write(template)
    open(os.path.join(out_dir, "manifest.bin"), "wb").write(manifest)

    covered_bytes = sum(e[1] for e in kept)
    report.insert(0, "version %s, ROMs: %s" % (ver, ", ".join(langs)))
    report.append("module: %d bytes; %d bytes (%.1f%%) come from the ROM" % (len(prx), covered_bytes, 100.0 * covered_bytes / len(prx)))
    report.append("manifest: %d entries, %d bytes" % (len(kept), len(manifest)))
    partial = [u for u in unmapped if u[1] and u[2] < u[1]]
    missing = [u for u in unmapped if not u[1]]
    if partial:
        report.append("assets only partly present (usually padding at the end): " +
                      ", ".join("%s %d/%d" % (n, c, t) for n, t, c in partial))
    ok = True
    if args.verify:
        rebuilt = install(template, manifest, {l: open(os.path.join(repo, "baserom.%s.z64" % l), "rb").read() for l in langs})
        ok = rebuilt == prx
        report.append("verify: module rebuilt from ROM + template + manifest is %s" % ("IDENTICAL" if ok else "DIFFERENT"))
    if args.leak_check:
        corpus = set()
        for name, cands in blobs:
            for cand in cands:
                d = rs.read(*cand[:4], cand[4])
                if len(d) >= WINDOW:
                    corpus.add(d)
        big = b"\xff\xfe".join(sorted(corpus))
        hits = 0
        for i in range(0, len(template) - WINDOW, 64):
            w = template[i:i + WINDOW]
            if len(set(w)) >= 12 and big.find(w) >= 0:
                hits += 1
        report.append("leak check: %d probes of the template still match ROM asset data" % hits)
        ok = ok and hits == 0
    if args.pack:
        pack_dir = pack_release(repo, ver, langs, out_dir, args.installer)
        report.append("pack: " + pack_dir)
    text = "\n".join(report) + "\n"
    open(os.path.join(out_dir, "report.txt"), "w").write(text)
    print(text)
    print("written to " + out_dir)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
