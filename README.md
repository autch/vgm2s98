# vgm2s98

Convert VGM files to S98 V3 format.

The reverse tool (s982vgm) already exists in the wild; this fills the
missing direction. The main use case is playing the OPNA/OPN-compatible
portion of a VGM log on real PC-98 hardware with an [S98 player](https://github.com/autch/s98play-plus).

## Usage

```
python3 vgm2s98.py input.vgm [output.s98] [--sync N/D] [-q]
```

- `.vgz` (gzip-compressed VGM) is read transparently.
- The output defaults to the input name with an `.s98` extension.
- `--sync N/D` sets the S98 sync unit in seconds (default `1/1000`).

## What is converted

| VGM command | S98 output |
|---|---|
| `0x56`/`0x57` YM2608 port 0/1 | OPNA device, normal/extend (1:1) |
| `0x55` YM2203 | OPN device (1:1) |
| `0xA0` AY8910/YM2149 | PSG device (type from the VGM AY chip type) |
| `0x61`-`0x63`, `0x7n` waits | syncs (`FF`/`FE`+varint) |
| `0x67` type `0x81` DELTA-T memory image | ADPCM RAM upload sequence (see below) |
| loop offset | S98 loop point |
| GD3 tag | `[S98]` tag, UTF-8 with BOM, appended at end of file |

Everything else (YM2612, SN76489, DAC streams, other data blocks,
second chips of a dual-chip pair, ...) is skipped and reported in the
conversion summary.

## ADPCM

VGM stores YM2608 ADPCM sample data in data blocks, not as register
writes, so a register-level conversion alone loses it. vgm2s98
synthesizes the same upload a PC-98 sound driver performs at load time:
switch the DELTA-T unit into memory-write mode, set START/STOP/LIMIT,
then write the sample bytes one register write at a time. One sync is
inserted every 256 bytes so a real player's interrupt handler is not
blocked for the whole upload; these syncs extend the timeline (the
music simply starts a moment later) rather than being deducted from
later waits, which would bake a rushed passage into the loop region.
Blocks appearing before the first wait are hoisted ahead of the loop
point, so a song that loops from the top does not replay the upload on
every pass.

The START/STOP/LIMIT address unit depends on the RAM type (x8-bit DRAM
or ROM: 32-byte units, x1-bit DRAM: 4-byte units), so the upload adopts
the RAM type the song itself selects through register $01.

Playing the result on real hardware requires a board whose OPNA has
DELTA-T RAM (e.g. Speak Board); a plain PC-9801-86 has none, and the
ADPCM part will stay silent there. Pass `--no-adpcm` to drop the blocks
and keep the file small.

## Timing conversion

VGM waits are counted in 1/44100 s samples. They are quantized to the
S98 sync unit with a running total, so the emitted sync count always
equals `floor(elapsed_samples * sync_rate / 44100)` — individual events
may shift by up to half a sync unit, but the error never accumulates.

## Limitations

- No pitch correction: register values are copied verbatim, so if the
  source chip clock differs from the target hardware clock (e.g. an
  arcade YM2203 at 3.58 MHz played on a PC-98 at 3.9936 MHz), the pitch
  shifts accordingly. VGM logs of PC-88/PC-98 origin match and need no
  correction.
- YM2612 is not translated to OPNA (planned as a possible future
  extension together with F-Number rescaling).

## C port (`src/`)

A C89 port of `vgm2s98.py` lives under `src/`. With the default host
build the `.s98` dump is byte-identical to the Python output (including
the GD3-derived `[S98]` tag). The design streams the VGM command stream
in two seekable passes and avoids `long long`, so the same sources build
with modern gcc, DJGPP, and 16-bit DOS compilers (LSI C-86 verified).

Conversion has been checked with:

- **gcc / DJGPP** — `src/Makefile` (output matches the Python tool)
- **LSI C-86** — `src/Makefile.lsi` (converts on real PC-98 DOS)

### Host and DJGPP (`src/Makefile`)

GNU make. Works for native Linux/macOS gcc and for DJGPP on DOS.

```
cd src
make                 # default: zlib + GD3 tag conversion
make ZLIB=0 GD3=0    # slim build; same feature set as the 16-bit DOS target
```

### 16-bit DOS / LSI C-86 (`src/Makefile.lsi`)

```
cd src
make -f Makefile.lsi
```

Neither `HAVE_ZLIB` nor `HAVE_GD3` is defined, so the binary stays small
and free of compression and floating-point libraries:

| Macro | Effect when undefined |
|---|---|
| `HAVE_ZLIB` | no `.vgz`; decompress on the host and pass a plain `.vgm` |
| `HAVE_GD3` | no GD3 → `[S98]` tag (`tag_ofs` stays 0) |

GD3 → `[S98]` needs UTF-16LE decoding, UTF-8 encoding, and a heap
buffer for the tag. On a DOS-sized binary that cost is not worth it
(UTF-8 tags also sit poorly on a Shift_JIS environment). Stats printing
uses integer seconds only (no `printf` `%f`), so LSI C can link with
**intlib** instead of a floating-point library.

## Tests

```
python3 test/mkvgm.py        # generate synthetic test VGMs
python3 vgm2s98.py test/scale.vgm
```

The output can be inspected and round-trip verified with the [s98ml
toolchain](https://github.com/autch/s98ml) (`s98d` / `s98c`).
