#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The OpenOSV Contributors
"""
Generate the small synthetic ISO BMFF fixtures used by tests/unit/test_container.cpp.

Only the Python standard library is used, output is ASCII, and every file is
written in binary mode so the script behaves identically on Windows and
POSIX.  Run from anywhere:

    python scripts/make_fixtures.py [--out tests/fixtures]

Fixtures (each well under 50 KB):

  fx_largesize.mp4      ftyp + mdat(largesize) + moov(largesize) with one
                        'avc1' video track (avcC, colr nclx 9/18/9, pasp),
                        edts/elst, stss, ctts, stsc with two runs, variable
                        stsz and a co64 chunk table; udta with DJI style
                        dbpm/dbcm/btec/fsid, meta/ilst (covr + (c)too) and an
                        'Xtra' box; a trailing 'free' box with size == 0.
  fx_moov_size0.mp4     Same track, but 32-bit sizes, an stco table and the
                        'moov' box is the LAST box with size == 0.
  fx_camd.mp4           ftyp + 'free' index table + mdat + moov with one
                        'djmd' metadata track (constant sample size) + a
                        trailing 'camd' box whose payload is a complete
                        nested MP4 with its own 'djmd' track.  The index table
                        has verified 'covr' and 'camd' entries plus one bogus
                        entry that must fail verification.
  fx_truncated_moov.mp4 fx_largesize.mp4 cut inside its 'stsz' box so the
                        only track cannot be recovered (parse -> Truncated).

The expected values asserted by the C++ tests are documented in the
constants below; keep the two in sync when changing anything.
"""

import argparse
import io
import os
import struct
import sys

# -----------------------------------------------------------------------------
#  Box helpers
# -----------------------------------------------------------------------------


def box(kind, payload=b"", large=False, size_zero=False):
    """Plain box.  `large` uses the 64-bit largesize encoding (size field
    1), `size_zero` writes 0 (extends to the end of the parent)."""
    if not isinstance(kind, bytes) or len(kind) != 4:
        raise ValueError("box type must be 4 bytes")
    if large and size_zero:
        raise ValueError("a box cannot be both largesize and size 0")
    if large:
        return struct.pack(">I4sQ", 1, kind, 16 + len(payload)) + payload
    if size_zero:
        return struct.pack(">I4s", 0, kind) + payload
    return struct.pack(">I4s", 8 + len(payload), kind) + payload


def full(kind, version, flags, payload=b"", large=False, size_zero=False):
    """FullBox: version(1) + flags(3) precede the payload."""
    head = struct.pack(">B", version) + struct.pack(">I", flags)[1:]
    return box(kind, head + payload, large=large, size_zero=size_zero)


def u16(v):
    return struct.pack(">H", v)


def u32(v):
    return struct.pack(">I", v)


def u64(v):
    return struct.pack(">Q", v)


def i32(v):
    return struct.pack(">i", v)


def fixed1616(v):
    return u32(int(round(v * 65536)))


def unity_matrix():
    return u32(0x00010000) + u32(0) + u32(0) + u32(0) + u32(0x00010000) + u32(0) + u32(0) + u32(0) + u32(0x40000000)


# -----------------------------------------------------------------------------
#  Fixture parameters shared with test_container.cpp
# -----------------------------------------------------------------------------

VIDEO_SAMPLE_SIZES = [10, 20, 30, 40, 50]          # five samples
VIDEO_CHUNK_SPLIT = 2                              # chunk 1 = samples 0..1, chunk 2 = 2..4
VIDEO_CHUNK_GAP = 7                                # junk bytes between the two chunks
VIDEO_STTS = [(3, 100), (2, 200)]                  # count, delta -> total 700
VIDEO_TIMESCALE = 1000
VIDEO_STSS = [1, 4]                                # 1-based sync samples (0 and 3 zero-based)
VIDEO_CTTS = [(3, 0), (2, 100)]                    # count, offset
VIDEO_WIDTH = 64
VIDEO_HEIGHT = 32
VIDEO_ELST_MEDIA_TIME = 100
MOVIE_TIMESCALE = 1000
MOVIE_DURATION = 700
TOOL_STRING = b"OpenOSV fixture"
BTEC_STRING = b"beauty_enable=0;smoother=0"
FSID_STRING = b"/fixtures/CAM_0001.OSV"
DBPM_VALUE = 21
DBCM_VALUE = 3
XTRA_CATEGORY = "pb_file:fixture.proto;model_name:FX001"

# A tiny "JPEG": SOI + APP0 + EOI, enough for the FF D8 check.
COVER_JPEG = b"\xFF\xD8\xFF\xE0" + u16(16) + b"JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00" + b"\xFF\xD9"

