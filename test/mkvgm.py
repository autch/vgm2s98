#!/usr/bin/env python3
"""Generate synthetic VGM files for testing vgm2s98.

Creates a small YM2608 VGM (SSG scale with a loop and a GD3 tag) plus a
variant exercising skipped commands (YM2612 writes, data blocks, DAC
waits) mixed into the stream.
"""

import gzip
import os
import struct

SAMPLE_RATE = 44100
NOTE_SAMPLES = SAMPLE_RATE // 4          # 250 ms per note

# SSG periods for a C5..C6 scale on a 7.987 MHz OPNA
NOTES = [round(7987200 / 64 / f) for f in
         [523, 587, 659, 698, 784, 880, 988, 1047]]


def ym2608(addr, value):
    return bytes([0x56, addr, value])


def ym2608_p1(addr, value):
    return bytes([0x57, addr, value])


def wait(samples):
    out = b""
    while samples > 65535:
        out += struct.pack("<BH", 0x61, 65535)
        samples -= 65535
    if samples:
        out += struct.pack("<BH", 0x61, samples)
    return out


def ssg_note(period):
    return ym2608(0x00, period & 0xFF) + ym2608(0x01, (period >> 8) & 0x0F)


def deltat_block(start_addr, blob):
    """VGM data block 0x67 type 0x81 (YM2608 DELTA-T memory image)."""
    payload = struct.pack("<LL", start_addr + len(blob), start_addr) + blob
    return bytes([0x67, 0x66, 0x81]) + struct.pack("<L", len(payload)) + payload


def adpcm_play(start_addr, length):
    """Register writes that play a DELTA-T sample from RAM (x8-bit units)."""
    s = start_addr >> 5
    e = (start_addr + length - 1) >> 5
    out = ym2608_p1(0x00, 0x01)             # reset
    out += ym2608_p1(0x01, 0xC2)            # L+R, x8-bit DRAM
    out += ym2608_p1(0x02, s & 0xFF) + ym2608_p1(0x03, (s >> 8) & 0xFF)
    out += ym2608_p1(0x04, e & 0xFF) + ym2608_p1(0x05, (e >> 8) & 0xFF)
    out += ym2608_p1(0x09, 0x00) + ym2608_p1(0x0A, 0x40)  # DELTA-N
    out += ym2608_p1(0x0B, 0xFF)            # volume
    out += ym2608_p1(0x00, 0xA0)            # start, from external memory
    return out


def gd3(title):
    fields = [title, "", "test game", "", "PC-9801", "", "vgm2s98", "",
              "2026", "mkvgm", ""]
    payload = "\x00".join(fields + [""]).encode("utf-16-le")
    return b"Gd3 " + struct.pack("<LL", 0x100, len(payload)) + payload


def build(path, noise=False, adpcm=False, loop_all=False):
    intro = b""
    if adpcm:
        sample = bytes((i * 37) & 0xFF for i in range(1024))
        intro += deltat_block(0x2000, sample)
        intro += adpcm_play(0x2000, len(sample))
    intro += ym2608(0x07, 0x3E) + ym2608(0x08, 0x0F)  # SSG tone A only
    intro += ssg_note(NOTES[0]) + wait(NOTE_SAMPLES)

    loop = b""
    for period in NOTES[1:]:
        loop += ssg_note(period)
        if noise:
            loop += bytes([0x52, 0x28, 0x00])          # YM2612 write (skip)
            loop += bytes([0x67, 0x66, 0x00]) + struct.pack("<L", 4) + b"\xde\xad\xbe\xef"
            loop += bytes([0x84])                      # DAC write + wait 4
            loop += wait(NOTE_SAMPLES - 4)
        else:
            loop += wait(NOTE_SAMPLES)

    data = intro + loop + b"\x66"
    label = "loop test"
    if noise:
        label += " with noise"
    if adpcm:
        label += " with adpcm"
    tag = gd3(label)

    header = bytearray(0x80)
    header[0:4] = b"Vgm "
    struct.pack_into("<L", header, 0x08, 0x00000151)   # version 1.51
    total = SAMPLE_RATE // 4 * 8
    struct.pack_into("<L", header, 0x18, total)        # total samples
    loop_target = 0x80 if loop_all else 0x80 + len(intro)
    struct.pack_into("<L", header, 0x1C, loop_target - 0x1C)  # loop
    struct.pack_into("<L", header, 0x20, total - NOTE_SAMPLES)
    struct.pack_into("<L", header, 0x34, 0x80 - 0x34)  # data offset
    struct.pack_into("<L", header, 0x48, 7987200)      # YM2608 clock
    struct.pack_into("<L", header, 0x14, 0x80 + len(data) - 0x14)  # GD3
    struct.pack_into("<L", header, 0x04, 0x80 + len(data) + len(tag) - 0x04)

    blob = bytes(header) + data + tag
    if path.endswith(".vgz"):
        with open(path, "wb") as f:
            f.write(gzip.compress(blob))
    else:
        with open(path, "wb") as f:
            f.write(blob)
    print(f"{path}: {len(blob)} bytes")


if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    build(os.path.join(here, "scale.vgm"))
    build(os.path.join(here, "scale.vgz"))
    build(os.path.join(here, "noisy.vgm"), noise=True)
    build(os.path.join(here, "adpcm.vgm"), adpcm=True)
    build(os.path.join(here, "adpcmtop.vgm"), adpcm=True, loop_all=True)
