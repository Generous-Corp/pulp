#!/usr/bin/env python3
"""Generate the audio-decoder fuzz seed corpus.

Usage:
    python3 test/fuzz/audio_decoder_seed_corpus.py <out_dir>

Writes one plain container file per seed into <out_dir> (flat, no tag
prefix), named `<format>-<description>.<ext>`. WAV, AIFF, RF64, W64 and the
hand-built FLAC seeds are constructed byte by byte with the Python standard
library and are reproducible by construction. Encoded formats (FLAC via the
reference encoder, MP3 via LAME, Ogg Vorbis via ffmpeg, AAC/ALAC/CAF via
afconvert) shell out to tools when they are present; a missing tool skips
its seeds with a printed notice and never fails the run. Every malformed
seed derives from a valid base built in the same run, so the pre-corruption
bytes are always known.

The run ends with a manifest (name, bytes, sha256) on stdout and a list of
the encoders it used or skipped. The corpus README documents each seed.
"""

from __future__ import annotations

import hashlib
import math
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
from typing import Callable

PRNG_SEED = 0x5EED_C0DE
MAX_SEED_BYTES = 32 * 1024

# Candidate tool locations, first match wins. Absolute paths pin the encoders
# this corpus was generated with; the bare name is a fallback for other hosts.
TOOL_CANDIDATES = {
    "flac": ["/opt/homebrew/bin/flac", "flac"],
    "lame": ["/opt/homebrew/bin/lame", "lame"],
    "ffmpeg": ["/usr/local/bin/ffmpeg", "ffmpeg"],
    "afconvert": ["/usr/bin/afconvert", "afconvert"],
}


def find_tool(name: str) -> str | None:
    for candidate in TOOL_CANDIDATES[name]:
        if os.path.isabs(candidate):
            if os.access(candidate, os.X_OK):
                return candidate
        else:
            found = shutil.which(candidate)
            if found:
                return found
    return None


