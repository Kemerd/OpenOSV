#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The OpenOSV Contributors
"""
gen_golden.py - independent golden generator for the osv_meta test-suite.

This script deliberately shares NO code with the C++ library.  It contains a
minimal ISO BMFF box walker, a sample-table resolver and a proto3 wire decoder
written from scratch in pure Python (stdlib only; numpy is optional and only
used for a couple of sanity statistics).  The goldens it writes are what the
C++ decoder (DjmdDecoder / MetadataTrack) must reproduce:

    tests/golden/sample_probe.json         ClipMeta + StreamMeta (all populated
                                           dewarp slots) + container/format
                                           summary decoded from djmd sample 0
    tests/golden/sample_frames_0_1_64.json FrameMeta of frames 0, 1 and 64 with
                                           per-frame quaternion, IMU batch size,
                                           first/last/anchor IMU quaternion and
                                           timestamps

Usage:
    python scripts/gen_golden.py [path/to/clip.OSV] [--out tests/golden]

Console output is plain 7-bit ASCII so it is safe on any Windows code page.
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
from typing import Any, Dict, Iterator, List, Optional, Tuple

# -----------------------------------------------------------------------------
#  Small helpers
# -----------------------------------------------------------------------------


def be32(buf: bytes, pos: int) -> int:
    """Big-endian unsigned 32-bit at pos (raises on truncation)."""
    return struct.unpack_from(">I", buf, pos)[0]


def be64(buf: bytes, pos: int) -> int:
    """Big-endian unsigned 64-bit at pos."""
    return struct.unpack_from(">Q", buf, pos)[0]


def ascii_tag(raw: bytes) -> str:
    """Render a four character code with non-ASCII bytes replaced by '?'."""
    return "".join(chr(b) if 0x20 <= b < 0x7F else "?" for b in raw)


# -----------------------------------------------------------------------------
#  ISO BMFF box walker
# -----------------------------------------------------------------------------

# Boxes whose payload is a list of child boxes (after an optional header).
CONTAINER_BOXES = {
    "moov", "trak", "mdia", "minf", "stbl", "dinf", "udta", "edts", "gmhd",
}
# Full boxes that contain children after a 4 byte version/flags header.
FULLBOX_CONTAINERS = {"meta"}


def iter_boxes(buf: bytes, start: int, end: int) -> Iterator[Tuple[str, int, int, int]]:
    """Yield (type, box_start, payload_start, box_end) for the boxes in [start, end).

    Handles size==1 (64-bit largesize) and size==0 (extends to end).  Stops on
    the first box whose header does not fit or whose size runs past `end`.
    """
    pos = start
    while pos + 8 <= end:
        size = be32(buf, pos)
        kind = ascii_tag(buf[pos + 4:pos + 8])
        header = 8
        if size == 1:
            if pos + 16 > end:
                return
            size = be64(buf, pos + 8)
            header = 16
        elif size == 0:
            size = end - pos
        if size < header or pos + size > end:
            return
        yield kind, pos, pos + header, pos + size
        pos += size


def find_children(buf: bytes, payload_start: int, box_end: int) -> List[Tuple[str, int, int, int]]:
    """All direct children of a container box."""
    return list(iter_boxes(buf, payload_start, box_end))


def child(children: List[Tuple[str, int, int, int]], kind: str) -> Optional[Tuple[str, int, int, int]]:
    """First child with the given type, or None."""
    for c in children:
        if c[0] == kind:
            return c
    return None


# -----------------------------------------------------------------------------
#  Track / sample table parsing
# -----------------------------------------------------------------------------


class Track:
    """Everything we need from one 'trak' box."""

    def __init__(self) -> None:
        self.track_id = 0
        self.flags = 0
        self.width = 0
        self.height = 0
        self.timescale = 0
        self.duration = 0
        self.handler = ""
        self.entry_fourcc = ""
        self.sample_sizes: List[int] = []
        self.chunk_offsets: List[int] = []
        self.stsc: List[Tuple[int, int, int]] = []  # (first_chunk, samples_per_chunk, desc)
        self.stts: List[Tuple[int, int]] = []
        self.stss: List[int] = []
        self.sample_offsets: List[int] = []

    def resolve_samples(self) -> None:
        """Turn stsz/stsc/stco into an absolute file offset per sample."""
        offsets: List[int] = []
        n_samples = len(self.sample_sizes)
        n_chunks = len(self.chunk_offsets)
        sample_index = 0
        for i, (first_chunk, per_chunk, _desc) in enumerate(self.stsc):
            # Chunk range covered by this stsc entry (1-based first_chunk).
            last_chunk = self.stsc[i + 1][0] - 1 if i + 1 < len(self.stsc) else n_chunks
            for chunk in range(first_chunk, last_chunk + 1):
                if chunk - 1 >= n_chunks:
                    break
                pos = self.chunk_offsets[chunk - 1]
                for _ in range(per_chunk):
                    if sample_index >= n_samples:
                        break
                    offsets.append(pos)
                    pos += self.sample_sizes[sample_index]
                    sample_index += 1
        self.sample_offsets = offsets

    def sample(self, buf: bytes, index: int) -> bytes:
        """Raw bytes of sample `index`."""
        off = self.sample_offsets[index]
        return buf[off:off + self.sample_sizes[index]]


def parse_track(buf: bytes, trak: Tuple[str, int, int, int]) -> Track:
    """Parse the parts of a 'trak' box we care about."""
    t = Track()
    kids = find_children(buf, trak[2], trak[3])
    tkhd = child(kids, "tkhd")
    if tkhd:
        p = tkhd[2]
        version = buf[p]
        t.flags = be32(buf, p) & 0xFFFFFF
        if version == 1:
            t.track_id = be32(buf, p + 4 + 8 + 8)
            body = p + 4 + 8 + 8 + 4 + 4 + 8
        else:
            t.track_id = be32(buf, p + 4 + 4 + 4)
            body = p + 4 + 4 + 4 + 4 + 4 + 4
        # body: 8 reserved, layer(2), alt(2), volume(2), reserved(2), matrix(36), w(4), h(4)
        t.width = be32(buf, body + 8 + 2 + 2 + 2 + 2 + 36) >> 16
        t.height = be32(buf, body + 8 + 2 + 2 + 2 + 2 + 36 + 4) >> 16
    mdia = child(kids, "mdia")
    if not mdia:
        return t
    mkids = find_children(buf, mdia[2], mdia[3])
    mdhd = child(mkids, "mdhd")
    if mdhd:
        p = mdhd[2]
        version = buf[p]
        if version == 1:
            t.timescale = be32(buf, p + 4 + 8 + 8)
            t.duration = be64(buf, p + 4 + 8 + 8 + 4)
        else:
            t.timescale = be32(buf, p + 4 + 4 + 4)
            t.duration = be32(buf, p + 4 + 4 + 4 + 4)
    hdlr = child(mkids, "hdlr")
    if hdlr:
        t.handler = ascii_tag(buf[hdlr[2] + 8:hdlr[2] + 12])
    minf = child(mkids, "minf")
    if not minf:
        return t
    stbl = child(find_children(buf, minf[2], minf[3]), "stbl")
    if not stbl:
        return t
    skids = find_children(buf, stbl[2], stbl[3])
    stsd = child(skids, "stsd")
    if stsd:
        p = stsd[2] + 8  # version/flags + entry_count
        if p + 8 <= stsd[3]:
            t.entry_fourcc = ascii_tag(buf[p + 4:p + 8])
    stsz = child(skids, "stsz")
    if stsz:
        p = stsz[2] + 4
        default_size = be32(buf, p)
        count = be32(buf, p + 4)
        if default_size:
            t.sample_sizes = [default_size] * count
        else:
            t.sample_sizes = [be32(buf, p + 8 + 4 * i) for i in range(count)]
    stsc = child(skids, "stsc")
    if stsc:
        p = stsc[2] + 4
        count = be32(buf, p)
        t.stsc = [(be32(buf, p + 4 + 12 * i), be32(buf, p + 8 + 12 * i), be32(buf, p + 12 + 12 * i)) for i in range(count)]
    stco = child(skids, "stco")
    co64 = child(skids, "co64")
    if stco:
        p = stco[2] + 4
        count = be32(buf, p)
        t.chunk_offsets = [be32(buf, p + 4 + 4 * i) for i in range(count)]
    elif co64:
        p = co64[2] + 4
        count = be32(buf, p)
        t.chunk_offsets = [be64(buf, p + 4 + 8 * i) for i in range(count)]
    stts = child(skids, "stts")
    if stts:
        p = stts[2] + 4
        count = be32(buf, p)
        t.stts = [(be32(buf, p + 4 + 8 * i), be32(buf, p + 8 + 8 * i)) for i in range(count)]
    stss = child(skids, "stss")
    if stss:
        p = stss[2] + 4
        count = be32(buf, p)
        t.stss = [be32(buf, p + 4 + 4 * i) for i in range(count)]
    t.resolve_samples()
    return t


def parse_movie(buf: bytes, base: int, end: int) -> Dict[str, Any]:
    """Parse the top level boxes of a movie in buf[base:end]."""
    info: Dict[str, Any] = {"boxes": [], "tracks": [], "timescale": 0, "duration": 0, "index_table": []}
    for kind, bstart, pstart, bend in iter_boxes(buf, base, end):
        info["boxes"].append({"type": kind, "offset": bstart - base, "size": bend - bstart})
        if kind == "moov":
            for ck, cs, cp, ce in iter_boxes(buf, pstart, bend):
                if ck == "mvhd":
                    version = buf[cp]
                    if version == 1:
                        info["timescale"] = be32(buf, cp + 4 + 8 + 8)
                        info["duration"] = be64(buf, cp + 4 + 8 + 8 + 4)
                    else:
                        info["timescale"] = be32(buf, cp + 4 + 4 + 4)
                        info["duration"] = be32(buf, cp + 4 + 4 + 4 + 4)
                elif ck == "trak":
                    info["tracks"].append(parse_track(buf, (ck, cs, cp, ce)))
        elif kind == "free" and bstart - base < 64 and not info["index_table"]:
            # DJI index table: 16 byte records tag | u64 offset | u32 size,
            # stored in a 'free' box right after ftyp (offset 36 in .OSV
            # files, 40 in .LRF files).  Only a payload whose first record
            # has a printable tag and an in-file offset qualifies; the other
            # early 'free' box is plain padding.
            p = pstart
            entries: List[Dict[str, Any]] = []
            while p + 16 <= bend:
                tag = buf[p:p + 4]
                if tag == b"\0\0\0\0":
                    break
                if not all(0x20 <= b < 0x7F for b in tag):
                    entries = []
                    break
                offset = be64(buf, p + 4)
                size = be32(buf, p + 12)
                if offset + size > end - base:
                    entries = []
                    break
                entries.append({"tag": ascii_tag(tag), "offset": offset, "size": size})
                p += 16
            info["index_table"] = entries
    return info


# -----------------------------------------------------------------------------
#  proto3 wire decoder (independent of the C++ ProtoScanner)
# -----------------------------------------------------------------------------

WIRE_VARINT, WIRE_FIXED64, WIRE_LEN, WIRE_SGROUP, WIRE_EGROUP, WIRE_FIXED32 = 0, 1, 2, 3, 4, 5


def read_varint(buf: bytes, pos: int, end: int) -> Tuple[int, int]:
    """Decode a base-128 varint; returns (value, new_pos).  Raises on overrun."""
    value = 0
    shift = 0
    for _ in range(10):
        if pos >= end:
            raise ValueError("varint overrun")
        b = buf[pos]
        pos += 1
        value |= (b & 0x7F) << shift
        shift += 7
        if not (b & 0x80):
            return value, pos
    raise ValueError("varint too long")


def scan_fields(buf: bytes, start: int = 0, end: Optional[int] = None) -> List[Tuple[int, int, Any]]:
    """Return [(field_number, wire_type, value)] for one message.

    value is int for varint/fixed32/fixed64 (raw bits) and bytes for
    length-delimited.  Groups are skipped.  Decoding stops (partial result)
    at the first malformed element.
    """
    if end is None:
        end = len(buf)
    out: List[Tuple[int, int, Any]] = []
    pos = start
    depth = 0
    try:
        while pos < end:
            tag, pos = read_varint(buf, pos, end)
            field = tag >> 3
            wire = tag & 7
            if field == 0:
                break
            if wire == WIRE_VARINT:
                v, pos = read_varint(buf, pos, end)
                val: Any = v
            elif wire == WIRE_FIXED64:
                if pos + 8 > end:
                    break
                val = struct.unpack_from("<Q", buf, pos)[0]
                pos += 8
            elif wire == WIRE_LEN:
                ln, pos = read_varint(buf, pos, end)
                if pos + ln > end:
                    break
                val = buf[pos:pos + ln]
                pos += ln
            elif wire == WIRE_FIXED32:
                if pos + 4 > end:
                    break
                val = struct.unpack_from("<I", buf, pos)[0]
                pos += 4
            elif wire == WIRE_SGROUP:
                depth += 1
                continue
            elif wire == WIRE_EGROUP:
                depth -= 1
                continue
            else:
                break
            if depth == 0:
                out.append((field, wire, val))
    except ValueError:
        pass
    return out


def f32(bits: int) -> float:
    """fixed32 bits -> IEEE-754 float (as Python float)."""
    return struct.unpack("<f", struct.pack("<I", bits & 0xFFFFFFFF))[0]


def i32(v: int) -> int:
    """varint -> int32 (two's complement wrap)."""
    v &= 0xFFFFFFFFFFFFFFFF
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


def rep_float(fields: List[Tuple[int, int, Any]], number: int) -> List[float]:
    """Repeated float (packed or unpacked)."""
    out: List[float] = []
    for f, w, v in fields:
        if f != number:
            continue
        if w == WIRE_FIXED32:
            out.append(f32(v))
        elif w == WIRE_LEN and isinstance(v, (bytes, bytearray)) and len(v) % 4 == 0:
            out.extend(struct.unpack("<%df" % (len(v) // 4), v))
    return out


def rep_varint(fields: List[Tuple[int, int, Any]], number: int, signed: bool = False) -> List[int]:
    """Repeated int32/uint32 (packed or unpacked)."""
    out: List[int] = []
    for f, w, v in fields:
        if f != number:
            continue
        if w == WIRE_VARINT:
            out.append(i32(v) if signed else v)
        elif w == WIRE_LEN:
            p = 0
            while p < len(v):
                x, p = read_varint(v, p, len(v))
                out.append(i32(x) if signed else x)
    return out


def first(fields: List[Tuple[int, int, Any]], number: int, wire: Optional[int] = None) -> Any:
    """Last occurrence of a scalar field (proto3 semantics), or None."""
    result = None
    for f, w, v in fields:
        if f == number and (wire is None or w == wire):
            result = v
    return result


def sub(fields: List[Tuple[int, int, Any]], number: int) -> Optional[List[Tuple[int, int, Any]]]:
    """Decode the last occurrence of an embedded message field, or None."""
    raw = first(fields, number, WIRE_LEN)
    if raw is None:
        return None
    return scan_fields(raw)


def subs(fields: List[Tuple[int, int, Any]], number: int) -> List[List[Tuple[int, int, Any]]]:
    """Decode every occurrence of a repeated embedded message field."""
    return [scan_fields(v) for f, w, v in fields if f == number and w == WIRE_LEN]


def scalar_float(fields: List[Tuple[int, int, Any]], number: int, default: float = 0.0) -> float:
    v = first(fields, number, WIRE_FIXED32)
    return f32(v) if v is not None else default


def scalar_u32(fields: List[Tuple[int, int, Any]], number: int, default: int = 0) -> int:
    v = first(fields, number, WIRE_VARINT)
    return (v & 0xFFFFFFFF) if v is not None else default


def scalar_i32(fields: List[Tuple[int, int, Any]], number: int, default: int = 0) -> int:
    v = first(fields, number, WIRE_VARINT)
    return i32(v) if v is not None else default


def scalar_u64(fields: List[Tuple[int, int, Any]], number: int, default: int = 0) -> int:
    v = first(fields, number, WIRE_VARINT)
    return v if v is not None else default


def scalar_dimension(fields: List[Tuple[int, int, Any]], number: int, default: int = 0) -> int:
    """Pixel count that the camera writes either as a fixed32 float
    (DewarpParams.width/height are 3840.0f on the Osmo 360) or as a varint.
    Non-finite / negative / absurd values collapse to 0 like the C++ side."""
    # proto3 semantics: the last occurrence of the field wins.
    for f, w, raw in reversed(fields):
        if f != number:
            continue
        if w == WIRE_FIXED32:
            value = f32(raw)
            if value != value or value < 0.0 or value > 65535.0:
                return 0
            return int(round(value))
        if w == WIRE_VARINT:
            return raw & 0xFFFFFFFF
        return default
    return default


def scalar_str(fields: List[Tuple[int, int, Any]], number: int) -> str:
    v = first(fields, number, WIRE_LEN)
    return v.decode("utf-8", "replace") if v is not None else ""


def wrapped_float(fields: List[Tuple[int, int, Any]], number: int) -> float:
    """Message {1 float} wrapper."""
    m = sub(fields, number)
    return scalar_float(m, 1) if m is not None else 0.0


def wrapped_u32(fields: List[Tuple[int, int, Any]], number: int) -> int:
    m = sub(fields, number)
    return scalar_u32(m, 1) if m is not None else 0


def wrapped_i32(fields: List[Tuple[int, int, Any]], number: int) -> int:
    m = sub(fields, number)
    return scalar_i32(m, 1) if m is not None else 0


def wrapped_u64(fields: List[Tuple[int, int, Any]], number: int) -> int:
    m = sub(fields, number)
    return scalar_u64(m, 1) if m is not None else 0


def quaternion(fields: Optional[List[Tuple[int, int, Any]]]) -> Optional[Dict[str, float]]:
    """message Quaternion {1 w, 2 x, 3 y, 4 z}."""
    if fields is None:
        return None
    return {"w": scalar_float(fields, 1), "x": scalar_float(fields, 2), "y": scalar_float(fields, 3), "z": scalar_float(fields, 4)}


# -----------------------------------------------------------------------------
#  Typed schema decoding
# -----------------------------------------------------------------------------

EIS_NAMES = {0: "Off", 1: "RockSteady", 2: "HorizonSteady", 3: "Hyper", 4: "Tradeoff", 5: "HorizonBalancing",
             6: "DeepSpace", 7: "OffWithCrop", 8: "HorizonCorrection", 9: "RsAuto"}
COLOR_NAMES = {0: "Normal", 1: "DCinelike", 2: "DLog", 9: "HLG", 12: "Vivid", 19: "DLogM", 22: "DLog2"}
LENS_MODE_NAMES = {0: "Native", 1: "LensGuards", 2: "Underwater"}
SLOT_NAMES = [
    "unknown",
    "native_refine_slave", "native_refine_master",
    "native_refine_far_slave", "native_refine_far_master",
    "lens_guards_slave", "lens_guards_master",
    "water_above_slave", "water_above_master",
    "water_under_slave", "water_under_master",
    "native_slave", "native_master",
    "far_07_slave", "far_07_master",
    "far_09_slave", "far_09_master",
    "far_11_slave", "far_11_master",
    "far_12_5_slave", "far_12_5_master",
    "far_14_slave", "far_14_master",
    "far_16_slave", "far_16_master",
]


def decode_clip(fields: List[Tuple[int, int, Any]]) -> Dict[str, Any]:
    """ClipMeta -> JSON-ready dict."""
    hdr = sub(fields, 1) or []
    streams = sub(fields, 2) or []
    dist = sub(fields, 3) or []
    res = sub(fields, 14) or []
    fnum = sub(fields, 15) or []
    eis = wrapped_i32(fields, 9)
    present = sorted({f for f, _, _ in fields})
    return {
        "header": {
            "protoFileName": scalar_str(hdr, 1),
            "libVersion": scalar_str(hdr, 2),
            "productProtoVersion": scalar_str(hdr, 3),
            "serialNumber": scalar_str(hdr, 5),
            "firmware": scalar_str(hdr, 6),
            "clipTimestampUs": scalar_u64(hdr, 9),
            "productName": scalar_str(hdr, 10),
        },
        "videoStreamCount": scalar_u32(streams, 1),
        "audioStreamCount": scalar_u32(streams, 2),
        "distortionCoefficients": rep_float(dist, 1),
        "sensorReadoutTime": wrapped_u64(fields, 4),
        "sensorReadDirection": wrapped_i32(fields, 5),
        "digitalFocalLength": wrapped_float(fields, 8),
        "eisStatus": eis,
        "eisStatusName": EIS_NAMES.get(eis, "Unknown"),
        "imuSamplingRate": wrapped_u32(fields, 10),
        "sensorFps": wrapped_float(fields, 11),
        "itd": wrapped_i32(fields, 12),
        "lro": wrapped_u32(fields, 13),
        "sensorW": scalar_u32(res, 1),
        "sensorH": scalar_u32(res, 2),
        "fNumber": rep_varint(fnum, 1),
        "styleFilterMode": wrapped_u32(fields, 16),
        "yltmEnable": wrapped_u32(fields, 17),
        "presentFields": present,
    }


def decode_dewarp(fields: List[Tuple[int, int, Any]]) -> Dict[str, Any]:
    """DewarpParams -> JSON-ready dict (None when the slot is all zeros)."""
    k = [0.0] * 9
    for i, num in enumerate([5, 6, 7, 8, 15, 16, 17, 18, 19]):
        k[i] = scalar_float(fields, num)
    d = {
        "fx": scalar_float(fields, 1),
        "fy": scalar_float(fields, 2),
        "cx": scalar_float(fields, 3),
        "cy": scalar_float(fields, 4),
        "k": k,
        "xi": scalar_float(fields, 9),
        "width": scalar_dimension(fields, 10),
        "height": scalar_dimension(fields, 11),
        "yaw": scalar_float(fields, 12),
        "pitch": scalar_float(fields, 13),
        "roll": scalar_float(fields, 14),
        "p": rep_float(fields, 20),
        "q": rep_float(fields, 21),
        "occlusionPtX": rep_float(fields, 22),
        "occlusionPtY": rep_float(fields, 23),
        "lensModel": scalar_float(fields, 24),
        "temperature": scalar_float(fields, 25),
        "camImuExtriQ": quaternion(sub(fields, 26)),
        "tangentCoeff": rep_float(fields, 27),
        "camExtriQ": quaternion(sub(fields, 28)),
        "camImuCaliEnable": bool(scalar_u32(fields, 29)),
        "tempCompenEnable": bool(scalar_u32(fields, 30)),
        "tempCompenK": scalar_float(fields, 31),
        "gimbalYawH1": scalar_float(fields, 32),
        "gimbalYawH2": scalar_float(fields, 33),
        "tempCompenKOrder": rep_float(fields, 35),
        "presentFields": sorted({f for f, _, _ in fields}),
    }
    return d


def dewarp_is_empty(d: Dict[str, Any]) -> bool:
    """True when every numeric member is zero (an unused slot)."""
    numeric = [d["fx"], d["fy"], d["cx"], d["cy"], d["xi"], d["width"], d["height"], d["yaw"], d["pitch"], d["roll"],
               d["lensModel"], d["temperature"], d["tempCompenK"], d["gimbalYawH1"], d["gimbalYawH2"]] + d["k"]
    numeric += d["p"] + d["q"] + d["occlusionPtX"] + d["occlusionPtY"] + d["tangentCoeff"] + d["tempCompenKOrder"]
    for q in (d["camImuExtriQ"], d["camExtriQ"]):
        if q is not None:
            numeric += [q["w"], q["x"], q["y"], q["z"]]
    return all(v == 0 for v in numeric)


def decode_stream(fields: List[Tuple[int, int, Any]]) -> Dict[str, Any]:
    """StreamMeta -> JSON-ready dict with all 24 dewarp slots."""
    hdr = sub(fields, 1) or []
    vid = sub(fields, 3) or []
    color = wrapped_i32(fields, 4) if sub(fields, 4) is not None else -1
    lens_mode = wrapped_i32(fields, 7)
    dewarp: Dict[str, Any] = {}
    pano = sub(fields, 6) or []
    for slot in range(1, 25):
        rec = sub(pano, slot)
        if rec is None:
            dewarp[SLOT_NAMES[slot]] = None
            continue
        d = decode_dewarp(rec)
        dewarp[SLOT_NAMES[slot]] = None if dewarp_is_empty(d) else d
    return {
        "id": scalar_u32(hdr, 1),
        "type": scalar_i32(hdr, 2),
        "name": scalar_str(hdr, 3),
        "video": {
            "width": scalar_u32(vid, 1),
            "height": scalar_u32(vid, 2),
            "fps": scalar_float(vid, 3),
            "bitDepthValid": bool(scalar_u32(vid, 4)),
            "bitDepth": scalar_u32(vid, 5),
            "bitFormat": scalar_i32(vid, 6),
            "streamType": scalar_i32(vid, 7),
            "codec": scalar_i32(vid, 8),
        },
        "colorMode": color,
        "colorModeName": COLOR_NAMES.get(color, "Unknown"),
        "fovType": wrapped_i32(fields, 5),
        "extriLensMode": lens_mode,
        "extriLensModeName": LENS_MODE_NAMES.get(lens_mode, "Unknown"),
        "shadingCalibModeNum": wrapped_u32(fields, 8),
        "dewarp": dewarp,
        "presentFields": sorted({f for f, _, _ in fields}),
    }


def decode_batch(fields: Optional[List[Tuple[int, int, Any]]]) -> Optional[Dict[str, Any]]:
    """DeviceAttitude {1 ts, 2 vsync, 3 repeated Quaternion, 4 offset}."""
    if fields is None:
        return None
    qs = [quaternion(q) for q in subs(fields, 3)]
    return {
        "ts": scalar_u32(fields, 1),
        "vsync": scalar_u32(fields, 2),
        "offset": scalar_float(fields, 4),
        "count": len(qs),
        "first": qs[0] if qs else None,
        "last": qs[-1] if qs else None,
        "anchor4": qs[4] if len(qs) > 4 else None,
        "q": qs,
    }


def decode_frame(fields: List[Tuple[int, int, Any]]) -> Dict[str, Any]:
    """FrameMeta -> JSON-ready dict."""
    hdr = sub(fields, 1) or []
    cam = sub(fields, 2) or []
    imu = sub(fields, 3)
    gimbal = sub(fields, 4) or []
    gimbal_dev = sub(gimbal, 1) or []
    acc = sub(cam, 10)
    aec = sub(cam, 15) or []
    exposure = sub(cam, 4) or []
    eqf = sub(cam, 17) or []
    frame: Dict[str, Any] = {
        "seq": scalar_u64(hdr, 1),
        "timestampUs": scalar_u64(hdr, 2),
        "streamId": scalar_u32(hdr, 3),
        "camera": {
            "exposureIndex": wrapped_float(cam, 2),
            "iso": wrapped_float(cam, 3),
            "exposureTime": rep_varint(exposure, 1, signed=True),
            "digitalZoom": wrapped_float(cam, 5),
            "wbCct": wrapped_u32(cam, 6),
            "orientation": wrapped_i32(cam, 7),
            "attitude": quaternion(sub(cam, 9)),
            "acc": None if acc is None else {"x": scalar_float(acc, 2), "y": scalar_float(acc, 3), "z": scalar_float(acc, 4)},
            "sharpness": wrapped_float(cam, 11),
            "denoising": wrapped_float(cam, 12),
            "aecLv": scalar_float(aec, 1),
            "aecLuxIdx": scalar_float(aec, 2),
            "aecAdrcGain": scalar_float(aec, 6),
            "sensorTemperature": wrapped_float(cam, 16),
            "eqFocal": rep_varint(eqf, 1, signed=True),
        },
        "imu": None,
        "gimbalDeviceName": scalar_str(gimbal_dev, 4),
        "gimbalDeviceFrequency": scalar_float(gimbal_dev, 5),
    }
    if imu is not None:
        fusion = sub(imu, 2) or []
        frame["imu"] = {
            "current": decode_batch(sub(fusion, 1)),
            "prev": decode_batch(sub(fusion, 2)),
            "next": decode_batch(sub(fusion, 3)),
            "vsyncPos": wrapped_u32(imu, 3),
        }
    return frame


def decode_product(sample: bytes) -> Dict[str, Any]:
    """ProductMeta {1 clip, 2 stream, 3 frame}."""
    fields = scan_fields(sample)
    clip = sub(fields, 1)
    stream = sub(fields, 2)
    frame = sub(fields, 3)
    return {
        "clip": decode_clip(clip) if clip is not None else None,
        "stream": decode_stream(stream) if stream is not None else None,
        "frame": decode_frame(frame) if frame is not None else None,
    }


# -----------------------------------------------------------------------------
#  Format summary (mirrors what FormatDetector must conclude)
# -----------------------------------------------------------------------------


def format_summary(movie: Dict[str, Any], clip: Dict[str, Any], stream: Dict[str, Any], meta_track_id: int,
                   side_by_side: bool) -> Dict[str, Any]:
    """Derive the coarse format description from the decoded metadata."""
    video = [t for t in movie["tracks"] if t.handler == "vide"]
    video.sort(key=lambda t: t.track_id)
    # Frame rate from the container sample table (timescale / stts delta),
    # exactly like FormatDetector; the metadata's rounded 59.94 is the fallback.
    container_fps = 0.0
    if video and video[0].timescale > 0 and video[0].stts:
        delta = video[0].stts[0][1]
        if delta > 0:
            container_fps = video[0].timescale / float(delta)
    w = stream["video"]["width"]
    if side_by_side:
        mode = "Lrf"
    elif w == 1920:
        mode = "K4"
    elif w == 3000:
        mode = "K6"
    elif w == 3840:
        mode = "K8"
    else:
        mode = "Unknown"
    return {
        "cameraModel": clip["header"]["productName"],
        "mode": mode,
        "streamW": stream["video"]["width"],
        "streamH": stream["video"]["height"],
        "fps": container_fps if container_fps > 0 else stream["video"]["fps"],
        "colorMode": stream["colorMode"],
        "colorModeName": stream["colorModeName"],
        "colorModeFromMetadata": stream["colorMode"] != -1,
        "lensMode": stream["extriLensMode"],
        "bitDepth": stream["video"]["bitDepth"],
        "dualFisheye": len(video) == 2 or side_by_side,
        "videoTrackIds": [t.track_id for t in video][:2],
        "metaTrackId": meta_track_id,
        "sensorW": clip["sensorW"],
        "sensorH": clip["sensorH"],
        "digitalFocalLength": clip["digitalFocalLength"],
        "sideBySideProxy": side_by_side,
    }


# -----------------------------------------------------------------------------
#  Main
# -----------------------------------------------------------------------------


def track_json(t: Track) -> Dict[str, Any]:
    """Container summary row for one track."""
    return {
        "id": t.track_id,
        "flags": t.flags,
        "handler": t.handler,
        "entry": t.entry_fourcc,
        "width": t.width,
        "height": t.height,
        "timescale": t.timescale,
        "duration": t.duration,
        "sampleCount": len(t.sample_sizes),
        "syncSamples": t.stss,
        "firstSampleSize": t.sample_sizes[0] if t.sample_sizes else 0,
    }


def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser(description="Generate osv_meta golden JSON from an .OSV clip")
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument("clip", nargs="?", default=os.path.join(root, "CAM_20260904090647_0010_D.OSV"))
    ap.add_argument("--out", default=os.path.join(root, "tests", "golden"))
    ap.add_argument("--frames", default="0,1,64", help="comma separated frame indices for the frames golden")
    args = ap.parse_args(argv)

    if not os.path.isfile(args.clip):
        print("ERROR: clip not found: %s" % args.clip)
        return 2
    with open(args.clip, "rb") as fh:
        buf = fh.read()
    print("read %d bytes from %s" % (len(buf), os.path.basename(args.clip)))

    movie = parse_movie(buf, 0, len(buf))
    print("top-level boxes: %s" % ", ".join(b["type"] for b in movie["boxes"]))
    print("tracks: %d" % len(movie["tracks"]))
    for t in movie["tracks"]:
        print("  track %d handler=%s entry=%s %dx%d samples=%d flags=0x%x" %
              (t.track_id, t.handler, t.entry_fourcc, t.width, t.height, len(t.sample_sizes), t.flags))

    # Pick the metadata track: first djmd track whose sample 0 carries a clip,
    # a stream and at least one populated calibration slot.
    djmd_tracks = [t for t in movie["tracks"] if t.entry_fourcc == "djmd"]
    if not djmd_tracks:
        print("ERROR: no djmd track")
        return 2
    chosen: Optional[Track] = None
    chosen_product: Optional[Dict[str, Any]] = None
    for t in djmd_tracks:
        if not t.sample_sizes:
            continue
        product = decode_product(t.sample(buf, 0))
        has_calib = product["stream"] is not None and any(v is not None for v in product["stream"]["dewarp"].values())
        print("  djmd track %d: clip=%s stream=%s calibration=%s" %
              (t.track_id, product["clip"] is not None, product["stream"] is not None, has_calib))
        if chosen is None and product["clip"] is not None and product["stream"] is not None and has_calib:
            chosen = t
            chosen_product = product
    if chosen is None or chosen_product is None:
        print("ERROR: no djmd track with calibration")
        return 2
    print("metadata track: %d (%d samples)" % (chosen.track_id, len(chosen.sample_sizes)))

    # camd nested movie summary (from the index table entry).
    camd: Dict[str, Any] = {"present": False}
    for e in movie["index_table"]:
        if e["tag"] == "camd" and e["offset"] + e["size"] <= len(buf):
            nested = parse_movie(buf, e["offset"], e["offset"] + e["size"])
            camd = {
                "present": True,
                "offset": e["offset"],
                "size": e["size"],
                "tracks": [track_json(t) for t in nested["tracks"]],
            }
            print("camd nested movie: %d tracks, samples=%s" %
                  (len(nested["tracks"]), [len(t.sample_sizes) for t in nested["tracks"]]))

    # Proxy detection: a single video track twice as wide as high.
    video = [t for t in movie["tracks"] if t.handler == "vide"]
    side_by_side = len(video) == 1 and video[0].width == 2 * video[0].height
    fmt = format_summary(movie, chosen_product["clip"], chosen_product["stream"], chosen.track_id, side_by_side)

    probe = {
        "file": os.path.basename(args.clip),
        "generator": "scripts/gen_golden.py",
        "container": {
            "timescale": movie["timescale"],
            "duration": movie["duration"],
            "boxes": movie["boxes"],
            "tracks": [track_json(t) for t in movie["tracks"]],
            "indexTable": movie["index_table"],
            "camd": camd,
        },
        "format": fmt,
        "metaTrackId": chosen.track_id,
        "frameCount": len(chosen.sample_sizes),
        "clip": chosen_product["clip"],
        "stream": chosen_product["stream"],
    }

    # Per frame golden for the requested indices plus a few whole-track
    # invariants that the C++ tests re-check on every frame.
    indices = [int(x) for x in args.frames.split(",") if x.strip()]
    frames: Dict[str, Any] = {"file": os.path.basename(args.clip), "metaTrackId": chosen.track_id, "frames": {}}
    prev_ts = -1
    seq_steps = set()
    prev_seq = None
    batch_sizes = set()
    attitude_matches_anchor = True
    for i in range(len(chosen.sample_sizes)):
        p = decode_product(chosen.sample(buf, i))
        f = p["frame"]
        if f is None:
            print("WARNING: frame %d has no FrameMeta" % i)
            continue
        if f["timestampUs"] <= prev_ts:
            print("WARNING: timestamp not increasing at frame %d" % i)
        prev_ts = f["timestampUs"]
        if prev_seq is not None:
            seq_steps.add(f["seq"] - prev_seq)
        prev_seq = f["seq"]
        if f["imu"] and f["imu"]["current"]:
            batch_sizes.add(f["imu"]["current"]["count"])
            a = f["camera"]["attitude"]
            anchor = f["imu"]["current"]["anchor4"]
            if a is None or anchor is None or any(abs(a[c] - anchor[c]) > 1e-6 for c in "wxyz"):
                attitude_matches_anchor = False
        if i in indices:
            frames["frames"][str(i)] = f
    frames["invariants"] = {
        "seqSteps": sorted(seq_steps),
        "imuBatchSizes": sorted(batch_sizes),
        "attitudeEqualsImuAnchor4": attitude_matches_anchor,
        "frameCount": len(chosen.sample_sizes),
    }
    print("frames: %d, seq steps %s, imu batch sizes %s, attitude==imu[4]: %s" %
          (len(chosen.sample_sizes), sorted(seq_steps), sorted(batch_sizes), attitude_matches_anchor))

    os.makedirs(args.out, exist_ok=True)
    probe_path = os.path.join(args.out, "sample_probe.json")
    frames_path = os.path.join(args.out, "sample_frames_0_1_64.json")
    with open(probe_path, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(probe, fh, indent=2, ensure_ascii=True)
        fh.write("\n")
    with open(frames_path, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(frames, fh, indent=2, ensure_ascii=True)
        fh.write("\n")
    print("wrote %s" % probe_path)
    print("wrote %s" % frames_path)

    # A few headline values so a human can eyeball the run.
    clip = chosen_product["clip"]
    stream = chosen_product["stream"]
    print("clip: %s fw=%s sn=%s dfl=%.4f" % (clip["header"]["productName"], clip["header"]["firmware"],
                                              clip["header"]["serialNumber"], clip["digitalFocalLength"]))
    print("stream: %dx%d @ %.3f fps color=%s lensMode=%s" % (stream["video"]["width"], stream["video"]["height"],
                                                             stream["video"]["fps"], stream["colorModeName"],
                                                             stream["extriLensModeName"]))
    populated = [k for k, v in stream["dewarp"].items() if v is not None]
    print("populated dewarp slots: %s" % ", ".join(populated))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
