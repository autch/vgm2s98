#!/usr/bin/env python3
"""vgm2s98 - Convert VGM files to S98 V3 format.

Converts the OPNA/OPN-compatible portion of a VGM log into an S98 file
playable on real PC-98 hardware (or any S98 player).

Supported VGM chips:
  YM2608 (0x56/0x57)      -> S98 OPNA device (normal/extend), 1:1
  YM2203 (0x55)           -> S98 OPN device, 1:1
  AY8910/YM2149 (0xA0)    -> S98 PSG device

YM2608 DELTA-T data blocks (type 0x81) are converted into an ADPCM RAM
upload sequence (memory-write mode, then one register write per byte),
the same thing PC-98 sound drivers do at load time. Playback on real
hardware therefore requires a board with DELTA-T RAM. The address unit
of the START/STOP/LIMIT registers depends on the RAM type (x8-bit or
ROM: 32-byte units; x1-bit: 4-byte units), so the upload uses the same
RAM type the song itself selects via register $01.

Everything else (YM2612, SN76489, DAC streams, other data blocks, ...)
is skipped; a summary of skipped commands is printed.

Timing: VGM waits are counted in 44100 Hz samples. They are converted to
the S98 sync unit (default 1/1000 s) with running-total quantization, so
the error never accumulates: at any point the emitted sync count equals
floor(elapsed_samples * sync_rate / 44100).

The VGM loop offset is mapped to the S98 loop point. GD3 tags are
converted to an [S98] tag (UTF-8 with BOM) appended at the end of file,
as recommended by the S98 V3 spec.
"""

import argparse
import gzip
import struct
import sys

VGM_SAMPLE_RATE = 44100

# S98 device type ids
S98_OPN = 2
S98_OPNA = 4
S98_PSG_YM2149 = 1
S98_PSG_AY8910 = 15

# Default chip clocks used when the VGM header field is absent/zero
DEFAULT_CLOCKS = {"ym2608": 7987200, "ym2203": 3993600, "ay8910": 1789750}


class VgmError(Exception):
    pass


def read_vgm(path):
    """Read a .vgm or .vgz (gzip) file."""
    with open(path, "rb") as f:
        head = f.read(2)
        f.seek(0)
        if head == b"\x1f\x8b":
            with gzip.open(f) as g:
                return g.read()
        return f.read()


def u32(data, ofs):
    return struct.unpack_from("<L", data, ofs)[0]


def parse_header(data):
    if data[0:4] != b"Vgm ":
        raise VgmError("not a VGM file (bad magic)")
    version = u32(data, 0x08)

    if version >= 0x150 and u32(data, 0x34) != 0:
        data_ofs = 0x34 + u32(data, 0x34)
    else:
        data_ofs = 0x40

    loop_ofs = u32(data, 0x1C)
    loop_abs = 0x1C + loop_ofs if loop_ofs else None
    gd3_ofs = u32(data, 0x14)
    gd3_abs = 0x14 + gd3_ofs if gd3_ofs else None

    def clock(field_ofs):
        # Header fields only exist if the data offset leaves room for them
        if field_ofs + 4 <= data_ofs and field_ofs + 4 <= len(data):
            return u32(data, field_ofs) & 0x3FFFFFFF  # mask dual-chip flags
        return 0

    ay_type = data[0x78] if 0x79 <= data_ofs and len(data) > 0x78 else 0

    return {
        "version": version,
        "data_ofs": data_ofs,
        "loop_abs": loop_abs,
        "gd3_abs": gd3_abs,
        "total_samples": u32(data, 0x18),
        "clock_ym2608": clock(0x48),
        "clock_ym2203": clock(0x44),
        "clock_ay8910": clock(0x74),
        "ay_type": ay_type,
    }


def parse_gd3(data, ofs):
    """Return the GD3 tag as a list of 11 strings, or None."""
    if ofs is None or data[ofs:ofs + 4] != b"Gd3 ":
        return None
    length = u32(data, ofs + 8)
    payload = data[ofs + 12:ofs + 12 + length]
    fields = payload.decode("utf-16-le", errors="replace").split("\x00")
    return (fields + [""] * 11)[:11]


