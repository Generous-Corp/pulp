"""Ogg Vorbis seeds: ffmpeg-encoded bases and page-level corruptions.
"""

from __future__ import annotations

import os
import struct

from .common import Corpus, find_tool, run
from .wav import write_temp_wav


# --------------------------------------------------------------------------
# Ogg page walking and CRC
# --------------------------------------------------------------------------


def _ogg_crc_table() -> list[int]:
    table = []
    for i in range(256):
        r = i << 24
        for _ in range(8):
            r = ((r << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if r & 0x80000000 else (r << 1) & 0xFFFFFFFF
        table.append(r)
    return table


OGG_CRC_TABLE = _ogg_crc_table()


def ogg_crc(data: bytes) -> int:
    crc = 0
    for b in data:
        crc = ((crc << 8) & 0xFFFFFFFF) ^ OGG_CRC_TABLE[((crc >> 24) & 0xFF) ^ b]
    return crc


def ogg_pages(data: bytes) -> list[tuple[int, int]]:
    """(offset, length) of every page in a well-formed Ogg stream."""
    pages = []
    pos = 0
    while pos + 27 <= len(data):
        if data[pos:pos + 4] != b"OggS":
            raise ValueError(f"lost Ogg sync at {pos}")
        nsegs = data[pos + 26]
        body = sum(data[pos + 27:pos + 27 + nsegs])
        length = 27 + nsegs + body
        pages.append((pos, length))
        pos += length
    return pages


def ogg_rewrite_serial(data: bytes, serial: int) -> bytes:
    out = bytearray(data)
    for offset, length in ogg_pages(data):
        out[offset + 14:offset + 18] = struct.pack("<I", serial)
        out[offset + 22:offset + 26] = b"\0\0\0\0"
        crc = ogg_crc(bytes(out[offset:offset + length]))
        out[offset + 22:offset + 26] = struct.pack("<I", crc)
    return bytes(out)


def build_ogg_seeds(corpus: Corpus, tmp: str) -> None:
    ffmpeg = find_tool("ffmpeg")
    names = ("ogg-vorbis-mono.ogg", "ogg-chained-two-streams.ogg", "ogg-truncated-page.ogg", "ogg-page-bad-crc.ogg")
    if not ffmpeg:
        for name in names:
            corpus.skip(name, "ffmpeg not found")
        return
    corpus.tools_used["ffmpeg"] = ffmpeg
    rate = 22050
    src_a = os.path.join(tmp, "ogg-a.wav")
    src_b = os.path.join(tmp, "ogg-b.wav")
    write_temp_wav(src_a, rate, 0.2, 1)
    write_temp_wav(src_b, rate, 0.15, 2)

    def encode(src: str, dst: str) -> bytes:
        run([ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-i", src, "-fflags", "+bitexact",
             "-flags:a", "+bitexact", "-map_metadata", "-1", "-c:a", "libvorbis", "-q:a", "0", "-f", "ogg", dst])
        with open(dst, "rb") as f:
            return f.read()

    first = corpus.add("ogg-vorbis-mono.ogg", encode(src_a, os.path.join(tmp, "a.ogg")))
    second = encode(src_b, os.path.join(tmp, "b.ogg"))
    # Chained streams need distinct serial numbers; rewrite the second and fix its page CRCs.
    corpus.add("ogg-chained-two-streams.ogg", first + ogg_rewrite_serial(second, 0x5EED0002))
    pages = ogg_pages(first)
    last_offset, last_length = pages[-1]
    corpus.add("ogg-truncated-page.ogg", first[:last_offset + 27 + first[last_offset + 26] + last_length // 3])
    # Flip one CRC byte on the first audio page (after the three Vorbis header pages).
    audio_page = pages[min(3, len(pages) - 1)][0]
    corrupted = bytearray(first)
    corrupted[audio_page + 23] ^= 0xA5
    corpus.add("ogg-page-bad-crc.ogg", bytes(corrupted))
