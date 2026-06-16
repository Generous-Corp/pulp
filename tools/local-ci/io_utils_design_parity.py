"""Design-parity image diffing + a tiny stdlib PNG codec.

Pure-stdlib PNG read/write/resize and a perceptual-ish diff summary used by the
design-parity video recipe to compare a reference image against a rendered
frame without pulling in Pillow/numpy.
"""

from __future__ import annotations

from pathlib import Path
import struct
import zlib


def _paeth_predictor(left: int, up: int, up_left: int) -> int:
    p = left + up - up_left
    pa = abs(p - left)
    pb = abs(p - up)
    pc = abs(p - up_left)
    if pa <= pb and pa <= pc:
        return left
    if pb <= pc:
        return up
    return up_left


def _read_png_rgb(path: Path) -> tuple[int, int, list[tuple[int, int, int]]]:
    data = path.read_bytes()
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ValueError("not a PNG file")
    offset = 8
    width = height = bit_depth = color_type = None
    compressed = bytearray()
    while offset + 8 <= len(data):
        length = struct.unpack(">I", data[offset : offset + 4])[0]
        chunk_type = data[offset + 4 : offset + 8]
        chunk_data = data[offset + 8 : offset + 8 + length]
        offset += 12 + length
        if chunk_type == b"IHDR":
            width, height, bit_depth, color_type, _compression, _filter, interlace = struct.unpack(">IIBBBBB", chunk_data)
            if bit_depth != 8 or color_type not in {2, 6} or interlace != 0:
                raise ValueError("only non-interlaced 8-bit RGB/RGBA PNGs are supported")
        elif chunk_type == b"IDAT":
            compressed.extend(chunk_data)
        elif chunk_type == b"IEND":
            break
    if width is None or height is None or bit_depth is None or color_type is None:
        raise ValueError("missing PNG header")
    channels = 4 if color_type == 6 else 3
    stride = width * channels
    raw = zlib.decompress(bytes(compressed))
    rows: list[bytearray] = []
    pos = 0
    previous = bytearray(stride)
    for _row in range(height):
        filter_type = raw[pos]
        pos += 1
        row = bytearray(raw[pos : pos + stride])
        pos += stride
        for i, value in enumerate(row):
            left = row[i - channels] if i >= channels else 0
            up = previous[i]
            up_left = previous[i - channels] if i >= channels else 0
            if filter_type == 1:
                row[i] = (value + left) & 0xFF
            elif filter_type == 2:
                row[i] = (value + up) & 0xFF
            elif filter_type == 3:
                row[i] = (value + ((left + up) // 2)) & 0xFF
            elif filter_type == 4:
                row[i] = (value + _paeth_predictor(left, up, up_left)) & 0xFF
            elif filter_type != 0:
                raise ValueError(f"unsupported PNG filter type {filter_type}")
        rows.append(row)
        previous = row
    pixels: list[tuple[int, int, int]] = []
    for row in rows:
        for x in range(width):
            base = x * channels
            pixels.append((row[base], row[base + 1], row[base + 2]))
    return width, height, pixels


def _write_png_rgb(path: Path, width: int, height: int, pixels: list[tuple[int, int, int]]) -> None:
    def chunk(chunk_type: bytes, payload: bytes) -> bytes:
        return (
            struct.pack(">I", len(payload))
            + chunk_type
            + payload
            + struct.pack(">I", zlib.crc32(chunk_type + payload) & 0xFFFFFFFF)
        )

    raw = bytearray()
    for y in range(height):
        raw.append(0)
        for x in range(width):
            raw.extend(bytes(pixels[y * width + x]))
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b""))


def _resize_rgb_nearest(
    width: int,
    height: int,
    pixels: list[tuple[int, int, int]],
    target_width: int,
    target_height: int,
) -> list[tuple[int, int, int]]:
    if width == target_width and height == target_height:
        return pixels
    resized: list[tuple[int, int, int]] = []
    for y in range(target_height):
        source_y = min(height - 1, int(y * height / target_height))
        for x in range(target_width):
            source_x = min(width - 1, int(x * width / target_width))
            resized.append(pixels[source_y * width + source_x])
    return resized


