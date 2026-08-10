#!/usr/bin/env python3
"""Hash Mach-O content while excluding its replaceable code signature."""

import hashlib
from pathlib import Path
import struct
import sys


LC_CODE_SIGNATURE = 0x1D
LC_SEGMENT = 0x1
LC_SEGMENT_64 = 0x19


def content_sha256(path: str) -> str:
    data = bytearray(Path(path).read_bytes())
    if len(data) < 28:
        return hashlib.sha256(data).hexdigest()
    magic = bytes(data[:4])
    if magic == b"\xcf\xfa\xed\xfe":
        endian, header_size = "<", 32
    elif magic == b"\xfe\xed\xfa\xcf":
        endian, header_size = ">", 32
    elif magic == b"\xce\xfa\xed\xfe":
        endian, header_size = "<", 28
    elif magic == b"\xfe\xed\xfa\xce":
        endian, header_size = ">", 28
    else:
        return hashlib.sha256(data).hexdigest()

    ncmds = struct.unpack_from(endian + "I", data, 16)[0]
    offset = header_size
    signature_offset = None
    for _ in range(ncmds):
        if offset + 8 > len(data):
            raise ValueError("truncated Mach-O load commands")
        command, size = struct.unpack_from(endian + "II", data, offset)
        if size < 8 or offset + size > len(data):
            raise ValueError("invalid Mach-O load command")
        if command == LC_CODE_SIGNATURE:
            if size < 16:
                raise ValueError("truncated LC_CODE_SIGNATURE")
            signature_offset = struct.unpack_from(endian + "I", data, offset + 8)[0]
            data[offset + 8:offset + 16] = b"\0" * 8
        elif command in (LC_SEGMENT, LC_SEGMENT_64) and size >= 40:
            segment_name = bytes(data[offset + 8:offset + 24]).rstrip(b"\0")
            if segment_name == b"__LINKEDIT":
                # codesign grows/shrinks __LINKEDIT to contain the replaceable
                # signature. Those size fields describe the excluded bytes,
                # not executable content.
                if command == LC_SEGMENT_64:
                    if size < 56:
                        raise ValueError("truncated LC_SEGMENT_64")
                    data[offset + 32:offset + 40] = b"\0" * 8  # vmsize
                    data[offset + 48:offset + 56] = b"\0" * 8  # filesize
                else:
                    data[offset + 28:offset + 32] = b"\0" * 4  # vmsize
                    data[offset + 36:offset + 40] = b"\0" * 4  # filesize
        offset += size
    if signature_offset is not None:
        if signature_offset > len(data):
            raise ValueError("code signature begins beyond end of Mach-O")
        data = data[:signature_offset]
    return hashlib.sha256(data).hexdigest()


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: binary_identity.py <Mach-O>")
    print(content_sha256(sys.argv[1]))
