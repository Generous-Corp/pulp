#!/usr/bin/env python3
"""Fetch the VCV Rack SDK, so nobody has to go and find it.

Forge Modular compiles real Rack modules, which needs Rack's SDK. We are not
permitted to redistribute it -- it is GPLv3 and VCV's to ship -- but fetching
it on someone's behalf is a different act from redistributing it, and it is
the same thing rustup does for a toolchain or Android Studio for the NDK. The
build system's refusal to vendor the SDK stays exactly as it is; this only
removes the errand.

THIS FILE IS THE ONE RESOLVER. generate.py and patch.py both take the SDK's
location from installed_at()/DEST rather than keeping paths of their own --
three components once held three different answers ("~/SDKs/Rack-SDK", this
DEST, and an "sdk/" sibling of it), so an SDK one of them fetched was an SDK
the others could not see, and a fresh machine died on a manual-download
message. A new consumer must import this module, never restate a path.

The fetch runs when the first module build needs it (generate.py calls
ensure()), gated by the auto_fetch_sdk setting. The installer's licence text
is the acknowledgement: it is not paperwork for the download, it is there
because obligations transfer to whoever distributes a module they built, and
nobody would guess them. Give a module away and any licence will do. Sell one
and it needs a commercial licence from VCV, which is included free if sold
through the VCV Library.

    fetch_sdk.py                 # fetch if missing, print where it landed
    fetch_sdk.py --check         # report only, fetch nothing
    fetch_sdk.py --licence       # the text the installer must show
"""
from __future__ import annotations

import hashlib
import io
import os
import platform
import shutil
import stat
import sys
import tempfile

SDK_VERSION = "2.6.6"
BASE = "https://vcvrack.com/downloads"
SDK_SHA256 = {
    "mac-arm64": "29414e52417992cbafa47e30f947c3c0c7a34e5c424bb83c5a0af8c24840481f",
    "mac-x64": "9b8b0d7582ca25fac879f8f64de40f2481df2fd903b65f3abb5d6801689060ec",
    "lin-x64": "420da2452def7b195f98e3a0650c35f1e0f058a98a71840b4347d3d203618532",
    "win-x64": "12f2043311db98592bdf73a7797e8a957c47950c3c8c17c7ff6421eb0015a05c",
}

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


def compiler_missing() -> str | None:
    """Why modules cannot be compiled here, or None when they can.

    The compiler is Apple's (or the distro's) and we do not redistribute it
    either. A missing toolchain must surface as a named, actionable message,
    never a blank failure -- this product once showed a bare traceback where
    this sentence belonged.
    """
    if sys.platform == "darwin":
        import subprocess
        r = subprocess.run(["xcode-select", "-p"], capture_output=True)
        if r.returncode == 0:
            return None
        # NOT "patch generation does not need them", which is what this said
        # and is false on macOS: /usr/bin/python3, /usr/bin/clang++ and
        # /usr/bin/git are one shared xcode-select shim binary, so without the
        # Command Line Tools there is no python3 to run the generator at all.
        # Telling someone a step is optional when nothing works without it
        # sends them looking for a different problem.
        return ("Apple's Command Line Tools are not installed. Run:  "
                "xcode-select --install  and try again. They are needed for "
                "patches as well as modules: on macOS the python3 that runs "
                "the generator is part of them.")
    import shutil
    if shutil.which("clang++") or shutil.which("g++"):
        return None
    return ("no C++ compiler was found, so modules cannot be compiled. "
            "Install clang or g++ and try again. Patch generation does not "
            "need one.")


def ensure(may_fetch: bool = True, announce=None) -> str:
    """The SDK directory, fetched first when it is absent and permitted.

    The only errors are real ones: an unsupported platform, a failed
    download, or auto-fetch switched off while nothing is installed. A
    merely-missing SDK on a machine that permits fetching is an errand, and
    this runs it, saying so through `announce` before the wait starts.
    """
    have = installed_at()
    if have:
        return have
    if not may_fetch:
        raise SystemExit(
            "the Rack SDK is not installed, and auto_fetch_sdk is switched "
            "off in Forge Modular's settings. Switch it back on, or set "
            "RACK_SDK_DIR to an SDK you have already downloaded.")
    if announce:
        announce(f"the Rack SDK {SDK_VERSION} is not installed yet. "
                 f"Fetching it from vcvrack.com (about 40 MB) into {DEST}. "
                 "It is VCV's, GPLv3 and free; your machine downloads it, "
                 "we never ship it.")
    return fetch(quiet=announce is None)


def fetch(quiet: bool = True) -> str:
    """Download and unpack. Returns the directory, or raises with a reason."""
    import urllib.request

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

    key = platform_key()
    expected = SDK_SHA256.get(key or "")
    if not expected:
        raise SystemExit(f"no pinned Rack SDK digest exists for {key}")
    return _install_archive(blob, DEST, expected)


def _install_archive(blob: bytes, dest: str, expected_digest: str) -> str:
    """Validate one SDK ZIP and replace only ``dest`` from isolated staging."""
    import zipfile

    digest = hashlib.sha256(blob).hexdigest()
    if digest != expected_digest:
        raise SystemExit(
            "the Rack SDK download did not match the digest pinned for this "
            f"release (got {digest}, expected {expected_digest})")

    parent = os.path.dirname(dest)
    os.makedirs(parent, exist_ok=True)
    try:
        with tempfile.TemporaryDirectory(prefix=".rack-sdk-stage-",
                                         dir=parent) as stage:
            with zipfile.ZipFile(io.BytesIO(blob)) as z:
                for member in z.infolist():
                    name = member.filename
                    parts = name.split("/")
                    mode = member.external_attr >> 16
                    file_type = stat.S_IFMT(mode)
                    unsafe_type = file_type and not (stat.S_ISREG(mode) or
                                                     stat.S_ISDIR(mode))
                    if ("\\" in name or name.startswith("/") or
                            not parts or parts[0] != "Rack-SDK" or
                            any(p in ("", ".", "..") for p in parts[:-1]) or
                            unsafe_type):
                        raise ValueError(f"unsafe archive member: {name!r}")
                z.extractall(stage)

            staged_sdk = os.path.join(stage, "Rack-SDK")
            if not os.path.isfile(os.path.join(staged_sdk, "include", "rack.hpp")):
                raise ValueError("Rack-SDK/include/rack.hpp is missing")

            backup_root = None
            try:
                if os.path.lexists(dest):
                    backup_root = tempfile.mkdtemp(prefix=".rack-sdk-old-",
                                                   dir=parent)
                    os.replace(dest, os.path.join(backup_root, "Rack-SDK"))
                os.replace(staged_sdk, dest)
            except Exception:
                if backup_root and not os.path.lexists(dest):
                    os.replace(os.path.join(backup_root, "Rack-SDK"), dest)
                raise
            finally:
                if backup_root:
                    shutil.rmtree(backup_root, ignore_errors=True)
    except Exception as e:
        raise SystemExit(f"the Rack SDK archive could not be unpacked: {e}")
    return dest


def main(argv):
    if "--licence" in argv or "--license" in argv:
        print(LICENCE, end="")
        return 0

    have = installed_at()
    if "--check" in argv:
        u = url()
        problem = compiler_missing()
        print(f"platform   : {platform_key() or 'unsupported'}")
        print(f"would fetch: {u or '(nothing available)'}")
        print(f"installed  : {have or 'no'}")
        print(f"compiler   : {'present' if problem is None else problem}")
        return 0 if have and problem is None else 1

    where = fetch(quiet="--verbose" not in argv)
    print(where)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
