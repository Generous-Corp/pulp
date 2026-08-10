#!/usr/bin/env python3
"""Is every module the @ list offers actually free?

    tools/rack/test_catalog_is_free.py

A row for an uninstalled module says "GET · FREE", and that is a claim about
somebody's money. It holds for one reason: library_catalog.py caches module
lists only for OPEN-SOURCE plugins, and of the 315 plugins VCV publishes, the
67 premium ones are none of them. So a module in the cache came from a free
plugin, and the label is a fact rather than an assumption.

Nothing enforced that. Cache one premium plugin's modules — reasonable, if its
source ever appeared — and every one of its rows would claim to be free.

Checked against the cache on this machine, offline. The upstream half (that no
premium plugin is open source) is checked only when --online is passed, since a
test that needs the network is a test that fails on a train.
"""

import json
import os
import sys

CACHE = os.path.expanduser(
    "~/Library/Application Support/Forge Modular/library/index.json")
bad = 0


def main():
    global bad
    if not os.path.exists(CACHE):
        print("  skip   no library cache here — run library_catalog.py fetch.")
        print("         This is a skip, not a pass.")
        return 0

    index = json.load(open(CACHE))
    withmods = {k: v for k, v in index.items() if v.get("modules")}
    print(f"  --     {len(index)} plugin(s) cached, {len(withmods)} with modules")

    # A cached entry must not be marked premium. The fetcher does not record
    # the flag today, so this also catches it starting to.
    paid = sorted(k for k, v in withmods.items()
                  if str(v.get("premium", "")).lower() == "true")
    if paid:
        print(f"  WRONG  {len(paid)} cached plugin(s) are premium, and every "
              f"module of theirs is offered as FREE: {paid[:5]}")
        bad += 1
    else:
        print(f"  ok     no cached plugin is premium, so FREE is a fact")

    # And every cached entry carries a licence, which is what "open source"
    # meant when it was fetched.
    unlicensed = sorted(k for k, v in withmods.items() if not v.get("license"))
    if unlicensed:
        print(f"  WRONG  {len(unlicensed)} cached plugin(s) have no licence "
              f"recorded — nothing says they are free: {unlicensed[:5]}")
        bad += 1
    else:
        print(f"  ok     every cached plugin records a licence")

    if "--online" in sys.argv:
        import urllib.request
        req = urllib.request.Request(
            "https://api.vcvrack.com/library/manifests",
            headers={"User-Agent": "forge-modular-catalog"})
        live = json.loads(urllib.request.urlopen(req, timeout=60)
                          .read().decode())["manifests"]
        both = [k for k, v in live.items()
                if str(v.get("premium", "")).lower() == "true"
                and str(v.get("openSource", "")).lower() == "true"]
        if both:
            print(f"  WRONG  upstream now has {len(both)} plugin(s) that are "
                  f"premium AND open source: {both[:4]}. The fetcher would "
                  f"cache their modules and the rows would say FREE.")
            bad += 1
        else:
            print(f"  ok     upstream: no premium plugin is open source")

    print("\n" + ("all good" if bad == 0 else "FAILED"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