# avcC with one SPS and one PPS (not a decodable stream, just structure).
AVCC_SPS = b"\x67\x42\x00\x1E\xAB\x40\x40\x0F"
AVCC_PPS = b"\x68\xCE\x38\x80"

DJMD_SAMPLE_SIZE = 8                               # constant size in fx_camd.mp4
DJMD_SAMPLE_COUNT = 4
DJMD_TIMESCALE = 60000
DJMD_DELTA = 1001
CAMD_SAMPLE_SIZES = [5, 6, 7]                      # nested movie samples


def sample_bytes(index, size):
    """Sample `index` is filled with the byte (index + 1) so tests can check
    that the right bytes come back."""
    return bytes([index + 1]) * size


# -----------------------------------------------------------------------------
#  Building blocks
# -----------------------------------------------------------------------------


def ftyp(major=b"isom", minor=0x200, brands=(b"isom", b"iso2", b"mp41")):
    return box(b"ftyp", major + u32(minor) + b"".join(brands))


def mvhd(timescale, duration, next_track):
    payload = u32(0) + u32(0) + u32(timescale) + u32(duration)
    payload += fixed1616(1.0) + u16(0x0100) + b"\x00" * 10 + unity_matrix() + b"\x00" * 24 + u32(next_track)
    return full(b"mvhd", 0, 0, payload)


def tkhd(track_id, duration, width, height, flags=3):
    payload = u32(0) + u32(0) + u32(track_id) + u32(0) + u32(duration)
    payload += b"\x00" * 8 + u16(0) + u16(0) + u16(0) + u16(0) + unity_matrix()
    payload += fixed1616(width) + fixed1616(height)
    return full(b"tkhd", 0, flags, payload)


def mdhd(timescale, duration):
    # language 'und' = 0x55C4
    payload = u32(0) + u32(0) + u32(timescale) + u32(duration) + u16(0x55C4) + u16(0)
    return full(b"mdhd", 0, 0, payload)


def hdlr(handler, name):
    return full(b"hdlr", 0, 0, u32(0) + handler + b"\x00" * 12 + name + b"\x00")


def dinf():
    url = full(b"url ", 0, 1)
    dref = full(b"dref", 0, 0, u32(1) + url)
    return box(b"dinf", dref)


def stts(runs):
    return full(b"stts", 0, 0, u32(len(runs)) + b"".join(u32(c) + u32(d) for c, d in runs))


def stss(samples):
    return full(b"stss", 0, 0, u32(len(samples)) + b"".join(u32(s) for s in samples))


def ctts(runs):
    return full(b"ctts", 0, 0, u32(len(runs)) + b"".join(u32(c) + u32(o) for c, o in runs))


def stsc(runs):
    return full(b"stsc", 0, 0, u32(len(runs)) + b"".join(u32(f) + u32(n) + u32(d) for f, n, d in runs))


def stsz(sizes=None, constant=0, count=None):
    if constant:
        return full(b"stsz", 0, 0, u32(constant) + u32(count))
    return full(b"stsz", 0, 0, u32(0) + u32(len(sizes)) + b"".join(u32(s) for s in sizes))


def stco(offsets):
    return full(b"stco", 0, 0, u32(len(offsets)) + b"".join(u32(o) for o in offsets))


def co64(offsets):
    return full(b"co64", 0, 0, u32(len(offsets)) + b"".join(u64(o) for o in offsets))


def avc1_entry(width, height):
    """VisualSampleEntry 'avc1' with avcC, colr (nclx 9/18/9) and pasp."""
    body = b"\x00" * 6 + u16(1)                        # reserved + data_reference_index
    body += u16(0) + u16(0) + b"\x00" * 12             # pre_defined, reserved, pre_defined[3]
    body += u16(width) + u16(height)
    body += u32(0x00480000) + u32(0x00480000)          # 72 dpi
    body += u32(0) + u16(1)                            # reserved, frame_count
    name = b"OpenOSV"
    body += bytes([len(name)]) + name + b"\x00" * (31 - len(name))
    body += u16(0x0018) + u16(0xFFFF)                  # depth, pre_defined = -1
    avcc = b"\x01" + b"\x42\x00\x1E" + b"\xFF" + b"\xE1" + u16(len(AVCC_SPS)) + AVCC_SPS + b"\x01" + u16(len(AVCC_PPS)) + AVCC_PPS
    body += box(b"avcC", avcc)
    body += box(b"colr", b"nclx" + u16(9) + u16(18) + u16(9) + b"\x00")
    body += box(b"pasp", u32(1) + u32(1))
    return box(b"avc1", body)


def djmd_entry():
    """DJI's 20-byte 'djmd' sample entry: header + 6 reserved + dref index."""
    return box(b"djmd", b"\x00" * 6 + u16(1))


