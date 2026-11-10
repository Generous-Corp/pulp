"""AIFF and AIFF-C seeds, built byte by byte (big-endian IFF).
"""

from __future__ import annotations

import struct

from .common import Corpus, interleave, make_signal, pack_pcm_be, pack_pcm_le, quantize


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
