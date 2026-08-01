#!/usr/bin/env python3
"""Build a local catalog of the whole VCV library, without owning any of it.

    library_catalog.py fetch     download and cache what is publicly available
    library_catalog.py report    what is covered, and what is missing

Two kinds of metadata matter here, and only one of them needs the module.

WHAT A MODULE DOES -- its name, description and tags -- is published. VCV's
library API lists every plugin, and 2 in 3 are open source with a source URL,
so their plugin.json (which carries every module's description and tags) can be
read straight from their repository. That is the layer learning tips are built
on, and it costs nothing: no subscription, no install, no Rack.

WHERE ITS CONTROLS ARE -- what can be wired to what, and where each knob and
jack sits -- is NOT published anywhere. It exists only in compiled widget code.
That is what CARTOG measures from inside Rack, and it is the only part that
requires actually having the module.

Keeping the two apart is the point. It means a patch can be described, tagged
and reasoned about for most of the library today, and only DRAWING faithfully
needs ownership.

Cached under ~/Library/Application Support/Forge Modular/library/, so a rebuild
costs one HTTP request per plugin that has changed version, and nothing at all
for the rest.
"""

import json
import os
import re
import sys
import time
import urllib.error
import urllib.request

MANIFESTS = "https://api.vcvrack.com/library/manifests"
CACHE = os.path.expanduser(
    "~/Library/Application Support/Forge Modular/library")
INDEX = os.path.join(CACHE, "index.json")
UA = "forge-modular-catalog (+https://vcvrack.com)"


def _get(url, timeout=30):
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode("utf-8", "replace")


def raw_urls(source_url):
    """Where a repo's plugin.json might live.

    Only GitHub and GitLab are tried by pattern. Anything else is left alone
    rather than guessed at -- a wrong URL that happens to return JSON would put
    somebody else's modules in the catalog under this plugin's name.
    """
    u = (source_url or "").rstrip("/").removesuffix(".git")
    m = re.match(r"https?://github\.com/([^/]+)/([^/]+)", u)
    if m:
        owner, repo = m.group(1), m.group(2)
        return [f"https://raw.githubusercontent.com/{owner}/{repo}/{b}/plugin.json"
                for b in ("master", "main")]
    m = re.match(r"https?://gitlab\.com/(.+)", u)
    if m:
        return [f"https://gitlab.com/{m.group(1)}/-/raw/{b}/plugin.json"
                for b in ("master", "main")]
    return []


def load_index():
    if os.path.exists(INDEX):
        try:
            return json.load(open(INDEX))
        except Exception:                                   # noqa: BLE001
            return {}
    return {}


def fetch(argv):
    limit = int(argv[0]) if argv else 0
    os.makedirs(CACHE, exist_ok=True)
    print("fetching the library manifest…")
    manifests = json.loads(_get(MANIFESTS))["manifests"]
    print(f"  {len(manifests)} plugins published")

    index = load_index()
    todo = []
    # Premium plugins are counted and not fetched, so the gap is a number
    # somebody can see rather than an absence. VCV publishes 67 of them and not
    # one is open source, so their MODULE lists exist nowhere public — the @
    # list can offer only what it can name, and every module it does offer is
    # therefore from a free plugin. That is what lets the row say "GET · FREE"
    # honestly; test_catalog_is_free.py holds it to that.
    premium = sum(1 for m in manifests.values()
                  if str(m.get("premium", "")).lower() == "true")
    if premium:
        print(f"  {premium} premium plugin(s) skipped — no public module list")

    for slug, m in sorted(manifests.items()):
        if str(m.get("openSource", "")).lower() != "true":
            continue
        if not raw_urls(m.get("sourceUrl")):
            continue
        # Version is the cache key. A plugin that has not been released since
        # we last looked cannot have changed its module list.
        have = index.get(slug, {})
        if have.get("version") == m.get("version") and have.get("modules"):
            continue
        todo.append((slug, m))
    if limit:
        todo = todo[:limit]
    print(f"  {len(todo)} to fetch ({len(index)} already cached)")

    ok = fail = 0
    for i, (slug, m) in enumerate(todo, 1):
        text = None
        for url in raw_urls(m["sourceUrl"]):
            try:
                text = _get(url)
                break
            except urllib.error.HTTPError:
                continue
            except Exception:                               # noqa: BLE001
                continue
        entry = {"version": m.get("version", ""),
                 "brand": m.get("brand") or m.get("name", slug),
                 "manualUrl": m.get("manualUrl", ""),
                 "sourceUrl": m.get("sourceUrl", ""),
                 "license": m.get("license", ""),
                 "modules": []}
        if text:
            try:
                doc = json.loads(text)
                for mod in doc.get("modules", []):
                    if "slug" not in mod:
                        continue
                    entry["modules"].append({
                        "slug": mod["slug"],
                        "name": mod.get("name", mod["slug"]),
                        "description": mod.get("description", ""),
                        "tags": mod.get("tags", []),
                        "manualUrl": mod.get("manualUrl", ""),
                    })
                ok += 1
            except Exception:                               # noqa: BLE001
                fail += 1
        else:
            fail += 1
        index[slug] = entry
        if i % 20 == 0 or i == len(todo):
            json.dump(index, open(INDEX, "w"), indent=1)
            print(f"    {i}/{len(todo)}  ok={ok} miss={fail}")
        # Polite: these are other people's servers and nothing here is urgent.
        time.sleep(0.15)

    json.dump(index, open(INDEX, "w"), indent=1)
    print(f"cached → {INDEX}")
    return 0


def report(_argv):
    index = load_index()
    if not index:
        print("nothing cached yet — run: library_catalog.py fetch")
        return 3
    plugins = len(index)
    with_mods = [v for v in index.values() if v["modules"]]
    modules = sum(len(v["modules"]) for v in index.values())
    described = sum(1 for v in index.values() for m in v["modules"]
                    if m["description"])
    tagged = sum(1 for v in index.values() for m in v["modules"] if m["tags"])
    print(f"  plugins cached      {plugins}")
    print(f"    with a module list {len(with_mods)}")
    print(f"  modules             {modules}")
    print(f"    with a description {described} ({100*described//max(modules,1)}%)")
    print(f"    with tags          {tagged} ({100*tagged//max(modules,1)}%)")

    # What this catalog CANNOT say, stated plainly. Descriptions and tags come
    # from a published manifest; positions do not exist in one.
    print()
    print("  none of these carry control positions — that is CARTOG's half,")
    print("  and it needs the module installed. This catalog is what a module")
    print("  IS; the port map is where its knobs and jacks are.")
    return 0


def main(argv):
    cmds = {"fetch": fetch, "report": report}
    if len(argv) < 2 or argv[1] not in cmds:
        print(__doc__)
        return 2
    return cmds[argv[1]](argv[2:])


if __name__ == "__main__":
    sys.exit(main(sys.argv))
