#!/usr/bin/env python3
"""Fetch the VCV Rack SDK, so nobody has to go and find it.

Forge Modular compiles real Rack modules, which needs Rack's SDK. We are not
permitted to redistribute it -- it is GPLv3 and VCV's to ship -- but fetching
it on someone's behalf is a different act from redistributing it, and it is
the same thing rustup does for a toolchain or Android Studio for the NDK. The
build system's refusal to vendor the SDK stays exactly as it is; this only
removes the errand.

So it runs during install, silently, and the only thing a person sees is one
licence acknowledgement. That acknowledgement is not paperwork for the
download: it is there because obligations transfer to whoever distributes a
module they built, and nobody would guess them. Give a module away and any
licence will do. Sell one and it needs a commercial licence from VCV, which is
included free if sold through the VCV Library.

    fetch_sdk.py                 # fetch if missing, print where it landed
    fetch_sdk.py --check         # report only, fetch nothing
    fetch_sdk.py --licence       # the text the installer must show
"""
from __future__ import annotations

import os
import platform
import sys

SDK_VERSION = "2.6.6"
BASE = "https://vcvrack.com/downloads"

# Where it lands. Deliberately outside any checkout: the SDK is GPLv3 and the
# build refuses to configure if it is found inside the source tree.
DEST = os.path.expanduser("~/Library/Application Support/Forge Modular/Rack-SDK") \
    if sys.platform == "darwin" else \
    os.path.expanduser("~/.local/share/forge-modular/Rack-SDK")

LICENCE = """\
Forge Modular builds real VCV Rack modules, so it needs the Rack SDK — about
40 MB, downloaded from vcvrack.com. It is free and open source (GPLv3), made
by VCV, not by us. We fetch it; we do not ship it.

Modules you build are yours. Give one away and it can carry any licence you
like. Sell one and it needs a commercial licence from VCV — which is included
free for anything sold through the VCV Library.
"""


def platform_key() -> str | None:
    """The SDK archive VCV publishes for this machine."""
    mach = platform.machine().lower()
    if sys.platform == "darwin":
        return "mac-arm64" if mach in ("arm64", "aarch64") else "mac-x64"
    if sys.platform.startswith("linux"):
        return "lin-x64" if mach in ("x86_64", "amd64") else None
    if sys.platform.startswith("win"):
        return "win-x64" if mach in ("amd64", "x86_64") else None
    return None


def url() -> str | None:
    key = platform_key()
    return f"{BASE}/Rack-SDK-{SDK_VERSION}-{key}.zip" if key else None


def installed_at() -> str | None:
    """An SDK we can build against, wherever it already is.

    Checks an explicit override and a developer's own copy before ours, so a
    machine that already has one is not made to download a second.
    """
    for cand in (os.environ.get("RACK_SDK_DIR"),
                 os.environ.get("PULP_RACK_SDK_DIR"),
                 DEST,
                 os.path.expanduser("~/SDKs/Rack-SDK")):
        if cand and os.path.exists(os.path.join(cand, "include", "rack.hpp")):
            return cand
    return None


def fetch(quiet: bool = True) -> str:
    """Download and unpack. Returns the directory, or raises with a reason."""
    import io
    import urllib.request
    import zipfile

    have = installed_at()
    if have:
        return have

    u = url()
    if not u:
        raise SystemExit(
            f"no Rack SDK is published for {sys.platform}/{platform.machine()}. "
            f"Rack supports macOS (arm64, x64), Linux x64 and Windows x64.")

    if not quiet:
        print(f"fetching {u}", file=sys.stderr)
    try:
        with urllib.request.urlopen(u, timeout=300) as r:
            blob = r.read()
    except Exception as e:
        raise SystemExit(f"could not download the Rack SDK: {e}")

    os.makedirs(os.path.dirname(DEST), exist_ok=True)
    try:
        with zipfile.ZipFile(io.BytesIO(blob)) as z:
            # The archive holds a single Rack-SDK/ directory; unpack its
            # contents into DEST so the path does not gain a level.
            z.extractall(os.path.dirname(DEST))
    except Exception as e:
        raise SystemExit(f"the Rack SDK archive could not be unpacked: {e}")

    if not os.path.exists(os.path.join(DEST, "include", "rack.hpp")):
        raise SystemExit(
            f"the SDK unpacked to {DEST} but has no include/rack.hpp — the "
            f"archive layout may have changed")
    return DEST


def main(argv):
    if "--licence" in argv or "--license" in argv:
        print(LICENCE, end="")
        return 0

    have = installed_at()
    if "--check" in argv:
        u = url()
        print(f"platform   : {platform_key() or 'unsupported'}")
        print(f"would fetch: {u or '(nothing available)'}")
        print(f"installed  : {have or 'no'}")
        return 0 if have else 1

    where = fetch(quiet="--verbose" not in argv)
    print(where)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
