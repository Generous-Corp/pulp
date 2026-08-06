#!/usr/bin/env python3
"""Fetch real, shared VCV patches, to find out what our gate wrongly rejects.

Not a source of good patches. A shared patch is not ground truth for quality,
and most of them use modules nobody here has installed.

It is a FALSE-REJECTION DETECTOR. Patches that humans built, played and shared
are patches that should overwhelmingly pass. Every one our gate rejects is a
candidate defect -- and a corpus surfaces them together, in one sweep, instead
of one at a time from whichever run happened to fail. Five gate defects were
found that way in a single day, each costing an investigation.

    patch_corpus.py fetch [N]     # add up to N patches to the corpus
    patch_corpus.py status        # what is held, and under which licences

TERMS. Patchstorage publishes no Terms of Service -- there is no such page and
none is linked. What governs instead:

  * `robots.txt` is `User-agent: * / Crawl-delay: 10` with NO Disallow, so
    automated access is permitted and the delay is honoured here to the second.
  * The Beta API is public and documented for querying.
  * Every patch carries its OWN licence, which the index records. That is a
    statement from the person who holds the rights, which is worth more than a
    site-wide policy: it travels with each patch and can be filtered on.

We analyse; we do not redistribute. Nothing fetched here is ever packaged: the
corpus lives beside the reference library under Application Support, outside
the repository, because a `.gitignore` has no authority over a file copy and
113 MB of somebody else's books reached a signed installer that way once.
"""

from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request

API = "https://patchstorage.com/api/beta"
VCV_PLATFORM = 745

# Their robots.txt asks for ten seconds between requests. It is the only rate
# statement they publish, so it is the one we keep -- including on the API,
# where we could argue a different norm applies and would rather not.
CRAWL_DELAY = float(os.environ.get("FORGE_CORPUS_DELAY", "10"))

# Identifying, with a way to be told to stop. An anonymous scraper is the
# thing a site defends itself against; a named one can be asked to leave.
UA = ("pulp-forge-research/1.0 (VCV patch analysis, not redistribution; "
      "contact daniel.raffel@gmail.com)")

ROOT = os.path.expanduser(
    "~/Library/Application Support/Forge Modular/corpus/patchstorage")
INDEX = os.path.join(ROOT, "index.json")
PATCH_DIR = os.path.join(ROOT, "patches")

_last_request = 0.0


def _get(url: str) -> bytes:
    """One request, never sooner than the crawl delay since the last."""
    global _last_request
    wait = CRAWL_DELAY - (time.time() - _last_request)
    if wait > 0:
        time.sleep(wait)
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.read()
    finally:
        _last_request = time.time()


ZSTD_MAGIC = b"\x28\xb5\x2f\xfd"


def read_patch(path: str) -> dict | None:
    """The patch inside a `.vcv`, which is not itself JSON.

    Rack 2 writes a `.vcv` as a **zstd-compressed tar** holding `patch.json`
    and a `modules/` directory of per-module state. Reading one with
    `json.load` raises, and a sweep that treats that as "this patch did not
    parse" reports an empty corpus while the files sit on disk perfectly
    intact -- which is what it did.

    Rack 1 wrote plain JSON with the same extension, so both are accepted:
    the magic number decides, rather than the version in a name.

    Shelled out to `tar` because macOS's bsdtar reads zstd natively and the
    stdlib did not gain a zstd module until 3.14; adding a dependency to read
    an archive the OS already reads would be a worse trade.
    """
    try:
        with open(path, "rb") as f:
            head = f.read(4)
    except OSError:
        return None
    if head != ZSTD_MAGIC:
        try:
            with open(path) as f:
                return json.load(f)
        except (OSError, ValueError):
            return None
    try:
        done = subprocess.run(["tar", "--zstd", "-xOf", path, "patch.json"],
                              capture_output=True, timeout=60)
    except (OSError, subprocess.SubprocessError):
        return None
    if done.returncode != 0 or not done.stdout:
        return None
    try:
        return json.loads(done.stdout)
    except ValueError:
        return None


def load_index() -> dict:
    if not os.path.exists(INDEX):
        return {"source": "patchstorage.com", "patches": {}}
    with open(INDEX) as f:
        return json.load(f)


def save_index(index: dict) -> None:
    os.makedirs(ROOT, exist_ok=True)
    tmp = INDEX + ".part"
    with open(tmp, "w") as f:
        json.dump(index, f, indent=2, sort_keys=True)
    os.replace(tmp, INDEX)