# Byte length (including the command byte) of VGM commands we skip.
# Documented reserved ranges are included so unknown-chip files still parse.
def skip_len(cmd):
    if 0x30 <= cmd <= 0x3F:
        return 2
    if 0x40 <= cmd <= 0x4E:
        return 3
    if cmd in (0x4F, 0x50):
        return 2
    if 0x51 <= cmd <= 0x5F:
        return 3
    if cmd == 0x64:
        return 4
    if 0x70 <= cmd <= 0x8F:
        return 1        # 0x7n/0x8n also carry a wait, handled by caller
    if cmd == 0x90 or cmd == 0x91:
        return 5
    if cmd == 0x92:
        return 6
    if cmd == 0x93:
        return 11
    if cmd == 0x94:
        return 2
    if cmd == 0x95:
        return 5
    if 0xA0 <= cmd <= 0xBF:
        return 3
    if 0xC0 <= cmd <= 0xDF:
        return 4
    if 0xE0 <= cmd <= 0xFF:
        return 5
    return None


def parse_commands(data, hdr):
    """Walk the VGM command stream.

    Returns (events, used_chips, skipped) where events is a list of
    ('write', chip, port, addr, value) / ('wait', samples) / ('loop',) /
    ('block', start_addr, data) for YM2608 DELTA-T memory images.
    """
    events = []
    used = set()
    skipped = {}
    pos = hdr["data_ofs"]
    loop_abs = hdr["loop_abs"]
    end = len(data)

    def skip_count(name):
        skipped[name] = skipped.get(name, 0) + 1

    while pos < end:
        if loop_abs is not None and pos == loop_abs:
            events.append(("loop",))
            loop_abs = None
        cmd = data[pos]

        if cmd == 0x66:                     # end of sound data
            break
        elif cmd == 0x55:                   # YM2203
            events.append(("write", "ym2203", 0, data[pos + 1], data[pos + 2]))
            used.add("ym2203")
            pos += 3
        elif cmd == 0x56 or cmd == 0x57:    # YM2608 port 0/1
            events.append(("write", "ym2608", cmd - 0x56,
                           data[pos + 1], data[pos + 2]))
            used.add("ym2608")
            pos += 3
        elif cmd == 0xA0:                   # AY8910 (bit7 of addr = 2nd chip)
            addr = data[pos + 1]
            if addr & 0x80:
                skip_count("AY8910 #2")
            else:
                events.append(("write", "ay8910", 0, addr, data[pos + 2]))
                used.add("ay8910")
            pos += 3
        elif cmd == 0x61:                   # wait nnnn samples
            events.append(("wait", struct.unpack_from("<H", data, pos + 1)[0]))
            pos += 3
        elif cmd == 0x62:                   # wait 1/60 s
            events.append(("wait", 735))
            pos += 1
        elif cmd == 0x63:                   # wait 1/50 s
            events.append(("wait", 882))
            pos += 1
        elif 0x70 <= cmd <= 0x7F:           # wait n+1 samples
            events.append(("wait", (cmd & 0x0F) + 1))
            pos += 1
        elif 0x80 <= cmd <= 0x8F:           # YM2612 DAC write + wait n
            skip_count("YM2612 DAC")
            if cmd & 0x0F:
                events.append(("wait", cmd & 0x0F))
            pos += 1
        elif cmd == 0x67:                   # data block
            if pos + 7 > end or data[pos + 1] != 0x66:
                raise VgmError(f"broken data block at 0x{pos:x}")
            btype = data[pos + 2]
            size = u32(data, pos + 3) & 0x7FFFFFFF
            if btype == 0x81 and size > 8:  # YM2608 DELTA-T memory image
                # payload: dword memory size, dword start address, data
                start = u32(data, pos + 7 + 4)
                events.append(("block", start,
                               data[pos + 7 + 8:pos + 7 + size]))
            else:
                skip_count(f"data block type 0x{btype:02x}")
            pos += 7 + size
        elif cmd == 0x68:                   # PCM RAM write
            skip_count("PCM RAM write")
            pos += 12
        else:
            n = skip_len(cmd)
            if n is None:
                raise VgmError(f"unknown VGM command 0x{cmd:02x} at 0x{pos:x}")
            skip_count(f"cmd 0x{cmd:02x}")
            pos += n
    else:
        print("warning: no end-of-data (0x66) command", file=sys.stderr)

    return events, used, skipped