def video_trak(chunk_offsets, use_co64):
    total = sum(c * d for c, d in VIDEO_STTS)
    stbl = full(b"stsd", 0, 0, u32(1) + avc1_entry(VIDEO_WIDTH, VIDEO_HEIGHT))
    stbl += stts(VIDEO_STTS) + stss(VIDEO_STSS) + ctts(VIDEO_CTTS)
    stbl += stsc([(1, VIDEO_CHUNK_SPLIT, 1), (2, len(VIDEO_SAMPLE_SIZES) - VIDEO_CHUNK_SPLIT, 1)])
    stbl += stsz(VIDEO_SAMPLE_SIZES)
    stbl += co64(chunk_offsets) if use_co64 else stco(chunk_offsets)
    minf = full(b"vmhd", 0, 1, u16(0) + u16(0) * 3) + dinf() + box(b"stbl", stbl)
    mdia = mdhd(VIDEO_TIMESCALE, total) + hdlr(b"vide", b"FixtureVideo") + box(b"minf", minf)
    elst = full(b"elst", 0, 0, u32(1) + u32(MOVIE_DURATION) + i32(VIDEO_ELST_MEDIA_TIME) + u16(1) + u16(0))
    trak = tkhd(1, MOVIE_DURATION, VIDEO_WIDTH, VIDEO_HEIGHT) + box(b"edts", elst) + box(b"mdia", mdia)
    return box(b"trak", trak)


def ilst_item(name, data_type, value):
    data = box(b"data", u32(data_type) + u32(0) + value)
    return box(name, data)


def xtra_box():
    text = XTRA_CATEGORY.encode("utf-16-le") + b"\x00\x00"
    value = u32(6 + len(text)) + u16(8) + text
    name = b"WM/Category"
    entry = u32(4 + 4 + len(name) + 4 + len(value)) + u32(len(name)) + name + u32(1) + value
    return box(b"Xtra", entry)


def udta(with_ilst=True):
    inner = box(b"\xA9uid", b"\x00\x11\x22\x33")
    inner += box(b"dbpm", u32(DBPM_VALUE)) + box(b"dbcm", u32(DBCM_VALUE))
    inner += box(b"btec", BTEC_STRING) + box(b"fsid", FSID_STRING)
    if with_ilst:
        items = ilst_item(b"\xA9too", 1, TOOL_STRING) + ilst_item(b"covr", 13, COVER_JPEG)
        meta = full(b"meta", 0, 0, hdlr(b"mdir", b"") + box(b"ilst", items))
        inner += meta
    inner += xtra_box()
    return box(b"udta", inner)


def video_mdat_payload():
    """Two chunks of concatenated samples with a junk gap between them.
    Returns (payload, [chunk offsets relative to the payload start])."""
    chunk1 = b"".join(sample_bytes(i, s) for i, s in enumerate(VIDEO_SAMPLE_SIZES[:VIDEO_CHUNK_SPLIT]))
    chunk2 = b"".join(sample_bytes(i, s) for i, s in enumerate(VIDEO_SAMPLE_SIZES) if i >= VIDEO_CHUNK_SPLIT)
    gap = b"\xEE" * VIDEO_CHUNK_GAP
    return chunk1 + gap + chunk2, [0, len(chunk1) + len(gap)]


# -----------------------------------------------------------------------------
#  Fixtures
# -----------------------------------------------------------------------------


def build_largesize():
    """ftyp | mdat(largesize) | moov(largesize) | free(size 0)."""
    head = ftyp()
    payload, rel = video_mdat_payload()
    mdat_offset = len(head)
    data_start = mdat_offset + 16                      # largesize header
    offsets = [data_start + r for r in rel]
    mdat = box(b"mdat", payload, large=True)
    moov_inner = mvhd(MOVIE_TIMESCALE, MOVIE_DURATION, 2) + video_trak(offsets, use_co64=True) + udta()
    moov = box(b"moov", moov_inner, large=True)
    tail = box(b"free", b"\x00" * 16, size_zero=True)
    return head + mdat + moov + tail


def build_moov_size0():
    """ftyp | mdat | moov(size 0, last box)."""
    head = ftyp()
    payload, rel = video_mdat_payload()
    data_start = len(head) + 8
    offsets = [data_start + r for r in rel]
    mdat = box(b"mdat", payload)
    moov_inner = mvhd(MOVIE_TIMESCALE, MOVIE_DURATION, 2) + video_trak(offsets, use_co64=False) + udta(with_ilst=False)
    return head + mdat + box(b"moov", moov_inner, size_zero=True)


