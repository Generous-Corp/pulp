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
    """The MODULE manifests. `_`-prefixed files are not modules."""
    d = os.path.join(pack, "modules")
    if not os.path.isdir(d):
        return {}
    return {f[:-5]: os.path.join(d, f)
            for f in os.listdir(d) if f.endswith(".json") and not f.startswith("_")}


def plugin_manifest(pack):
    """The pack's plugin-level manifest, or None.

    The emitter needs it: it is where the plugin's slug lives, and without one
    it refuses. `manifests()` filters `_`-prefixed files out because they are
    not modules -- correct for every other caller and wrong for the emit, which
    was handed 32 module manifests and no plugin, failed every time, and left
    the two copied modules in the tree with no panel, no plugin.json entry and
    no registration. A module the pack does not register is drift, and drift
    fails the WHOLE Rack plugin at load, not just that module.
    """
    p = os.path.join(pack, "modules", "_plugin.json")
    return p if os.path.exists(p) else None


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


def compiles(pack: str, srcs: list) -> tuple:
    """Do these sources compile against the Rack SDK? -> (ok, first-errors).

    The reason this exists: both modules this script had been advising be
    copied back, on every proof run for days, DO NOT COMPILE. HOLDEN has a
    syntax error; MONOVOICE calls `pulp::signal::VaOscillator` and `VaShape`,
    which do not exist. They are failed generations that were left in the
    installed pack, and copying them in registers them in plugin.json -- so
    the pack stops building and the whole Rack plugin fails to load, not just
    those two modules.

    A tool that recommends an action must know the action is safe. This one
    recommended it 30+ times.
    """
    import tempfile
    try:
        sys.path.insert(0, HERE)
        import generate as G                                # noqa: PLC0415
    except Exception as exc:                                # noqa: BLE001
        # Fail CLOSED. An unverifiable module is refused, not waved through:
        # the consequence of being wrong here is a pack that registers a
        # module it cannot build, which fails the WHOLE Rack plugin at load.
        # "I could not check" and "it is fine" are not the same answer.
        return False, [f"could not verify it compiles ({exc})"]
    # Compile against the pack the source LIVES in, not against the repo's.
    #
    # `_includes()` points at the repo's src/, whose generated_modules.hpp has
    # no Layout type for a module that is not in the repo yet — so every
    # candidate reported "no type named <SLUG>Layout" and the check condemned
    # good modules along with the broken ones. Swapping the pack's own src/ in
    # leaves only the errors that are really in the source.
    inc = [f"-I{pack}/src" if i == f"-I{G.PACK}/src" else i
           for i in G._includes()]
    errs = []
    with tempfile.TemporaryDirectory() as tmp:
        for src in srcs:
            path = os.path.join(pack, "src", src)
            if not os.path.exists(path):
                errs.append(f"{src}: missing")
                continue
            r = subprocess.run(
                ["clang++", "-std=c++20", "-O2", "-fPIC", "-fvisibility=hidden",
                 "-fsyntax-only", path, *inc, "-DARCH_MAC"],
                capture_output=True, text=True)
            if r.returncode != 0:
                first = [l for l in r.stderr.splitlines() if " error: " in l][:3]
                errs.append(f"{src}:\n" + "\n".join("      " + e.split("error: ")[-1]
                                                     for e in first))
    return (not errs), errs


def emit_all() -> bool:
    """Re-emit panels, plugin.json and the registration block. True on success."""
    mine = manifests(REPO_PACK)
    plug = plugin_manifest(REPO_PACK)
    if not plug:
        print(f"  FAILED — no plugin-level manifest in {REPO_PACK}/modules",
              file=sys.stderr)
        return False
    r = subprocess.run([sys.executable, os.path.join(HERE, "forge_modular.py"),
                        "all", plug, *sorted(mine.values())],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print("  FAILED — the manifests are copied but the panels are not:",
              file=sys.stderr)
        print((r.stderr or r.stdout)[-600:], file=sys.stderr)
        return False
    line = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else "done"
    print(f"  {line}")
    return True


def unemitted() -> list:
    """Modules in the checkout with no panel or no registration.

    Checked against what is on disk rather than against the emitter's exit
    code, because the emitter can decline to do the work and still exit 0 for
    other reasons -- and because the failure this catches is one where the
    sources are present and nothing builds them.
    """
    import json as _json
    # A pack with no plugin.json has never been emitted at all. That is a
    # different situation from "emitted, and these two were missed", and this
    # function has nothing useful to say about it — saying something anyway
    # made every fixture pack look broken.
    if not os.path.exists(os.path.join(REPO_PACK, "plugin.json")):
        return []
    out = []
    for stem, path in sorted(manifests(REPO_PACK).items()):
        try:
            doc = _json.load(open(path))
        except Exception:                                   # noqa: BLE001
            continue
        for mod in doc.get("modules") or []:
            slug = mod.get("slug")
            if not slug:
                continue
            panel = os.path.join(REPO_PACK, "res", f"{slug}.svg")
            registered = False
            pj = os.path.join(REPO_PACK, "plugin.json")
            if os.path.exists(pj):
                try:
                    registered = any(
                        m.get("slug") == slug
                        for m in (_json.load(open(pj)).get("modules") or []))
                except Exception:                           # noqa: BLE001
                    registered = False
            if not os.path.exists(panel) or not registered:
                out.append(slug)
    return out


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
        # Not simply "nothing to do". A previous run could have copied the
        # sources and failed to emit, and this early return then reported that
        # broken pack as healthy forever -- the copy is what makes the module
        # "present", and the check above only looks for presence.
        missing = unemitted()
        if not missing:
            print("the checkout has every module the installed pack does")
            return 0
        print("every module is here, but these have no panel or registration:")
        for slug in missing:
            print(f"  {slug}")
        print("\nre-emitting…")
        return 0 if emit_all() else 1

    print(f"{len(stems)} module(s) exist only in the installed pack:\n")
    copyable, broken = [], []
    for stem in stems:
        slug, src = source_for(INSTALLED_PACK, stem)
        if not slug:
            print(f"  {stem:12} SKIP  its manifest names no module")
            continue
        if not src:
            print(f"  {slug:12} SKIP  no src/{slug}.cpp beside the manifest")
            continue
        ok, why = compiles(INSTALLED_PACK, [f"{slug}.cpp"])
        if not ok:
            print(f"  {slug:12} WILL NOT COMPILE — refusing to copy it in:")
            for line in why:
                print(f"      {line}")
            broken.append(slug)
            continue
        print(f"  {slug:12} modules/{stem}.json + src/{slug}.cpp")
        copyable.append((stem, slug, src))

    if broken:
        print(f"\n{len(broken)} of these are failed generations: they are in the")
        print("installed pack because generating them is what put them there, not")
        print("because they work. Copying one in registers it in plugin.json and")
        print("the WHOLE Rack plugin then fails to load. Delete them from the")
        print("install when you are sure they are not wanted:")
        for slug in broken:
            print(f"  rm '{INSTALLED_PACK}/src/{slug}.cpp'")
    if not copyable:
        print("\nnothing here is safe to copy back.")
        return 1 if broken else 0
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
    if not emit_all():
        return 1
    still = unemitted()
    if still:
        # The exit code said the emitter was happy. Look at what it produced.
        print("  FAILED — the emitter reported success and these still have no",
              file=sys.stderr)
        print("  panel or registration: " + ", ".join(still), file=sys.stderr)
        return 1
    print("\nReview and commit them; nothing here does that for you.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