def _stdlib_design_parity_diff_summary(
    source_path: Path,
    native_path: Path,
    *,
    diff_output_path: Path,
    resized_source_output_path: Path | None,
    enhance_brightness: float,
) -> dict:
    source_width, source_height, source_pixels = _read_png_rgb(source_path)
    native_width, native_height, native_pixels = _read_png_rgb(native_path)
    resized = source_width != native_width or source_height != native_height
    source_for_diff = _resize_rgb_nearest(source_width, source_height, source_pixels, native_width, native_height)
    if resized_source_output_path is not None:
        _write_png_rgb(resized_source_output_path, native_width, native_height, source_for_diff)
    diff_pixels: list[tuple[int, int, int]] = []
    left = top = right = bottom = None
    for index, (source_pixel, native_pixel) in enumerate(zip(source_for_diff, native_pixels)):
        diff = tuple(min(255, int(abs(source_pixel[channel] - native_pixel[channel]) * enhance_brightness)) for channel in range(3))
        diff_pixels.append(diff)
        if diff != (0, 0, 0):
            x = index % native_width
            y = index // native_width
            left = x if left is None else min(left, x)
            top = y if top is None else min(top, y)
            right = x + 1 if right is None else max(right, x + 1)
            bottom = y + 1 if bottom is None else max(bottom, y + 1)
    _write_png_rgb(diff_output_path, native_width, native_height, diff_pixels)
    summary = {
        "source_image": str(source_path),
        "native_image": str(native_path),
        "diff_image": str(diff_output_path),
        "resized_source_image": str(resized_source_output_path) if resized_source_output_path else None,
        "source_size": {"width": source_width, "height": source_height},
        "native_size": {"width": native_width, "height": native_height},
        "resized_source": resized,
        "method": "stdlib-png-resized-source-diff",
        "enhance_brightness": enhance_brightness,
        "changed": left is not None,
    }
    if left is not None and top is not None and right is not None and bottom is not None:
        summary["bbox"] = {"left": left, "top": top, "right": right, "bottom": bottom}
    return summary


def design_parity_diff_summary(
    source_path: Path,
    native_path: Path,
    *,
    diff_output_path: Path,
    resized_source_output_path: Path | None = None,
    enhance_brightness: float = 3.0,
) -> dict:
    try:
        from PIL import Image, ImageChops, ImageEnhance
    except Exception as exc:
        try:
            return _stdlib_design_parity_diff_summary(
                source_path,
                native_path,
                diff_output_path=diff_output_path,
                resized_source_output_path=resized_source_output_path,
                enhance_brightness=enhance_brightness,
            )
        except Exception as fallback_exc:
            raise RuntimeError("design parity diff generation requires Pillow or 8-bit RGB/RGBA PNG inputs") from fallback_exc

    source = Image.open(source_path).convert("RGB")
    native = Image.open(native_path).convert("RGB")
    source_size = {"width": source.size[0], "height": source.size[1]}
    native_size = {"width": native.size[0], "height": native.size[1]}
    resized = source.size != native.size
    if resized:
        resampling = getattr(getattr(Image, "Resampling", Image), "LANCZOS")
        source_for_diff = source.resize(native.size, resampling)
    else:
        source_for_diff = source
    if resized_source_output_path is not None:
        resized_source_output_path.parent.mkdir(parents=True, exist_ok=True)
        source_for_diff.save(resized_source_output_path)
    diff = ImageChops.difference(source_for_diff, native)
    if enhance_brightness != 1.0:
        diff = ImageEnhance.Brightness(diff).enhance(enhance_brightness)
    diff_output_path.parent.mkdir(parents=True, exist_ok=True)
    diff.save(diff_output_path)
    bbox = ImageChops.difference(source_for_diff, native).getbbox()
    summary = {
        "source_image": str(source_path),
        "native_image": str(native_path),
        "diff_image": str(diff_output_path),
        "resized_source_image": str(resized_source_output_path) if resized_source_output_path else None,
        "source_size": source_size,
        "native_size": native_size,
        "resized_source": resized,
        "method": "pillow-resized-source-diff",
        "enhance_brightness": enhance_brightness,
        "changed": bbox is not None,
    }
    if bbox is not None:
        summary["bbox"] = {
            "left": bbox[0],
            "top": bbox[1],
            "right": bbox[2],
            "bottom": bbox[3],
        }
    return summary


