"""FLAC seeds: a tiny VERBATIM encoder for the hand-built stream, the
reference encoder for the rest, and STREAMINFO rewriting.
"""

from __future__ import annotations

import hashlib
import os
import struct

from .common import Corpus, find_tool, make_signal, pack_pcm_be, pack_pcm_le, quantize, run
from .wav import write_temp_wav


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