def encode_syncs(n):
    """Encode n syncs as S98 FF / FE+varint commands."""
    if n <= 0:
        return b""
    if n == 1:
        return b"\xff"
    v = n - 2                               # spec: getvv() returns n + 2
    out = bytearray(b"\xfe")
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            break
    return bytes(out)


def build_devices(used, hdr):
    """Return an ordered list of (chip, s98_type, clock)."""
    devices = []
    if "ym2608" in used:
        devices.append(("ym2608", S98_OPNA,
                        hdr["clock_ym2608"] or DEFAULT_CLOCKS["ym2608"]))
    if "ym2203" in used:
        devices.append(("ym2203", S98_OPN,
                        hdr["clock_ym2203"] or DEFAULT_CLOCKS["ym2203"]))
    if "ay8910" in used:
        ay_type = S98_PSG_YM2149 if hdr["ay_type"] == 0x10 else S98_PSG_AY8910
        devices.append(("ay8910", ay_type,
                        hdr["clock_ay8910"] or DEFAULT_CLOCKS["ay8910"]))
    return devices


def build_tag(gd3):
    """Build an [S98] tag (UTF-8 with BOM) from GD3 fields."""
    if gd3 is None:
        return b""
    (track_en, track_jp, game_en, game_jp, system_en, system_jp,
     author_en, author_jp, date, ripper, _notes) = gd3

    def pick(jp, en):
        return jp or en

    pairs = [("title", pick(track_jp, track_en)),
             ("game", pick(game_jp, game_en)),
             ("artist", pick(author_jp, author_en)),
             ("system", pick(system_jp, system_en)),
             ("year", date),
             ("s98by", ripper),
             ("comment", "converted by vgm2s98")]
    body = "".join(f"{k}={v}\x0a" for k, v in pairs if v)
    return b"[S98]\xef\xbb\xbf" + body.encode("utf-8") + b"\x00"


