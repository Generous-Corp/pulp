#!/usr/bin/env python3
"""Convert PDF-compatible Illustrator artwork to SVG with provenance.

Adobe Illustrator files are often PDF-compatible.  Pulp keeps conversion as a
small, explicit boundary: Poppler's ``pdftocairo`` performs the conversion,
while this helper records the exact input/output hashes and converter version
beside the resulting SVG.  It intentionally does not attempt to parse an
Illustrator file itself or imply rights to third-party artwork.

The command is safe to call from an importer or a build script.  It never
passes a shell command, refuses unsupported extensions, refuses symlinked
inputs/outputs, writes through a temporary file, and fails closed when the
converter is unavailable or emits an invalid/empty SVG.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile
from datetime import datetime, timezone


SCHEMA = "pulp.illustrator-svg-provenance.v1"
SUPPORTED_SUFFIXES = {".ai", ".pdf"}
DEFAULT_TIMEOUT_SECONDS = 60


class ConversionError(RuntimeError):
    """A safe, user-facing conversion failure."""


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _regular_file(path: Path, *, label: str) -> Path:
    """Resolve a path and require a regular, non-symlink file."""
    try:
        if path.is_symlink():
            raise ConversionError(f"{label} must not be a symlink: {path}")
        info = path.stat()
    except FileNotFoundError as exc:
        raise ConversionError(f"{label} does not exist: {path}") from exc
    except FileNotFoundError as exc:
        raise ConversionError(f"pdftocairo is not an executable file: {candidate}") from exc
    except OSError as exc:
        raise ConversionError(f"cannot inspect {label} {path}: {exc}") from exc
    if not stat.S_ISREG(info.st_mode):
        raise ConversionError(f"{label} is not a regular file: {path}")
    return path.resolve()


def _converter() -> Path:
    # An explicit override is useful for hermetic tests and packaged installs,
    # but it is still checked as an executable regular file.
    override = os.environ.get("PULP_PDF_TO_SVG")
    candidate = Path(override).expanduser() if override else None
    if candidate is None:
        found = shutil.which("pdftocairo")
        if not found:
            raise ConversionError(
                "pdftocairo was not found on PATH; install Poppler or set "
                "PULP_PDF_TO_SVG to an executable converter")
        candidate = Path(found)
    if candidate.is_symlink():
        # ``which`` may return a symlink. Resolve it only after checking that
        # it points to a real executable; the invocation remains argv-safe.
        candidate = candidate.resolve()
    try:
        mode = candidate.stat().st_mode
    except FileNotFoundError as exc:
        raise ConversionError(f"pdftocairo is not an executable file: {candidate}") from exc
    except OSError as exc:
        raise ConversionError(f"cannot inspect pdftocairo: {candidate}: {exc}") from exc
    if not stat.S_ISREG(mode) or not os.access(candidate, os.X_OK):
        raise ConversionError(f"pdftocairo is not an executable file: {candidate}")
    return candidate


def _version(tool: Path) -> str:
    try:
        result = subprocess.run(
            [str(tool), "-v"],
            capture_output=True,
            text=True,
            check=False,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise ConversionError(f"could not query pdftocairo version: {exc}") from exc
    text = (result.stdout + "\n" + result.stderr).strip()
    if result.returncode != 0 or not text:
        raise ConversionError(
            f"pdftocairo version query failed (exit {result.returncode})")
    # Keep receipts compact and avoid embedding arbitrary converter output.
    return text.splitlines()[0].strip()[:256]


def _looks_like_svg(path: Path) -> bool:
    try:
        with path.open("rb") as stream:
            prefix = stream.read(8192)
    except OSError:
        return False
    if b"<svg" not in prefix.lower():
        return False
    # A generated SVG must be XML-ish and contain a closing element.  This is
    # deliberately a lightweight boundary check; Skia performs full parsing.
    return b"</svg" in prefix.lower() or b"</svg" in path.read_bytes()[-8192:].lower()


def convert(
    source: Path,
    output: Path | None = None,
    provenance: Path | None = None,
    *,
    page: int = 1,
    timeout: int = DEFAULT_TIMEOUT_SECONDS,
) -> dict:
    """Convert one PDF-compatible ``.ai``/``.pdf`` file and return its receipt."""
    if page < 1:
        raise ConversionError("page must be a positive integer")
    if timeout < 1:
        raise ConversionError("timeout must be a positive integer")

    source = _regular_file(Path(source), label="source")
    suffix = source.suffix.lower()
    if suffix not in SUPPORTED_SUFFIXES:
        allowed = ", ".join(sorted(SUPPORTED_SUFFIXES))
        raise ConversionError(
            f"unsupported source extension {source.suffix!r}; expected {allowed} "
            "(SVG is already a converted design and does not use this helper)")

    if output is None:
        output = source.with_suffix(".svg")
    else:
        output = Path(output).expanduser()
    if output.exists() or output.is_symlink():
        if output.is_symlink():
            raise ConversionError(f"output must not be a symlink: {output}")
        if not output.is_file():
            raise ConversionError(f"output is not a regular file: {output}")
    output = output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    if provenance is None:
        provenance = Path(str(output) + ".provenance.json")
    else:
        provenance = Path(provenance).expanduser().resolve()
    if provenance.exists() and provenance.is_symlink():
        raise ConversionError(f"provenance must not be a symlink: {provenance}")
    provenance.parent.mkdir(parents=True, exist_ok=True)

    tool = _converter()
    version = _version(tool)
    source_hash = sha256(source)

    fd, temporary_name = tempfile.mkstemp(
        prefix=f".{output.stem}.", suffix=".svg", dir=str(output.parent))
    os.close(fd)
    temporary = Path(temporary_name)
    try:
        # Restrict conversion to one artboard/page.  ``pdftocairo`` writes an
        # exact output path for SVG when one page is selected; ``-singlefile``
        # is intentionally omitted because Poppler only supports that option
        # for raster output formats.
        command = [
            str(tool), "-svg", "-f", str(page), "-l", str(page),
            str(source), str(temporary),
        ]
        try:
            result = subprocess.run(
                command,
                capture_output=True,
                text=True,
                check=False,
                timeout=timeout,
            )
        except subprocess.TimeoutExpired as exc:
            raise ConversionError(
                f"pdftocairo timed out after {timeout}s") from exc
        except OSError as exc:
            raise ConversionError(f"could not run pdftocairo: {exc}") from exc
        if result.returncode != 0:
            detail = (result.stderr or result.stdout).strip().splitlines()
            suffix_text = f": {detail[-1][:300]}" if detail else ""
            raise ConversionError(
                f"pdftocairo failed with exit {result.returncode}{suffix_text}")
        if not temporary.exists() or temporary.stat().st_size == 0:
            raise ConversionError("pdftocairo produced no SVG output")
        if not _looks_like_svg(temporary):
            raise ConversionError("pdftocairo output is not a valid-looking SVG")
        os.replace(temporary, output)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass

    receipt = {
        "schema": SCHEMA,
        "source": {
            "path": str(source),
            "format": suffix[1:],
            "sha256": source_hash,
            "bytes": source.stat().st_size,
        },
        "output": {
            "path": str(output),
            "format": "svg",
            "sha256": sha256(output),
            "bytes": output.stat().st_size,
        },
        "converter": {
            "name": "pdftocairo",
            "path": str(tool),
            "version": version,
            "page": page,
        },
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
    }
    temporary_receipt = provenance.with_name(f".{provenance.name}.tmp")
    temporary_receipt.write_text(
        json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(temporary_receipt, provenance)
    return receipt


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Convert PDF-compatible Illustrator artwork to SVG with a hash receipt")
    parser.add_argument("source", type=Path, help="PDF-compatible .ai or .pdf input")
    parser.add_argument("-o", "--output", type=Path, help="SVG output path (default: alongside input)")
    parser.add_argument("--provenance", type=Path, help="JSON receipt path (default: <output>.provenance.json)")
    parser.add_argument("--page", type=int, default=1, help="1-based PDF page/artboard (default: 1)")
    parser.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT_SECONDS, help="converter timeout in seconds")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        receipt = convert(
            args.source,
            args.output,
            args.provenance,
            page=args.page,
            timeout=args.timeout,
        )
    except ConversionError as exc:
        print(f"illustrator-to-svg: error: {exc}", file=os.sys.stderr)
        return 2
    print(json.dumps(receipt, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