def run(cmd: list[str], cwd: str | None = None) -> None:
    subprocess.run(cmd, cwd=cwd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


# --------------------------------------------------------------------------
# Deterministic test signal
# --------------------------------------------------------------------------


def make_signal(rate: int, seconds: float, channels: int, amplitude: float = 0.6) -> list[list[float]]:
    """Per-channel float samples in (-1, 1): two sines plus faint seeded noise.

    Channel k gets its own fundamental so stereo seeds are not accidentally
    mono. The noise keeps lossless encoders from collapsing to trivial
    subframes while staying fully reproducible.
    """
    prng = random.Random(PRNG_SEED + rate * 7 + channels)
    frames = int(round(rate * seconds))
    out: list[list[float]] = []
    for ch in range(channels):
        f0 = 220.0 * (1.5 ** ch)
        f1 = f0 * 2.5
        samples = []
        for n in range(frames):
            t = n / rate
            env = min(1.0, n / 64.0, (frames - n) / 64.0)  # short fades, no clicks
            v = amplitude * env * (0.7 * math.sin(2 * math.pi * f0 * t) + 0.3 * math.sin(2 * math.pi * f1 * t))
            v += (prng.random() - 0.5) * 0.002
            samples.append(max(-0.999, min(0.999, v)))
        out.append(samples)
    return out


def quantize(signal: list[list[float]], bits: int) -> list[list[int]]:
    full = (1 << (bits - 1)) - 1
    return [[int(round(v * full)) for v in ch] for ch in signal]


def interleave(channels: list[list[int]]) -> list[int]:
    return [channels[c][i] for i in range(len(channels[0])) for c in range(len(channels))]


def pack_pcm_le(samples: list[int], bits: int, container_bits: int | None = None) -> bytes:
    container_bits = container_bits or bits
    shift = container_bits - bits
    out = bytearray()
    for s in samples:
        if container_bits == 8:
            out.append((s + 128) & 0xFF)
        else:
            v = (s << shift) & ((1 << container_bits) - 1)
            out += v.to_bytes(container_bits // 8, "little")
    return bytes(out)


def pack_pcm_be(samples: list[int], bits: int) -> bytes:
    out = bytearray()
    for s in samples:
        out += (s & ((1 << bits) - 1)).to_bytes(bits // 8, "big")
    return bytes(out)


def pack_float(signal: list[list[float]], fmt: str) -> bytes:
    inter = [signal[c][i] for i in range(len(signal[0])) for c in range(len(signal))]
    return struct.pack("<" + fmt * len(inter), *inter)


# --------------------------------------------------------------------------
# RIFF / WAVE construction
# --------------------------------------------------------------------------

WAVE_FORMAT_PCM = 0x0001
WAVE_FORMAT_IEEE_FLOAT = 0x0003
WAVE_FORMAT_EXTENSIBLE = 0xFFFE
KSDATAFORMAT_SUBTYPE_PCM = bytes.fromhex("0100000000001000800000aa00389b71")


def chunk(cid: bytes, payload: bytes, pad: bool = True, declared_size: int | None = None) -> bytes:
    size = len(payload) if declared_size is None else declared_size
    body = cid + struct.pack("<I", size) + payload
    if pad and len(payload) % 2:
        body += b"\0"
    return body


def riff(chunks: bytes, form: bytes = b"WAVE", declared_size: int | None = None) -> bytes:
    size = 4 + len(chunks) if declared_size is None else declared_size
    return b"RIFF" + struct.pack("<I", size) + form + chunks


def fmt_payload(tag: int, channels: int, rate: int, container_bits: int, cb_size: int | None = None,
                valid_bits: int | None = None, channel_mask: int = 0, subformat: bytes | None = None) -> bytes:
    block_align = channels * container_bits // 8
    payload = struct.pack("<HHIIHH", tag, channels, rate, rate * block_align, block_align, container_bits)
    if cb_size is not None:
        payload += struct.pack("<H", cb_size)
    if tag == WAVE_FORMAT_EXTENSIBLE:
        payload += struct.pack("<HI", valid_bits or container_bits, channel_mask)
        payload += subformat or KSDATAFORMAT_SUBTYPE_PCM
    return payload


def wav(fmt: bytes, data: bytes, before_data: bytes = b"", after_data: bytes = b"") -> bytes:
    return riff(chunk(b"fmt ", fmt) + before_data + chunk(b"data", data) + after_data)


def info_list(items: list[tuple[bytes, bytes]]) -> bytes:
    body = b"INFO"
    for key, text in items:
        body += chunk(key, text + b"\0")
    return chunk(b"LIST", body)


def bext_payload() -> bytes:
    """A Broadcast Wave Format bext chunk with obviously synthetic text."""
    description = b"SYNTHETIC BWF DESCRIPTION - Pulp fuzz seed".ljust(256, b"\0")
    originator = b"SYNTHETIC ORIGINATOR".ljust(32, b"\0")
    originator_ref = b"SYNTHREF000000000000000000000000"[:32].ljust(32, b"\0")
    origination_date = b"2000-01-01"
    origination_time = b"00:00:00"
    time_ref = struct.pack("<Q", 0)
    version = struct.pack("<H", 1)
    umid = bytes(64)
    loudness = struct.pack("<hhhhh", 0, 0, 0, 0, 0)
    reserved = bytes(180)
    coding_history = b"A=PCM,F=8000,W=16,M=mono,T=synthetic\r\n"
    return (description + originator + originator_ref + origination_date + origination_time + time_ref
            + version + umid + loudness + reserved + coding_history)


# --------------------------------------------------------------------------
# AIFF / AIFF-C construction (big-endian IFF)
# --------------------------------------------------------------------------


def extended80(value: int) -> bytes:
    """IEEE 754 80-bit extended encoding of a positive integer (AIFF sample rate)."""
    if value <= 0:
        return bytes(10)
    exponent = value.bit_length() - 1
    mantissa = value << (63 - exponent)
    return struct.pack(">HQ", 16383 + exponent, mantissa)


def iff_chunk(cid: bytes, payload: bytes, declared_size: int | None = None) -> bytes:
    size = len(payload) if declared_size is None else declared_size
    body = cid + struct.pack(">I", size) + payload
    if len(payload) % 2:
        body += b"\0"
    return body


def form(chunks: bytes, kind: bytes) -> bytes:
    return b"FORM" + struct.pack(">I", 4 + len(chunks)) + kind + chunks


def aiff_comm(channels: int, frames: int, bits: int, rate: int, compression: bytes | None = None,
              compression_name: bytes = b"") -> bytes:
    payload = struct.pack(">hIh", channels, frames, bits) + extended80(rate)
    if compression is not None:
        pstring = bytes([len(compression_name)]) + compression_name
        if len(pstring) % 2:
            pstring += b"\0"
        payload += compression + pstring
    return iff_chunk(b"COMM", payload)


def aiff_ssnd(data: bytes, declared_size: int | None = None) -> bytes:
    payload = struct.pack(">II", 0, 0) + data
    return iff_chunk(b"SSND", payload, declared_size)


# --------------------------------------------------------------------------
# FLAC: a tiny variable-blocksize encoder (VERBATIM subframes) and
# STREAMINFO rewriting
# --------------------------------------------------------------------------


def crc8(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def crc16_flac(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x8005) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def flac_utf8_number(value: int) -> bytes:
    """FLAC's extended UTF-8-style coding of frame/sample numbers (up to 36 bits)."""
    if value < 0x80:
        return bytes([value])
    for length, first_mask, limit in ((2, 0xC0, 1 << 11), (3, 0xE0, 1 << 16), (4, 0xF0, 1 << 21),
                                      (5, 0xF8, 1 << 26), (6, 0xFC, 1 << 31), (7, 0xFE, 1 << 36)):
        if value < limit:
            continuation = [(0x80 | ((value >> (6 * i)) & 0x3F)) for i in range(length - 1)][::-1]
            return bytes([first_mask | (value >> (6 * (length - 1)))] + continuation)
    raise ValueError("value too large for FLAC utf8 coding")


def flac_blocksize_code(blocksize: int) -> tuple[int, bytes]:
    fixed = {192: 0x1, 576: 0x2, 1152: 0x3, 2304: 0x4, 4608: 0x5,
             256: 0x8, 512: 0x9, 1024: 0xA, 2048: 0xB, 4096: 0xC, 8192: 0xD, 16384: 0xE, 32768: 0xF}
    if blocksize in fixed:
        return fixed[blocksize], b""
    if blocksize - 1 < 256:
        return 0x6, bytes([blocksize - 1])
    return 0x7, struct.pack(">H", blocksize - 1)


def flac_frame_variable(samples: list[int], first_sample: int, rate_code: int, bits: int) -> bytes:
    """One mono FLAC frame, variable blocksize strategy, VERBATIM subframe."""
    bs_code, bs_extra = flac_blocksize_code(len(samples))
    sample_size_code = {8: 0x1, 12: 0x2, 16: 0x4, 20: 0x5, 24: 0x6, 32: 0x7}[bits]
    header = bytes([0xFF, 0xF9])  # sync + reserved 0 + blocking strategy 1 (variable)
    header += bytes([(bs_code << 4) | rate_code])
    header += bytes([(0x0 << 4) | (sample_size_code << 1)])  # mono, reserved 0
    header += flac_utf8_number(first_sample) + bs_extra
    header += bytes([crc8(header)])
    subframe = bytes([0x02]) + pack_pcm_be(samples, bits)  # VERBATIM, no wasted bits
    frame = header + subframe
    return frame + struct.pack(">H", crc16_flac(frame))


def flac_streaminfo(min_bs: int, max_bs: int, min_fs: int, max_fs: int, rate: int, channels: int, bits: int,
                    total: int, md5: bytes, last: bool = True) -> bytes:
    packed = (rate << 44) | ((channels - 1) << 41) | ((bits - 1) << 36) | total
    body = struct.pack(">HH", min_bs, max_bs) + min_fs.to_bytes(3, "big") + max_fs.to_bytes(3, "big")
    body += packed.to_bytes(8, "big") + md5
    header = bytes([(0x80 if last else 0) | 0x00]) + len(body).to_bytes(3, "big")
    return header + body


def build_variable_blocksize_flac(samples: list[int], rate: int, bits: int, blocks: list[int]) -> bytes:
    assert sum(blocks) == len(samples)
    rate_code = {88200: 0x1, 176400: 0x2, 192000: 0x3, 8000: 0x4, 16000: 0x5, 22050: 0x6, 24000: 0x7,
                 32000: 0x8, 44100: 0x9, 48000: 0xA, 96000: 0xB}[rate]
    frames = []
    position = 0
    for size in blocks:
        frames.append(flac_frame_variable(samples[position:position + size], position, rate_code, bits))
        position += size
    md5 = hashlib.md5(pack_pcm_le(samples, bits)).digest()
    info = flac_streaminfo(min(blocks), max(blocks), min(map(len, frames)), max(map(len, frames)), rate, 1,
                           bits, len(samples), md5)
    return b"fLaC" + info + b"".join(frames)


def flac_set_total_samples(data: bytes, total: int) -> bytes:
    """Rewrite STREAMINFO.total_samples (36 bits) of an existing FLAC stream."""
    assert data[:4] == b"fLaC" and data[4] & 0x7F == 0
    body_start = 8
    packed = int.from_bytes(data[body_start + 10:body_start + 18], "big")
    packed = (packed & ~((1 << 36) - 1)) | (total & ((1 << 36) - 1))
    return data[:body_start + 10] + packed.to_bytes(8, "big") + data[body_start + 18:]


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


# --------------------------------------------------------------------------
# MP3 helpers
# --------------------------------------------------------------------------


def ape_tag_items(items: list[tuple[bytes, bytes]]) -> bytes:
    body = b""
    for key, value in items:
        body += struct.pack("<II", len(value), 0) + key + b"\0" + value
    return body


def apev2_tag(items: list[tuple[bytes, bytes]], version: int = 2000, with_header: bool = True) -> bytes:
    """Build an APE tag: optional 32-byte header, items, 32-byte footer.

    The footer's size field (bytes 12-15) counts the items and the footer but
    not the header. Flags bit 31 says a header is present and bit 29 marks the
    block that is the header. APEv1 (version 1000) has no header and leaves
    the flags zero, which `with_header=False` reproduces.
    """
    body = ape_tag_items(items)
    tag_size = len(body) + 32  # items plus footer
    has_header = (1 << 31) if with_header else 0
    is_header = 1 << 29

    def block(flags: int) -> bytes:
        return b"APETAGEX" + struct.pack("<IIII", version, tag_size, len(items), flags) + bytes(8)

    header = block(has_header | is_header) if with_header else b""
    return header + body + block(has_header)


def apev2_tag_with_size(items: list[tuple[bytes, bytes]], tag_size: int) -> bytes:
    """Build an APEv2 tag whose footer size field is exactly `tag_size`.

    A trailing `Padding` item absorbs the difference, so the low size byte can
    be steered to a value at or above 0x80.
    """
    fixed = len(ape_tag_items(items + [(b"Padding", b"")])) + 32
    if tag_size < fixed:
        raise ValueError(f"tag size {tag_size} is below the {fixed}-byte minimum for these items")
    tag = apev2_tag(items + [(b"Padding", b"\0" * (tag_size - fixed))])
    assert struct.unpack_from("<I", tag, len(tag) - 20)[0] == tag_size
    return tag


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


# --------------------------------------------------------------------------
# Corpus assembly
# --------------------------------------------------------------------------


class Corpus:
    def __init__(self) -> None:
        self.files: dict[str, bytes] = {}
        self.notes: list[str] = []
        self.tools_used: dict[str, str] = {}
        self.skipped: list[str] = []

    def add(self, name: str, data: bytes) -> bytes:
        if name in self.files:
            raise ValueError(f"duplicate seed name {name}")
        if len(data) > MAX_SEED_BYTES:
            raise ValueError(f"{name} is {len(data)} bytes, over the {MAX_SEED_BYTES}-byte seed budget")
        self.files[name] = data
        return data

    def skip(self, name: str, reason: str) -> None:
        self.skipped.append(f"{name}: {reason}")
        print(f"skip {name}: {reason}", file=sys.stderr)


def build_wav_seeds(corpus: Corpus) -> None:
    rate = 8000
    mono16 = quantize(make_signal(rate, 0.1, 1), 16)
    mono16_bytes = pack_pcm_le(interleave(mono16), 16)
    stereo = make_signal(rate, 0.1, 2)

    # Canonical 44-byte header: 16-byte fmt directly followed by data.
    base = corpus.add("wav-pcm16-mono-canonical44.wav",
                      wav(fmt_payload(WAVE_FORMAT_PCM, 1, rate, 16), mono16_bytes))
    assert base.index(b"data") == 36

    corpus.add("wav-pcm8-stereo.wav",
               wav(fmt_payload(WAVE_FORMAT_PCM, 2, rate, 8), pack_pcm_le(interleave(quantize(stereo, 8)), 8)))
    corpus.add("wav-pcm24-stereo.wav",
               wav(fmt_payload(WAVE_FORMAT_PCM, 2, rate, 24), pack_pcm_le(interleave(quantize(stereo, 24)), 24)))
    corpus.add("wav-pcm32-stereo.wav",
               wav(fmt_payload(WAVE_FORMAT_PCM, 2, rate, 32), pack_pcm_le(interleave(quantize(stereo, 32)), 32)))
    mono = make_signal(rate, 0.1, 1)
    corpus.add("wav-float32-mono-fmt18.wav",
               wav(fmt_payload(WAVE_FORMAT_IEEE_FLOAT, 1, rate, 32, cb_size=0), pack_float(mono, "f")))
    corpus.add("wav-float64-mono.wav",
               wav(fmt_payload(WAVE_FORMAT_IEEE_FLOAT, 1, rate, 64), pack_float(mono, "d")))
    corpus.add("wav-extensible-pcm24in32-stereo.wav",
               wav(fmt_payload(WAVE_FORMAT_EXTENSIBLE, 2, rate, 32, cb_size=22, valid_bits=24, channel_mask=0x3),
                   pack_pcm_le(interleave(quantize(stereo, 24)), 24, container_bits=32)))
    corpus.add("wav-bwf-bext-before-data.wav",
               wav(fmt_payload(WAVE_FORMAT_PCM, 1, rate, 16), mono16_bytes,
                   before_data=chunk(b"bext", bext_payload())))
    corpus.add("wav-list-info-heavy.wav",
               wav(fmt_payload(WAVE_FORMAT_PCM, 1, rate, 16), mono16_bytes,
                   before_data=info_list([(b"INAM", b"SYNTHETIC TITLE"), (b"IART", b"SYNTHETIC ARTIST"),
                                          (b"IPRD", b"SYNTHETIC PRODUCT"), (b"ICMT", b"generated by Pulp"),
                                          (b"ICRD", b"2000-01-01"), (b"IGNR", b"test signal"),
                                          (b"ISFT", b"audio_decoder_seed_corpus.py"), (b"ICOP", b"MIT")]),
                   after_data=info_list([(b"IKEY", b"fuzz;seed;synthetic"), (b"ISBJ", b"trailing INFO list")])))

    # RF64: RIFF size and data size hold the 0xFFFFFFFF sentinel; ds64 carries the real sizes.
    rf64_fmt = chunk(b"fmt ", fmt_payload(WAVE_FORMAT_PCM, 1, rate, 16))
    data_chunk = chunk(b"data", mono16_bytes, declared_size=0xFFFFFFFF)
    ds64 = struct.pack("<QQQI", 0, len(mono16_bytes), len(mono16), 0)
    chunks = chunk(b"ds64", ds64) + rf64_fmt + data_chunk
    ds64_body = struct.pack("<QQQI", 4 + len(chunks), len(mono16_bytes), len(mono16), 0)
    chunks = chunk(b"ds64", ds64_body) + rf64_fmt + data_chunk
    corpus.add("wav-rf64-ds64.wav", b"RF64" + struct.pack("<I", 0xFFFFFFFF) + b"WAVE" + chunks)

    # Sony Wave64: 16-byte GUID chunk ids, 64-bit sizes that include the 24-byte chunk header.
    def w64_guid(fourcc: bytes) -> bytes:
        return fourcc + bytes.fromhex("f3acd3118cd100c04f8edb8a")

    def w64_chunk(fourcc: bytes, payload: bytes) -> bytes:
        body = w64_guid(fourcc) + struct.pack("<Q", 24 + len(payload)) + payload
        if len(body) % 8:
            body += bytes(8 - len(body) % 8)
        return body

    w64_body = w64_chunk(b"fmt ", fmt_payload(WAVE_FORMAT_PCM, 1, rate, 16)) + w64_chunk(b"data", mono16_bytes)
    w64_wave_guid = bytes.fromhex("77617665f3acd3118cd100c04f8edb8a")
    # Kept under .wav: the registry claims seeds by extension and has no .w64 entry.
    corpus.add("wav-w64-pcm16-mono.wav",
               bytes.fromhex("72696666 2e91cf11 a5d628db 04c10000".replace(" ", ""))
               + struct.pack("<Q", 40 + len(w64_body)) + w64_wave_guid + w64_body)

    # Malformed variants, each derived from a valid base above.
    fmt16 = fmt_payload(WAVE_FORMAT_PCM, 1, rate, 16)
    odd_data = mono16_bytes[:-1]  # 1599 bytes: odd, with one partial frame
    corpus.add("wav-odd-chunk-eof-no-pad.wav", riff(chunk(b"fmt ", fmt16) + chunk(b"data", odd_data, pad=False)))
    corpus.add("wav-duplicate-fmt.wav",
               riff(chunk(b"fmt ", fmt16) + chunk(b"fmt ", fmt_payload(WAVE_FORMAT_PCM, 2, 44100, 8))
                    + chunk(b"data", mono16_bytes)))
    corpus.add("wav-fmt-after-data.wav", riff(chunk(b"data", mono16_bytes) + chunk(b"fmt ", fmt16)))
    truncated = base[:44 + len(mono16_bytes) // 3]
    corpus.add("wav-data-past-eof.wav", truncated)
    corpus.add("wav-data-size-zero.wav", riff(chunk(b"fmt ", fmt16) + chunk(b"data", b"")))
    corpus.add("wav-data-size-ffffffff-no-ds64.wav",
               riff(chunk(b"fmt ", fmt16) + chunk(b"data", mono16_bytes, declared_size=0xFFFFFFFF)))
    # fmt declares 16 bytes more than it carries, so the declared span runs into the data header.
    corpus.add("wav-overlapping-chunks.wav",
               riff(chunk(b"fmt ", fmt16, declared_size=len(fmt16) + 16) + chunk(b"data", mono16_bytes)))
    declared_frames = 100_000_001
    corpus.add("wav-declares-100000001-frames.wav",
               riff(chunk(b"fmt ", fmt16) + chunk(b"data", mono16_bytes, declared_size=declared_frames * 2),
                    declared_size=4 + 8 + len(fmt16) + 8 + declared_frames * 2))
    corpus.add("wav-riff-header-only.wav", b"RIFF" + struct.pack("<I", 0))
    corpus.add("wav-riff-size-lying-small.wav", riff(chunk(b"fmt ", fmt16) + chunk(b"data", mono16_bytes),
                                                     declared_size=4 + 8 + len(fmt16) + 8 + 100))


def build_aiff_seeds(corpus: Corpus) -> None:
    rate = 8000
    mono16 = quantize(make_signal(rate, 0.1, 1), 16)
    stereo16 = quantize(make_signal(rate, 0.1, 2), 16)
    frames = len(mono16[0])
    mono_be = pack_pcm_be(interleave(mono16), 16)

    base = corpus.add("aiff-pcm16-mono.aiff", form(aiff_comm(1, frames, 16, rate) + aiff_ssnd(mono_be), b"AIFF"))
    fver = iff_chunk(b"FVER", struct.pack(">I", 0xA2805140))
    corpus.add("aifc-none-pcm16-mono.aifc",
               form(fver + aiff_comm(1, frames, 16, rate, b"NONE", b"not compressed") + aiff_ssnd(mono_be), b"AIFC"))
    corpus.add("aifc-sowt-pcm16-stereo.aifc",
               form(fver + aiff_comm(2, frames, 16, rate, b"sowt", b"")
                    + aiff_ssnd(pack_pcm_le(interleave(stereo16), 16)), b"AIFC"))
    corpus.add("aiff-ssnd-past-eof.aiff",
               form(aiff_comm(1, frames, 16, rate) + aiff_ssnd(mono_be[:len(mono_be) // 4], declared_size=8 + len(mono_be)),
                    b"AIFF"))
    corpus.add("aiff-comm-after-ssnd.aiff", form(aiff_ssnd(mono_be) + aiff_comm(1, frames, 16, rate), b"AIFF"))
    assert base[:4] == b"FORM"


def write_temp_wav(path: str, rate: int, seconds: float, channels: int) -> None:
    signal = quantize(make_signal(rate, seconds, channels), 16)
    with open(path, "wb") as f:
        f.write(wav(fmt_payload(WAVE_FORMAT_PCM, channels, rate, 16), pack_pcm_le(interleave(signal), 16)))


def build_flac_seeds(corpus: Corpus, tmp: str) -> None:
    rate = 8000
    mono16 = quantize(make_signal(rate, 0.1, 1), 16)[0]
    # Hand-built: variable blocksize (192, 256, 200, 152) exercising all four blocksize code forms.
    variable = corpus.add("flac-variable-blocksize-verbatim.flac",
                          build_variable_blocksize_flac(mono16, rate, 16, [192, 256, 200, 152]))
    corpus.add("flac-streaminfo-huge-total-samples.flac", flac_set_total_samples(variable, (1 << 36) - 1))

    flac = find_tool("flac")
    if not flac:
        for name in ("flac-fixed-blocksize-mono.flac", "flac-seektable-padding.flac", "flac-truncated-midframe.flac"):
            corpus.skip(name, "flac encoder not found")
        return
    corpus.tools_used["flac"] = flac
    src = os.path.join(tmp, "flac-src.wav")
    write_temp_wav(src, rate, 0.1, 1)
    fixed_path = os.path.join(tmp, "fixed.flac")
    run([flac, "--totally-silent", "-f", "--no-padding", "--no-seektable", "-b", "192", "-8", "-o", fixed_path, src])
    with open(fixed_path, "rb") as f:
        fixed = corpus.add("flac-fixed-blocksize-mono.flac", f.read())
    seek_path = os.path.join(tmp, "seek.flac")
    run([flac, "--totally-silent", "-f", "-P", "256", "-S", "4x", "-b", "192", "-5", "-o", seek_path, src])
    with open(seek_path, "rb") as f:
        corpus.add("flac-seektable-padding.flac", f.read())
    # Cut inside the frame region: past the metadata, two thirds of the way through the stream.
    corpus.add("flac-truncated-midframe.flac", fixed[:len(fixed) * 2 // 3])


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


def build_mp3_seeds(corpus: Corpus, tmp: str) -> None:
    lame = find_tool("lame")
    names = ("mp3-cbr-mono.mp3", "mp3-id3v2-xing-vbr.mp3", "mp3-apev2-footer.mp3", "mp3-apev1-footer.mp3",
             "mp3-ape-size-below-footer.mp3", "mp3-apev1-header-flag.mp3", "mp3-apev2-size-high-bit.mp3",
             "mp3-truncated-frame.mp3")
    if not lame:
        for name in names:
            corpus.skip(name, "lame encoder not found")
        return
    corpus.tools_used["lame"] = lame
    src = os.path.join(tmp, "mp3-src.wav")
    write_temp_wav(src, 32000, 0.2, 1)
    # `--resample 32` keeps LAME at 32 kHz MPEG-1 Layer III; left alone it drops a 32 kbit/s
    # mono stream to 22.05 kHz MPEG-2. `-t` suppresses the Xing/Info tag on the plain CBR seed.
    common = [lame, "--quiet", "--nohist", "--noreplaygain", "--resample", "32", "-m", "m"]
    cbr_path = os.path.join(tmp, "cbr.mp3")
    run(common + ["-t", "--cbr", "-b", "32", src, cbr_path])
    with open(cbr_path, "rb") as f:
        cbr = corpus.add("mp3-cbr-mono.mp3", f.read())
    # VBR adds a Xing header; the ID3v2.3 tag is Latin-1 so every reader agrees on its text frames.
    vbr_path = os.path.join(tmp, "vbr.mp3")
    run(common + ["-V", "9", "--id3v2-only", "--id3v2-latin1", "--tt", "SYNTHETIC TITLE", "--ta", "SYNTHETIC ARTIST",
                  "--tl", "SYNTHETIC ALBUM", "--tc", "generated by Pulp seed corpus", src, vbr_path])
    with open(vbr_path, "rb") as f:
        corpus.add("mp3-id3v2-xing-vbr.mp3", f.read())
    corpus.add("mp3-apev2-footer.mp3",
               cbr + apev2_tag([(b"Title", b"SYNTHETIC TITLE"), (b"Comment", b"APEv2 footer appended by Pulp")]))
    # APEv1 carries no header block, so its footer flags leave bit 31 clear and
    # a reader that always strips 32 extra bytes cuts into the last frame.
    corpus.add("mp3-apev1-footer.mp3",
               cbr + apev2_tag([(b"Title", b"SYNTHETIC TITLE"), (b"Comment", b"APEv1 footer appended by Pulp")],
                               version=1000, with_header=False))
    # A footer whose size field (0) is below the footer's own 32 bytes, with no
    # items and no header. A reader that trusts the size strips less than the
    # footer and leaves tag bytes in front of the frame matcher.
    corpus.add("mp3-ape-size-below-footer.mp3",
               cbr + b"APETAGEX" + struct.pack("<IIII", 2000, 0, 0, 0) + bytes(8))
    # An APEv1 tag whose footer flags carry bit 31. APEv1 has no header block,
    # so a reader that trusts the flag strips 32 bytes of audio.
    v1_flagged = bytearray(apev2_tag([(b"Title", b"SYNTHETIC TITLE")], version=1000, with_header=False))
    struct.pack_into("<I", v1_flagged, len(v1_flagged) - 12, 1 << 31)
    corpus.add("mp3-apev1-header-flag.mp3", cbr + bytes(v1_flagged))
    # A size of 0x1F0 puts 0xF0 in footer byte 12, so a reader that widens the
    # footer bytes through a signed char reads a sign-extended size.
    high_bit = apev2_tag_with_size([(b"Title", b"SYNTHETIC TITLE")], 0x1F0)
    assert high_bit[-20] >= 0x80, "footer size byte 12 must have its high bit set"
    corpus.add("mp3-apev2-size-high-bit.mp3", cbr + high_bit)
    # The CBR seed carries no ID3 tag and every MPEG-1 Layer III frame at
    # 32 kbit/s and 32 kHz is 144 bytes, so the fourth frame starts at
    # 3 * 144; the cut lands 17 bytes into it. The sync word is asserted so a
    # change in LAME's framing fails here rather than producing a seed that
    # truncates somewhere else.
    fourth_frame = 3 * 144
    assert cbr[fourth_frame] == 0xFF and (cbr[fourth_frame + 1] & 0xE0) == 0xE0, "no frame sync at 3 * 144"
    corpus.add("mp3-truncated-frame.mp3", cbr[:fourth_frame + 17])


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


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    out_dir = argv[1]
    os.makedirs(out_dir, exist_ok=True)
    corpus = Corpus()
    with tempfile.TemporaryDirectory(prefix="pulp-seed-corpus-") as tmp:
        build_wav_seeds(corpus)
        build_aiff_seeds(corpus)
        build_flac_seeds(corpus, tmp)
        build_ogg_seeds(corpus, tmp)
        build_mp3_seeds(corpus, tmp)
        build_coreaudio_seeds(corpus, tmp)

    total = 0
    print(f"{'name':44} {'bytes':>7}  sha256")
    for name in sorted(corpus.files):
        data = corpus.files[name]
        with open(os.path.join(out_dir, name), "wb") as f:
            f.write(data)
        total += len(data)
        print(f"{name:44} {len(data):7d}  {hashlib.sha256(data).hexdigest()}")
    print(f"{len(corpus.files)} seeds, {total} bytes total")
    for tool, path in sorted(corpus.tools_used.items()):
        print(f"encoder used: {tool} -> {path}")
    for line in corpus.skipped:
        print(f"skipped: {line}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
