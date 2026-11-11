"""MP4/M4A and CAF seeds, encoded by afconvert and rewritten box by box.
"""

from __future__ import annotations

import os
import struct
from typing import Callable

from .common import Corpus, find_tool, run
from .wav import write_temp_wav


# --------------------------------------------------------------------------
# ISO BMFF (MP4) box rewriting for the m4a variants
# --------------------------------------------------------------------------

MP4_CONTAINERS = {b"moov", b"trak", b"mdia", b"minf", b"stbl", b"udta", b"edts"}


def mp4_boxes(data: bytes) -> list[tuple[int, int, bytes, int]]:
    """(offset, size, type, header_len) of the boxes in a byte range."""
    boxes = []
    pos = 0
    while pos + 8 <= len(data):
        size, kind = struct.unpack(">I4s", data[pos:pos + 8])
        header = 8
        if size == 1:
            size = struct.unpack(">Q", data[pos + 8:pos + 16])[0]
            header = 16
        elif size == 0:
            size = len(data) - pos
        if size < header:
            raise ValueError("bad box size")
        boxes.append((pos, size, kind, header))
        pos += size
    return boxes


def mp4_rewrite(data: bytes, leaf: Callable[[bytes, bytes], bytes]) -> bytes:
    """Rebuild a box tree, letting `leaf(kind, payload)` replace leaf payloads."""
    out = bytearray()
    for offset, size, kind, header in mp4_boxes(data):
        payload = data[offset + header:offset + size]
        if kind in MP4_CONTAINERS:
            payload = mp4_rewrite(payload, leaf)
        else:
            payload = leaf(kind, payload)
        out += struct.pack(">I4s", 8 + len(payload), kind) + payload
    return bytes(out)


def mp4_zero_times(data: bytes) -> bytes:
    """Zero creation/modification times so afconvert output is reproducible."""

    def leaf(kind: bytes, payload: bytes) -> bytes:
        if kind in (b"mvhd", b"tkhd", b"mdhd"):
            version = payload[0]
            width = 8 if version == 1 else 4
            return payload[:4] + bytes(2 * width) + payload[4 + 2 * width:]
        return payload

    return mp4_rewrite(data, leaf)


def mp4_shift_chunk_offsets(data: bytes, delta: int, convert_to_co64: bool = False) -> bytes:
    def leaf(kind: bytes, payload: bytes) -> bytes:
        if kind in (b"stco", b"co64"):
            count = struct.unpack(">I", payload[4:8])[0]
            width = 8 if kind == b"co64" else 4
            fmt = ">Q" if kind == b"co64" else ">I"
            entries = [struct.unpack(fmt, payload[8 + i * width:8 + (i + 1) * width])[0] + delta
                       for i in range(count)]
            if convert_to_co64:
                return payload[:8] + b"".join(struct.pack(">Q", e) for e in entries)
            return payload[:8] + b"".join(struct.pack(fmt, e) for e in entries)
        return payload

    def rename(kind: bytes) -> bytes:
        return b"co64" if convert_to_co64 and kind == b"stco" else kind

    out = bytearray()
    for offset, size, kind, header in mp4_boxes(data):
        payload = data[offset + header:offset + size]
        if kind in MP4_CONTAINERS:
            payload = mp4_shift_chunk_offsets(payload, delta, convert_to_co64)
        else:
            payload = leaf(kind, payload)
        out += struct.pack(">I4s", 8 + len(payload), rename(kind)) + payload
    return bytes(out)


def mp4_layout(data: bytes) -> dict[bytes, tuple[int, int]]:
    return {kind: (offset, size) for offset, size, kind, _ in mp4_boxes(data)}


def mp4_mdat_size1(data: bytes) -> bytes:
    """Rewrite the top-level mdat to use size==1 with a 64-bit largesize."""
    layout = mp4_layout(data)
    mdat_offset, mdat_size = layout[b"mdat"]
    moov_offset, _ = layout[b"moov"]
    out = (data[:mdat_offset] + struct.pack(">I4sQ", 1, b"mdat", mdat_size + 8)
           + data[mdat_offset + 8:])
    # Sample data moved by 8 bytes; chunk offsets must follow it.
    new_layout = mp4_layout(out)
    moov_offset, moov_size = new_layout[b"moov"]
    moov = out[moov_offset:moov_offset + moov_size]
    moov = mp4_shift_chunk_offsets(moov, 8)
    return out[:moov_offset] + moov + out[moov_offset + moov_size:]


def mp4_co64(data: bytes) -> bytes:
    """Convert every stco table to co64, adjusting offsets if moov precedes mdat."""
    layout = mp4_layout(data)
    moov_offset, moov_size = layout[b"moov"]
    mdat_offset, _ = layout[b"mdat"]
    moov = data[moov_offset:moov_offset + moov_size]
    grown = mp4_shift_chunk_offsets(moov, 0, convert_to_co64=True)
    delta = len(grown) - len(moov) if moov_offset < mdat_offset else 0
    if delta:
        grown = mp4_shift_chunk_offsets(moov, delta, convert_to_co64=True)
    return data[:moov_offset] + grown + data[moov_offset + moov_size:]


def build_coreaudio_seeds(corpus: Corpus, tmp: str) -> None:
    afconvert = find_tool("afconvert")
    names = ("m4a-aac-lc-mono.m4a", "m4a-alac-stereo.m4a", "m4a-aac-mdat-size1.m4a", "m4a-aac-co64.m4a",
             "caf-pcm16-mono.caf")
    if not afconvert:
        for name in names:
            corpus.skip(name, "afconvert not found (macOS only)")
        return
    corpus.tools_used["afconvert"] = afconvert
    mono_src = os.path.join(tmp, "ca-mono.wav")
    stereo_src = os.path.join(tmp, "ca-stereo.wav")
    write_temp_wav(mono_src, 32000, 0.2, 1)
    write_temp_wav(stereo_src, 8000, 0.1, 2)

    aac_path = os.path.join(tmp, "aac.m4a")
    run([afconvert, "-f", "m4af", "-d", "aac", "-b", "32000", "-s", "0", mono_src, aac_path])
    with open(aac_path, "rb") as f:
        aac = corpus.add("m4a-aac-lc-mono.m4a", mp4_zero_times(f.read()))
    alac_path = os.path.join(tmp, "alac.m4a")
    run([afconvert, "-f", "m4af", "-d", "alac", stereo_src, alac_path])
    with open(alac_path, "rb") as f:
        corpus.add("m4a-alac-stereo.m4a", mp4_zero_times(f.read()))
    corpus.add("m4a-aac-mdat-size1.m4a", mp4_mdat_size1(aac))
    corpus.add("m4a-aac-co64.m4a", mp4_co64(aac))

    caf_path = os.path.join(tmp, "pcm.caf")
    run([afconvert, "-f", "caff", "-d", "LEI16", mono_src, caf_path])
    with open(caf_path, "rb") as f:
        corpus.add("caf-pcm16-mono.caf", f.read())
