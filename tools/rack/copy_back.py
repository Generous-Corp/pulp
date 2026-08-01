#!/usr/bin/env python3
"""Bring modules the app generated on this machine back into the checkout.

    copy_back.py            # list what is only in the installed pack
    copy_back.py --apply    # copy those modules into the checkout

A module built from the app is written into the INSTALLED pack, under
`~/Library/Application Support/Forge Modular`. The checkout never sees it. Every
proof run has said so for as long as the check has existed --

    NOTE the module packs differ: 31 manifest(s) here, 33 installed.
         Modules generated on this machine live only in the
         installed pack until they are copied back.

-- and nothing in the repository could copy them back. A message naming an
action nobody can take is a message that gets read past, which is what happened:
HOLDEN and MONOVOICE sat in one machine's Application Support directory for a
day, outside version control, while a script mentioned them on every run.

The install is deliberately not the source of truth (install_toolchain.sh only
ever writes TOWARD it), so this is the one deliberate step in the other
direction and it is never automatic. Listing is the default; copying takes
--apply.

A module is four files -- the manifest, the source, and a light and dark panel.
The panels are generated FROM the manifest, so they are re-emitted rather than
copied: a panel that came back stale would look right and disagree with the
manifest it was drawn from.
"""

import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_PACK = os.path.normpath(os.path.join(HERE, "..", "..",
                                          "examples", "forge-modular"))
INSTALLED_PACK = os.path.expanduser(
    "~/Library/Application Support/Forge Modular/examples/forge-modular")


def manifests(pack):
    d = os.path.join(pack, "modules")
    if not os.path.isdir(d):
        return {}
    return {f[:-5]: os.path.join(d, f)
            for f in os.listdir(d) if f.endswith(".json") and not f.startswith("_")}


def only_installed():
    """Modules the installed pack has and the checkout does not."""
    mine, theirs = manifests(REPO_PACK), manifests(INSTALLED_PACK)
    return sorted(set(theirs) - set(mine))


def source_for(pack, stem):
    """A module's .cpp, which is named for the SLUG, not the manifest file.

    They are usually the same word in different cases, but reading the slug is
    the only way that stays true -- guessing at `stem.upper()` would silently
    skip a module whose manifest and slug disagree.
    """
    import json
    try:
        doc = json.load(open(manifests(pack)[stem]))
    except Exception:                                       # noqa: BLE001
        return None, None
    mods = doc.get("modules") or []
    if not mods:
        return None, None
    slug = mods[0].get("slug")
    if not slug:
        return None, None
    path = os.path.join(pack, "src", f"{slug}.cpp")
    return (slug, path if os.path.exists(path) else None)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--apply", action="store_true",
                    help="actually copy; without it, only report")
    args = ap.parse_args(argv[1:])

    if not os.path.isdir(INSTALLED_PACK):
        print(f"no installed pack at {INSTALLED_PACK}")
        print("nothing to copy back — this is a skip, not a success.")
        return 0

    stems = only_installed()
    if not stems:
        print("the checkout has every module the installed pack does")
        return 0

    print(f"{len(stems)} module(s) exist only in the installed pack:\n")
    copyable = []
    for stem in stems:
        slug, src = source_for(INSTALLED_PACK, stem)
        if not slug:
            print(f"  {stem:12} SKIP  its manifest names no module")
            continue
        if not src:
            print(f"  {slug:12} SKIP  no src/{slug}.cpp beside the manifest")
            continue
        print(f"  {slug:12} modules/{stem}.json + src/{slug}.cpp")
        copyable.append((stem, slug, src))

    if not args.apply:
        print(f"\nlisting only. Copy them with: {os.path.basename(__file__)} --apply")
        return 0

    for stem, slug, src in copyable:
        shutil.copy2(manifests(INSTALLED_PACK)[stem],
                     os.path.join(REPO_PACK, "modules", f"{stem}.json"))
        shutil.copy2(src, os.path.join(REPO_PACK, "src", f"{slug}.cpp"))
        print(f"  copied {slug}")

    # Panels are OUTPUTS. Re-emitting is what keeps a panel and its manifest
    # from disagreeing, and it also regenerates plugin.json and the registration
    # block, which a copied manifest alone would leave one module short.
    print("\nre-emitting panels and registrations from the manifests…")
    mine = manifests(REPO_PACK)
    r = subprocess.run([sys.executable, os.path.join(HERE, "forge_modular.py"),
                        "all", *sorted(mine.values())],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print("  FAILED — the manifests are copied but the panels are not:",
              file=sys.stderr)
        print((r.stderr or r.stdout)[-600:], file=sys.stderr)
        return 1
    print(f"  {r.stdout.strip().splitlines()[-1] if r.stdout.strip() else 'done'}")
    print("\nReview and commit them; nothing here does that for you.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
