#!/usr/bin/env python3
"""Download and verify the pinned self-contained Node runtime used by design import."""

from __future__ import annotations

import argparse
import hashlib
import platform
import shutil
import tarfile
import tempfile
import urllib.request
import zipfile
from pathlib import Path


VERSION = "22.23.2"
ARTIFACTS = {
    "darwin-arm64": ("node-v22.23.2-darwin-arm64.tar.gz", "61130f394c1630d211dd50aecc4353d379480f36d3ac913cd85dbba1aed585c6"),
    "darwin-x64": ("node-v22.23.2-darwin-x64.tar.gz", "58e99022c2ff89395576cc7fd4d98cea24bb68081475d5f88b801ee8729fb026"),
    "linux-arm64": ("node-v22.23.2-linux-arm64.tar.gz", "013b59cfd2819703a6f4a14ab891fc46fc2a4e3f5bcd92de3fb4929b43e35b30"),
    "linux-x64": ("node-v22.23.2-linux-x64.tar.gz", "b294a556e639d64338823920e5866c21c02741742d2e1529ee1a225c1ec9252a"),
    "windows-arm64": ("node-v22.23.2-win-arm64.zip", "fec025a6da31757e3b6af84c5a1628e9d38442ca99a2161091d78f2fcfa35ef3"),
    "windows-x64": ("node-v22.23.2-win-x64.zip", "1177b4137ba5adaa56354ae40f1080c7450e8ae09cecb47da459d1c52ac99f97"),
}


def host_platform() -> str:
    system = {"Darwin": "darwin", "Linux": "linux", "Windows": "windows"}[platform.system()]
    machine = platform.machine().lower()
    arch = "arm64" if machine in {"arm64", "aarch64"} else "x64"
    return f"{system}-{arch}"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def prepare(target: str, output: Path, archive: Path | None = None) -> Path:
    name, expected = ARTIFACTS[target]
    with tempfile.TemporaryDirectory() as td:
        temp = Path(td)
        payload = temp / name
        if archive:
            shutil.copy2(archive, payload)
        else:
            urllib.request.urlretrieve(f"https://nodejs.org/dist/v{VERSION}/{name}", payload)
        actual = sha256(payload)
        if actual != expected:
            raise RuntimeError(f"Node archive checksum mismatch: expected {expected}, got {actual}")
        extracted = temp / "extracted"
        extracted.mkdir()
        if name.endswith(".zip"):
            with zipfile.ZipFile(payload) as bundle:
                bundle.extractall(extracted)
            candidate = next(extracted.glob("*/node.exe"))
        else:
            with tarfile.open(payload) as bundle:
                bundle.extractall(extracted, filter="data")
            candidate = next(extracted.glob("*/bin/node"))
        license_file = next(extracted.glob("*/LICENSE"))
        output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(candidate, output)
        shutil.copy2(license_file, output.with_name("node.LICENSE"))
        if target.startswith(("darwin-", "linux-")):
            output.chmod(0o755)
    return output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", choices=sorted(ARTIFACTS), default=host_platform())
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--archive", type=Path, help="Use an already-downloaded archive (tests/offline builds).")
    args = parser.parse_args()
    print(prepare(args.platform, args.output, args.archive))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