def djmd_trak(track_id, sizes, constant, chunk_offset, timescale, delta):
    count = len(sizes) if sizes else constant[1]
    stbl = full(b"stsd", 0, 0, u32(1) + djmd_entry())
    stbl += stts([(count, delta)])
    stbl += stsc([(1, count, 1)])
    stbl += stsz(sizes) if sizes else stsz(constant=constant[0], count=constant[1])
    stbl += stco([chunk_offset])
    gmin = full(b"gmin", 0, 0, u16(0x40) + u16(0x8000) * 3 + u16(0) + u16(0))
    minf = box(b"gmhd", gmin) + dinf() + box(b"stbl", stbl)
    mdia = mdhd(timescale, count * delta) + hdlr(b"meta", b"CAM meta") + box(b"minf", minf)
    return box(b"trak", tkhd(track_id, count * delta, 0, 0, flags=2) + box(b"mdia", mdia))


def build_nested_camd_movie():
    """A complete tiny MP4 (relative offsets) for the 'camd' payload."""
    head = ftyp()
    payload = b"".join(sample_bytes(i, s) for i, s in enumerate(CAMD_SAMPLE_SIZES))
    data_start = len(head) + 8
    mdat = box(b"mdat", payload)
    total = len(CAMD_SAMPLE_SIZES) * DJMD_DELTA
    moov = box(b"moov", mvhd(DJMD_TIMESCALE, total, 2) + djmd_trak(1, CAMD_SAMPLE_SIZES, None, data_start, DJMD_TIMESCALE, DJMD_DELTA))
    return head + mdat + moov


def build_camd():
    """ftyp | free(index table) | mdat | moov(djmd track + udta/covr) | camd."""
    head = ftyp()
    index_payload_size = 64                            # room for 4 entries
    index_box_size = 8 + index_payload_size
    mdat_offset = len(head) + index_box_size
    payload = b"".join(sample_bytes(i, DJMD_SAMPLE_SIZE) for i in range(DJMD_SAMPLE_COUNT))
    mdat = box(b"mdat", payload)
    data_start = mdat_offset + 8
    total = DJMD_SAMPLE_COUNT * DJMD_DELTA
    moov_inner = mvhd(DJMD_TIMESCALE, total, 2)
    moov_inner += djmd_trak(1, None, (DJMD_SAMPLE_SIZE, DJMD_SAMPLE_COUNT), data_start, DJMD_TIMESCALE, DJMD_DELTA)
    udta_box = udta()
    moov_inner += udta_box
    moov = box(b"moov", moov_inner)
    moov_offset = mdat_offset + len(mdat)
    nested = build_nested_camd_movie()
    camd = box(b"camd", nested)
    camd_offset = moov_offset + len(moov)
    camd_payload_offset = camd_offset + 8

    # Locate the 'covr' item box inside moov/udta/meta/ilst for the index entry.
    covr_rel = moov.find(b"covr")
    if covr_rel < 0:
        raise RuntimeError("covr item not found in fixture moov")
    covr_item_offset = moov_offset + covr_rel - 4     # back up over the size field
    entries = b"covr" + u64(covr_item_offset) + u32(len(COVER_JPEG))
    entries += b"camd" + u64(camd_payload_offset) + u32(len(nested))
    bogus_offset = camd_offset + len(camd) + 1000     # deliberately outside the file
    entries += b"bogx" + u64(bogus_offset) + u32(5)
    entries += b"\x00" * (index_payload_size - len(entries))
    index = box(b"free", entries)
    if len(index) != index_box_size:
        raise RuntimeError("index box size mismatch")
    return head + index + mdat + moov + camd


def build_truncated(largesize_bytes):
    """Cut fx_largesize.mp4 inside its stsz box (12 bytes past the type)."""
    pos = largesize_bytes.find(b"stsz")
    if pos < 0:
        raise RuntimeError("stsz not found")
    return largesize_bytes[: pos + 12]


# -----------------------------------------------------------------------------
#  Main
# -----------------------------------------------------------------------------


def write(path, data):
    if len(data) >= 50 * 1024:
        raise RuntimeError("%s is %d bytes, fixtures must stay under 50 KB" % (path, len(data)))
    with io.open(path, "wb") as f:
        f.write(data)
    print("wrote %-24s %6d bytes" % (os.path.basename(path), len(data)))


def main(argv):
    parser = argparse.ArgumentParser(description="Generate OpenOSV container fixtures.")
    default_out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests", "fixtures")
    parser.add_argument("--out", default=default_out, help="output directory (default: tests/fixtures)")
    args = parser.parse_args(argv)
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)

    largesize = build_largesize()
    write(os.path.join(out, "fx_largesize.mp4"), largesize)
    write(os.path.join(out, "fx_moov_size0.mp4"), build_moov_size0())
    write(os.path.join(out, "fx_camd.mp4"), build_camd())
    write(os.path.join(out, "fx_truncated_moov.mp4"), build_truncated(largesize))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