def convert(data, sync_num, sync_den, adpcm=True):
    hdr = parse_header(data)
    events, used, skipped = parse_commands(data, hdr)
    if not used:
        raise VgmError("no supported chip commands (YM2608/YM2203/AY8910)")

    devices = build_devices(used, hdr)
    dev_index = {chip: i for i, (chip, _, _) in enumerate(devices)}

    # RAM type for DELTA-T uploads: follow the song's own control2 ($01)
    # writes so the START/STOP address unit matches (x8-bit/ROM: 32 bytes,
    # x1-bit: 4 bytes). Default to x8-bit DRAM.
    ram_type = 0x02
    for ev in events:
        if ev[0] == "write" and ev[1] == "ym2608" and ev[2] == 1 \
                and ev[3] == 0x01:
            ram_type = ev[4] & 0x03
            break

    dump = bytearray()
    loop_pos = None
    total_samples = 0
    emitted_syncs = 0
    extra_syncs = 0
    adpcm_bytes = 0

    def upload_block(start, blob):
        """Emit a DELTA-T RAM upload as extend-port register writes.

        One sync is inserted every 256 data bytes so a real player's
        interrupt handler is not blocked for the whole upload. The
        inserted syncs extend the timeline (extra_syncs) instead of
        being absorbed by the following waits: shortening later waits
        would bake the shortened timing into the loop region, making
        every loop pass play those waits too fast.
        """
        nonlocal emitted_syncs, extra_syncs
        ext = dev_index["ym2608"] * 2 + 1
        shift = 5 if ram_type & 0x03 else 2

        def w(addr, value):
            dump.append(ext)
            dump.append(addr)
            dump.append(value)

        w(0x00, 0x01)                       # reset
        w(0x00, 0x60)                       # memory write mode (REC|MEMDATA)
        w(0x01, ram_type)
        unit = start >> shift
        w(0x02, unit & 0xFF)
        w(0x03, (unit >> 8) & 0xFF)
        w(0x04, 0xFF)                       # STOP = max
        w(0x05, 0xFF)
        w(0x0C, 0xFF)                       # LIMIT = max
        w(0x0D, 0xFF)
        for i, b in enumerate(blob):
            w(0x08, b)
            if (i + 1) % 256 == 0:
                dump.append(0xFF)
                emitted_syncs += 1
                extra_syncs += 1
        w(0x00, 0x00)                       # leave memory mode

    def do_block(ev):
        nonlocal adpcm_bytes
        _, start, blob = ev
        if not adpcm:
            skipped["ADPCM block (disabled)"] = \
                skipped.get("ADPCM block (disabled)", 0) + 1
        elif "ym2608" not in dev_index:
            skipped["ADPCM block (no YM2608)"] = \
                skipped.get("ADPCM block (no YM2608)", 0) + 1
        else:
            upload_block(start, blob)
            adpcm_bytes += len(blob)

    # Hoist data blocks that appear before the first wait to the very
    # beginning, ahead of the loop marker: a VGM that loops from the top
    # would otherwise replay the whole RAM upload on every loop pass.
    body = []
    seen_wait = False
    for ev in events:
        if ev[0] == "block" and not seen_wait:
            do_block(ev)
        else:
            seen_wait = seen_wait or ev[0] == "wait"
            body.append(ev)

    for ev in body:
        kind = ev[0]
        if kind == "write":
            _, chip, port, addr, value = ev
            dump.append(dev_index[chip] * 2 + port)
            dump.append(addr)
            dump.append(value)
        elif kind == "wait":
            total_samples += ev[1]
            target = (total_samples * sync_den
                      // (VGM_SAMPLE_RATE * sync_num)) + extra_syncs
            dump += encode_syncs(target - emitted_syncs)
            emitted_syncs = max(emitted_syncs, target)
        elif kind == "block":                # mid-stream block (rare)
            do_block(ev)
        else:                                # loop marker
            loop_pos = len(dump)
    dump.append(0xFD)

    dump_ofs = 0x20 + 0x10 * len(devices)
    tag = build_tag(parse_gd3(data, hdr["gd3_abs"]))
    tag_ofs = dump_ofs + len(dump) if tag else 0
    loop_abs = dump_ofs + loop_pos if loop_pos is not None else 0

    out = bytearray()
    out += b"S983"
    out += struct.pack("<6L", sync_num, sync_den, 0, tag_ofs, dump_ofs,
                       loop_abs)
    out += struct.pack("<L", len(devices))
    for _, s98_type, clock in devices:
        out += struct.pack("<4L", s98_type, clock, 0, 0)
    out += dump
    out += tag

    stats = {
        "devices": devices,
        "skipped": skipped,
        "seconds": total_samples / VGM_SAMPLE_RATE,
        "syncs": emitted_syncs,
        "loop": loop_pos is not None,
        "size": len(out),
        "adpcm_bytes": adpcm_bytes,
    }
    return bytes(out), stats


def main():
    ap = argparse.ArgumentParser(
        description="Convert VGM (OPNA/OPN/AY portion) to S98 V3")
    ap.add_argument("input", help="input .vgm or .vgz file")
    ap.add_argument("output", nargs="?", help="output .s98 file "
                    "(default: input name with .s98 extension)")
    ap.add_argument("--sync", default="1/1000", metavar="N/D",
                    help="S98 sync unit in seconds (default: 1/1000)")
    ap.add_argument("--no-adpcm", action="store_true",
                    help="drop YM2608 DELTA-T data blocks instead of "
                    "converting them to an ADPCM RAM upload")
    ap.add_argument("-q", "--quiet", action="store_true",
                    help="suppress the conversion summary")
    args = ap.parse_args()

    try:
        num, den = (int(x) for x in args.sync.split("/"))
    except ValueError:
        ap.error("--sync must look like 1/1000")

    output = args.output
    if output is None:
        base = args.input
        for ext in (".vgm", ".vgz", ".VGM", ".VGZ"):
            if base.endswith(ext):
                base = base[:-len(ext)]
                break
        output = base + ".s98"

    try:
        data = read_vgm(args.input)
        s98, stats = convert(data, num, den, adpcm=not args.no_adpcm)
    except (VgmError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    with open(output, "wb") as f:
        f.write(s98)

    if not args.quiet:
        devs = ", ".join(f"{chip}({clock}Hz)"
                         for chip, _, clock in stats["devices"])
        print(f"{output}: {stats['size']} bytes, {stats['seconds']:.1f}s, "
              f"{stats['syncs']} syncs, devices: {devs}, "
              f"loop: {'yes' if stats['loop'] else 'no'}")
        if stats["adpcm_bytes"]:
            print(f"  ADPCM RAM upload: {stats['adpcm_bytes']} bytes "
                  "(requires DELTA-T RAM on real hardware)")
        for name, count in sorted(stats["skipped"].items()):
            print(f"  skipped: {name} x{count}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