def listing(page: int, per_page: int = 100) -> list:
    url = (f"{API}/patches?platforms={VCV_PLATFORM}"
           f"&per_page={per_page}&page={page}")
    return json.loads(_get(url))


def detail(patch_id: int) -> dict:
    return json.loads(_get(f"{API}/patches/{patch_id}"))


def fetch(limit: int) -> int:
    """Add up to `limit` patches. Resumable: what is held is never refetched."""
    index = load_index()
    held = index["patches"]
    os.makedirs(PATCH_DIR, exist_ok=True)

    added = 0
    page = 1
    while added < limit:
        try:
            batch = listing(page)
        except urllib.error.HTTPError as e:              # noqa: PERF203
            print(f"  listing page {page}: HTTP {e.code} — stopping")
            break
        if not batch:
            break
        for row in batch:
            if added >= limit:
                break
            pid = str(row.get("id"))
            if pid in held:
                continue
            try:
                doc = detail(int(pid))
            except urllib.error.HTTPError as e:
                print(f"  patch {pid}: HTTP {e.code} — skipped")
                continue

            files = [f for f in (doc.get("files") or [])
                     if str(f.get("filename", "")).endswith(".vcv")]
            if not files:
                # Many entries are a screenshot or a zip; only a .vcv is a
                # patch we can read, and recording the miss stops us asking
                # again on the next run.
                held[pid] = {"skipped": "no .vcv file",
                             "url": doc.get("url", "")}
                continue

            f0 = files[0]
            try:
                blob = _get(f0["url"])
            except urllib.error.HTTPError as e:
                print(f"  patch {pid} file: HTTP {e.code} — skipped")
                continue

            name = f"{pid}.vcv"
            with open(os.path.join(PATCH_DIR, name), "wb") as out:
                out.write(blob)

            lic = doc.get("license") or {}
            held[pid] = {
                "id": doc.get("id"),
                "title": doc.get("title", ""),
                "url": doc.get("url", ""),
                "author": (doc.get("author") or {}).get("name", ""),
                # Provenance, per patch. Anything derived from this corpus can
                # name where it came from and under what terms, and anything
                # whose licence turns out not to permit a use can be found and
                # dropped without re-fetching the world.
                "license": lic.get("name", ""),
                "license_slug": lic.get("slug", ""),
                "excerpt": (doc.get("excerpt") or "")[:400],
                "tags": [t.get("name") for t in (doc.get("tags") or [])],
                "categories": [c.get("name") for c in (doc.get("categories") or [])],
                "file": name,
                "sha256": hashlib.sha256(blob).hexdigest(),
                "bytes": len(blob),
                "fetched_at": time.strftime("%Y-%m-%dT%H:%M:%S"),
            }
            added += 1
            print(f"  [{added}/{limit}] {doc.get('title','')[:48]:<48} "
                  f"{lic.get('slug','?')}", flush=True)
            save_index(index)                    # resumable after any stop
        page += 1
    save_index(index)
    return added


def status() -> None:
    index = load_index()
    held = index["patches"]
    real = {k: v for k, v in held.items() if "file" in v}
    print(f"corpus: {ROOT}")
    print(f"  patches held: {len(real)}   (entries skipped: "
          f"{len(held) - len(real)})")
    if not real:
        return
    by_licence: dict[str, int] = {}
    total = 0
    for v in real.values():
        by_licence[v.get("license") or "(none stated)"] = \
            by_licence.get(v.get("license") or "(none stated)", 0) + 1
        total += v.get("bytes", 0)
    print(f"  bytes on disk: {total / 1e6:.1f} MB")
    print("  licences:")
    for name, n in sorted(by_licence.items(), key=lambda kv: -kv[1]):
        print(f"    {n:4d}  {name}")


def main(argv: list[str]) -> int:
    args = argv[1:]
    if not args or args[0] not in ("fetch", "status"):
        print(__doc__.strip().split("\n\n")[2])
        return 2
    if args[0] == "status":
        status()
        return 0
    limit = int(args[1]) if len(args) > 1 else 50
    print(f"fetching up to {limit} VCV patches, {CRAWL_DELAY:g}s apart "
          f"(~{limit * CRAWL_DELAY * 2 / 60:.0f} min)", flush=True)
    added = fetch(limit)
    print(f"\nadded {added}")
    status()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
