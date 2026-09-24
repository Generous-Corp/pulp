#!/usr/bin/env python3
"""A reader of the shared module index never observes a partial file.

~/.cache/forge-modular is shared by every process on the machine, and two
harnesses started together both refresh it. The refresh used to write
modules.json in place, so the second process's json.load could race a
half-written file and fail with JSONDecodeError. Publishing must therefore be
atomic: while a refresh is mid-write, a concurrent reader sees either the
previous complete index or nothing, and afterwards the new one.

The race is reproduced deterministically rather than by timing: json.dump is
wrapped so that a probe runs after the first byte of the new payload has been
flushed, which is exactly the moment an in-place writer exposes a truncated
file. A wall-clock race would pass by luck on a fast disk.

    python3 tools/rack/test_module_index_atomic.py
"""
from __future__ import annotations

import io
import json
import os
import sys
import tarfile
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import patch as P  # noqa: E402


def _library_tarball(manifests: dict) -> bytes:
    """A VCV library archive holding one manifest per plugin slug."""
    blob = io.BytesIO()
    with tarfile.open(fileobj=blob, mode="w:gz") as tf:
        for slug, modules in manifests.items():
            payload = json.dumps({
                "slug": slug,
                "modules": [{"slug": m, "name": m, "tags": ["VCO"]} for m in modules],
            }).encode()
            info = tarfile.TarInfo(name=f"library-v2/manifests/{slug}.json")
            info.size = len(payload)
            tf.addfile(info, io.BytesIO(payload))
    return blob.getvalue()


class _Response(io.BytesIO):
    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False


def _run(cache_dir: str) -> list:
    failures: list = []
    previous = {"Old": {"Osc": {"name": "Osc", "tags": [], "description": ""}}}
    index_path = os.path.join(cache_dir, "modules.json")
    with open(index_path, "w", encoding="utf-8") as f:
        json.dump(previous, f)
    stale = time.time() - 30 * 86400
    os.utime(index_path, (stale, stale))

    fresh_tarball = _library_tarball({"Fundamental": ["VCO", "VCF"], "Bogaudio": ["LFO"]})
    observations: list = []

    def probe():
        # This runs while the new payload is partially written. An in-place
        # writer has already truncated modules.json by now.
        try:
            with open(index_path, encoding="utf-8") as f:
                observations.append(("ok", json.load(f)))
        except FileNotFoundError:
            observations.append(("missing", None))
        except json.JSONDecodeError as exc:
            observations.append(("partial", str(exc)))

    real_dump = json.dump

    def slow_dump(obj, fp, **kw):
        text = json.dumps(obj, **kw)
        fp.write(text[:1])
        fp.flush()
        probe()
        fp.write(text[1:])

    saved = (P.CACHE_DIR, P.MODULE_INDEX)
    import urllib.request
    real_urlopen = urllib.request.urlopen
    P.CACHE_DIR, P.MODULE_INDEX = cache_dir, index_path
    urllib.request.urlopen = lambda *a, **k: _Response(fresh_tarball)
    json.dump = slow_dump
    try:
        result = P.module_index()
    finally:
        json.dump = real_dump
        urllib.request.urlopen = real_urlopen
        P.CACHE_DIR, P.MODULE_INDEX = saved[0], saved[1]

    if observations != [("ok", previous)]:
        failures.append(f"mid-write reader saw {observations!r}, expected the previous "
                        f"complete index")
    if set(result) != {"Fundamental", "Bogaudio"} or set(result["Fundamental"]) != {"VCO", "VCF"}:
        failures.append(f"module_index returned {result!r}")
    with open(index_path, encoding="utf-8") as f:
        published = json.load(f)
    if published != result:
        failures.append("published index differs from the returned one")
    leftovers = sorted(n for n in os.listdir(cache_dir) if n != "modules.json")
    if leftovers:
        failures.append(f"temporary files left behind: {leftovers}")

    # The reader path is untouched: a fresh index is served from disk.
    def no_network(*a, **k):
        raise AssertionError("a fresh index must not be fetched again")

    P.CACHE_DIR, P.MODULE_INDEX = cache_dir, index_path
    urllib.request.urlopen = no_network
    try:
        if P.module_index() != result:
            failures.append("re-reading the fresh index did not return the published one")
    finally:
        urllib.request.urlopen = real_urlopen
        P.CACHE_DIR, P.MODULE_INDEX = saved[0], saved[1]
    return failures


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="forge-module-index-") as cache_dir:
        failures = _run(cache_dir)
    for line in failures:
        print(f"  FAIL  {line}")
    if failures:
        return 1
    print("  ok    module index is published atomically")
    return 0


if __name__ == "__main__":
    sys.exit(main())
