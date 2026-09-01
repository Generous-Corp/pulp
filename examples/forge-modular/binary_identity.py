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
    signature_range = None
    signature_command_range = None
    non_linkedit_end = 0
    linkedit_ranges = []
    linkedit_size_fields = []
    for _ in range(ncmds):
        if offset + 8 > len(data):
            raise ValueError("truncated Mach-O load commands")
        command, size = struct.unpack_from(endian + "II", data, offset)
        if size < 8 or offset + size > len(data):
            raise ValueError("invalid Mach-O load command")
        if command == LC_CODE_SIGNATURE:
            if size < 16:
                raise ValueError("truncated LC_CODE_SIGNATURE")
            if signature_range is not None:
                raise ValueError("multiple LC_CODE_SIGNATURE commands")
            signature_range = struct.unpack_from(
                endian + "II", data, offset + 8)
            signature_command_range = (offset + 8, offset + 16)
        elif command in (LC_SEGMENT, LC_SEGMENT_64):
            if size < 40:
                raise ValueError("truncated Mach-O segment command")
            segment_name = bytes(data[offset + 8:offset + 24]).rstrip(b"\0")
            if command == LC_SEGMENT_64:
                if size < 56:
                    raise ValueError("truncated LC_SEGMENT_64")
                file_offset, file_size = struct.unpack_from(
                    endian + "QQ", data, offset + 40)
                size_fields = ((offset + 32, offset + 40),
                               (offset + 48, offset + 56))
            else:
                file_offset, file_size = struct.unpack_from(
                    endian + "II", data, offset + 32)
                size_fields = ((offset + 28, offset + 32),
                               (offset + 36, offset + 40))
            file_end = file_offset + file_size
            if file_end > len(data):
                raise ValueError("Mach-O segment extends beyond end of file")
            if segment_name == b"__LINKEDIT":
                linkedit_ranges.append((file_offset, file_end))
                linkedit_size_fields.extend(size_fields)
            else:
                non_linkedit_end = max(non_linkedit_end, file_end)
        offset += size
    if signature_range is not None:
        signature_offset, signature_size = signature_range
        signature_end = signature_offset + signature_size
        if not signature_size or signature_offset < offset:
            raise ValueError("code signature overlaps Mach-O load commands")
        if not non_linkedit_end:
            raise ValueError("signed Mach-O has no file-backed executable segment")
        if signature_end != len(data):
            raise ValueError("code signature is not the terminal Mach-O blob")
        if signature_offset < non_linkedit_end:
            raise ValueError("code signature overlaps file-backed executable data")
        if linkedit_ranges and not any(
                start <= signature_offset and signature_end <= end
                for start, end in linkedit_ranges):
            raise ValueError("code signature is outside __LINKEDIT")
        assert signature_command_range is not None
        data[slice(*signature_command_range)] = b"\0" * 8
        for start, end in linkedit_size_fields:
            data[start:end] = b"\0" * (end - start)
        data = data[:signature_offset]
    return hashlib.sha256(data).hexdigest()


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: binary_identity.py <Mach-O>")
    print(content_sha256(sys.argv[1]))
