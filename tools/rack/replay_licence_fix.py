#!/usr/bin/env python3
"""Re-measure a corpus of patches with the licence fix on and off.

The audibility gate used to load third-party plugins without pointing
`rack::asset::userDir` at the cached VCV licence keys, so a commercially
licensed module resolved no key, decided it was unlicensed, and wrote zero to
every output. The gate called those patches silent. A one-off replay said the
fix recovered patches that had been rejected for that reason -- and left
nothing behind: no harness, no corpus, no results, and both binaries deleted.
A number nobody can re-run is not a measurement, so this file exists to
produce that number again on demand and to say what would have to be true for
it to mean anything.

Four measurements over the same patches, not one:

  prefix-a          the gate as it was before the fix
  prefix-b          the same binary again -- the NOISE FLOOR. The gate seeds
                    its RNG from the clock, so two runs of one binary over one
                    patch may disagree. Every flip below this floor is noise.
  fixed-no-user-dir the fixed binary invoked with no user directory, where the
                    fix is a no-op. ATTRIBUTION CONTROL: it must agree with
                    prefix-a, or the two binaries differ for some reason other
                    than licence resolution and the comparison is measuring
                    that instead.
  fixed             the fixed binary as the generator invokes it

and one more test on the result. MECHANISM CONTROL: a patch can only change
verdict because of this fix if it contains a plugin that HAS a cached licence
key. A patch built entirely from free modules that changes verdict is not a
recovery, it is drift -- and it is counted separately rather than folded in.

The corpus is Pulp's own patches only: the examples in this repository, the
fixtures beside the tests, and what the generator itself has written. The
crawled third-party patch corpus under `Forge Modular/corpus` is refused by
path, and the count of refusals is reported so the exclusion is visible rather
than assumed.

Absence is not success. With no SDK, no gate, or no patches this reports SKIP
and says it proves nothing.
"""

import argparse
import concurrent.futures
import functools
import hashlib
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import patch  # noqa: E402  (the resolvers for SDK, gate flags and plugin dir)

AUDIBLE = "audible"
SILENT = "silent"
UNMEASURED = "unmeasured"

#: Any path with one of these in it is somebody else's work. The crawled
#: corpus lives under `Forge Modular/corpus/patchstorage`; the reference
#: material lives under `Forge Modular/.corpus`. Neither may enter a corpus
#: this file measures, names, hashes, or writes down.
THIRD_PARTY_MARKERS = ("/corpus/patchstorage", "/.corpus/", "/corpus/")

#: The gate's own contract, mirrored from `patch.audibility`. 0 is audible and
#: 1 is silent; 2 is the gate refusing its own settings and a negative code is
#: a signal, and neither of those is a verdict about the patch.
GATE_AUDIBLE_RC = 0
GATE_REFUSED_RC = 2


def is_third_party(path: str) -> bool:
    """Whether a path lies in the crawled corpus of other people's patches."""
    real = os.path.realpath(path)
    return any(marker in real + "/" for marker in THIRD_PARTY_MARKERS)


def exclusion_control() -> tuple[bool, str]:
    """Prove the exclusion can refuse something, on this machine.

    The allowlist means the crawled corpus is never walked, so the refusal
    counter reads zero on a healthy run -- indistinguishable from a refusal
    that silently stopped working and let the corpus in. So the check is run
    directly against the crawled directory when this machine has one, and
    against nothing when it does not, and the run says which.
    """
    crawled = os.path.join(os.path.expanduser("~"), "Library",
                           "Application Support", "Forge Modular", "corpus")
    if not os.path.isdir(crawled):
        return True, ("no crawled corpus on this machine, so a refusal count "
                      "of zero says nothing either way")
    if not is_third_party(crawled):
        return False, f"{crawled} exists and was NOT refused"
    return True, "the crawled corpus on this machine is refused when asked"


# ---------------------------------------------------------------------------
# the two binaries
# ---------------------------------------------------------------------------

def licence_fix_commit(repo: str) -> str | None:
    """The commit that taught the gate to resolve licence keys.

    Found by content rather than named by hash, because a hash written down
    here stops resolving the first time this branch is rebased -- which is
    exactly what happened to the binaries this replay is reconstructing.
    """
    r = subprocess.run(
        ["git", "log", "--format=%H", "-S", "rack::asset::init()", "--",
         "tools/rack/patch_gate.cpp"],
        cwd=repo, capture_output=True, text=True)
    if r.returncode != 0:
        return None
    shas = [line.strip() for line in r.stdout.splitlines() if line.strip()]
    return shas[0] if shas else None


def _extract(repo: str, tree_ish: str, name: str, dest_dir: str) -> bool:
    r = subprocess.run(["git", "show", f"{tree_ish}:tools/rack/{name}"],
                       cwd=repo, capture_output=True, text=True)
    if r.returncode != 0:
        return False
    with open(os.path.join(dest_dir, name), "w") as f:
        f.write(r.stdout)
    return True


def materialize_gate_source(repo: str, tree_ish: str, dest_dir: str) -> bool:
    """The gate and the headers it includes, all from one commit.

    The headers come from the same commit as the source rather than from the
    working tree. They happen not to have moved across this range, but a
    replay that mixes an old measurement with today's thresholds is measuring
    the mixture.
    """
    os.makedirs(dest_dir, exist_ok=True)
    names = ["patch_gate.cpp", "patch_behaviour.hpp",
             "patch_behaviour_json.hpp"]
    return all(_extract(repo, tree_ish, n, dest_dir) for n in names)


def compile_gate(src_dir: str, out: str) -> tuple[str | None, str]:
    """Compile one gate variant with the flags `patch.build_gate` uses."""
    sdk = patch.SDK
    if not os.path.exists(os.path.join(sdk, "include", "rack.hpp")):
        return None, f"no Rack SDK at {sdk}"
    r = subprocess.run(
        ["clang++", "-std=c++20", "-O1", "-o", out,
         os.path.join(src_dir, "patch_gate.cpp"),
         f"-I{src_dir}", f"-I{sdk}/include", f"-I{sdk}/dep/include",
         "-DARCH_MAC", os.path.join(sdk, "libRack.dylib")],
        capture_output=True, text=True)
    if r.returncode == 0:
        return out, ""
    return None, "did not compile:\n" + (r.stderr or r.stdout).strip()


# ---------------------------------------------------------------------------
# the corpus
# ---------------------------------------------------------------------------

def _is_pulp_checkout(path: str) -> bool:
    return os.path.isfile(os.path.join(path, "tools", "rack", "patch.py"))


#: What the generator names an attempt it kept, and what the acid proof names
#: the two patches it derives. Both are written by this repository's own code;
#: nothing else puts a file of either shape in an attempts directory.
_ATTEMPT_VCV = re.compile(r"attempt\d\d-[a-z0-9-]+\.vcv$")
_ACID_DIR = re.compile(r"attempt\d\d-.*acid-proof$")


@functools.lru_cache(maxsize=None)
def _tracked_under(checkout: str, directory: str) -> frozenset[str]:
    """Every path git tracks under one directory, asked once.

    Cached per directory rather than per file: the sweep covers a few hundred
    sibling worktrees, and one `git ls-files` per candidate patch turns a
    minute of walking into an hour of process spawns.
    """
    r = subprocess.run(["git", "ls-files", "-z", "--", directory],
                       cwd=checkout, capture_output=True, text=True)
    if r.returncode != 0:
        return frozenset()
    return frozenset(
        os.path.realpath(os.path.join(checkout, rel))
        for rel in r.stdout.split("\0") if rel)


def _tracked_in(checkout: str, path: str) -> bool:
    return os.path.realpath(path) in _tracked_under(
        checkout, os.path.dirname(path))


def provenance(path: str, root_label: str, checkout: str | None) -> str | None:
    """Why this file is ours, or None -- in which case it is not measured.

    Positive establishment, never "it was in a directory I like". A patch
    somebody downloaded and dropped in the generator's output folder is
    indistinguishable from a generated one by content -- both are Rack 2
    documents with modules and cables and no author field anywhere. So each
    source has to produce its own evidence:

      a checkout      the file is TRACKED. Being in the repository's history
                      is the strongest claim available and excludes anything
                      merely sitting in the directory.
      generated       the generator's own sidecar beside it: the `why.json`
                      it writes for a patch it kept, the attempt artifacts it
                      writes for one it rejected, the acid proof's own pair.
      a project       the app's `project.json`, and only when it says the
                      project was neither remixed nor opened read-only from
                      the marketplace and carries the conversation it was
                      built in.

    Losing a patch to this costs sample size. Admitting somebody else's costs
    much more, so the tie goes to exclusion.
    """
    directory, name = os.path.split(path)
    if root_label.startswith(("repo/", "worktree:")):
        if checkout and _tracked_in(checkout, path):
            return "tracked in a Pulp checkout"
        return None
    if root_label == "generated/patches":
        if name.endswith(".vcv") and os.path.exists(
                os.path.join(directory, name[:-len(".vcv")] + ".why.json")):
            return "kept by the generator, with the why.json it wrote"
        return None
    if root_label == "generated/attempts":
        if _ATTEMPT_VCV.search(name):
            stem = name[:len("attemptNN")]
            siblings = [f for f in os.listdir(directory)
                        if f.startswith(stem) and f != name]
            if siblings:
                return "rejected by the generator, with its attempt artifacts"
            return None
        if _ACID_DIR.search(os.path.basename(directory)) and \
                name.startswith("acid-"):
            return "written by the acid proof inside a generator attempt"
        return None
    if root_label == "generated/projects":
        meta = os.path.join(directory, "project.json")
        if name != "patch.vcv" or not os.path.exists(meta):
            return None
        try:
            doc = json.load(open(meta))
        except Exception:                                      # noqa: BLE001
            return None
        if doc.get("remixed_from") or doc.get("marketplace_read_only"):
            return None
        if not doc.get("chat_history"):
            return None
        return "built in this app, from its own conversation"
    return None


def corpus_roots(repo: str) -> list[tuple[str, str, str | None]]:
    """(label, directory, owning checkout) for every place our patches land.

    An allowlist, not a filter over everything on the disk. A directory has to
    be named here to be read at all, so a corpus somebody drops on this
    machine later cannot wander in. The checkout is carried along because a
    file's provenance in a repository root is decided by asking git.
    """
    roots: list[tuple[str, str, str | None]] = []

    def add(label: str, path: str, checkout: str | None = None) -> None:
        if path and os.path.isdir(path) and not is_third_party(path):
            roots.append((label, os.path.realpath(path), checkout))

    add("repo/examples", os.path.join(repo, "examples/forge-modular/patches"),
        repo)
    add("repo/fixtures", os.path.join(repo, "tools/rack/fixtures"), repo)
    add("repo/test-fixtures", os.path.join(repo, "tools/rack/test_fixtures"),
        repo)

    # Sibling worktrees hold the same examples at other revisions plus
    # fixtures a branch added and main has not taken yet.
    siblings = os.environ.get("PULP_WORKTREES_ROOT") or os.path.dirname(repo)
    if os.path.isdir(siblings):
        for name in sorted(os.listdir(siblings)):
            tree = os.path.join(siblings, name)
            if tree == repo or not _is_pulp_checkout(tree):
                continue
            add(f"worktree:{name}/examples",
                os.path.join(tree, "examples/forge-modular/patches"), tree)
            add(f"worktree:{name}/fixtures",
                os.path.join(tree, "tools/rack/fixtures"), tree)
            add(f"worktree:{name}/test-fixtures",
                os.path.join(tree, "tools/rack/test_fixtures"), tree)

    # What the generator itself wrote: the patches it kept, the attempts it
    # rejected, and the projects built in the app. The rejected ones are the
    # point -- a fix that recovers a patch recovers it from that pile.
    try:
        generated = os.path.dirname(patch.user_patches_dir())
        add("generated/patches", patch.user_patches_dir())
        add("generated/attempts", os.path.join(generated, "attempts"))
        add("generated/projects", os.path.join(generated, "projects"))
    except Exception:                                          # noqa: BLE001
        pass

    named = os.environ.get("FORGE_ATTEMPT_DIR")
    if named:
        add("generated/named-attempt-dir", named)
    return roots


def collect_corpus(
        roots: list[tuple[str, str, str | None]]) -> tuple[dict, int, int]:
    """sha256 -> entry, plus refusals and provenance exclusions.

    Content-deduplicated, because the same example file exists once per
    worktree and counting it thirty times would inflate every cell of the
    matrix at once.

    Both reject counts are returned rather than discarded. Zero refusals on a
    machine that has the crawled corpus installed means the exclusion never
    ran, which looks exactly like an exclusion that had nothing to exclude;
    zero provenance exclusions across directories that are known to hold
    unexplained files means the same about that gate.
    """
    seen: dict[str, dict] = {}
    refused = 0
    unestablished = 0
    for label, root, checkout in roots:
        for dirpath, _dirs, files in os.walk(root):
            if is_third_party(dirpath):
                refused += 1
                continue
            for name in sorted(files):
                if not name.endswith(".vcv"):
                    continue
                full = os.path.join(dirpath, name)
                if is_third_party(full):
                    refused += 1
                    continue
                why = provenance(full, label, checkout)
                if why is None:
                    unestablished += 1
                    continue
                try:
                    blob = open(full, "rb").read()
                except OSError:
                    continue
                digest = hashlib.sha256(blob).hexdigest()
                if digest not in seen:
                    seen[digest] = {
                        "root": label,
                        "path": os.path.relpath(full, root),
                        "full": full,
                        "provenance": why,
                    }
    return seen, refused, unestablished


def licensed_plugin_slugs() -> set[str]:
    """The plugins this machine holds a cached VCV key for.

    Read live rather than committed. Which plugins somebody has bought is a
    property of the machine, and writing it into the repository would both
    date instantly and say more about the owner than the fix.
    """
    for base in ("~/Library/Application Support/Rack2", "~/.local/share/Rack2",
                 "~/.Rack2"):
        d = os.path.join(os.path.expanduser(base), "licenses")
        if os.path.isdir(d):
            return {n[:-len(".vcvkey")] for n in os.listdir(d)
                    if n.endswith(".vcvkey")}
    return set()


def patch_plugins(path: str) -> set[str]:
    try:
        doc = json.load(open(path))
    except Exception:
        return set()
    return {m.get("plugin") for m in doc.get("modules", []) or []
            if m.get("plugin")}


def is_licence_bearing(plugins: set[str], licensed: set[str]) -> bool:
    """Whether the fix could possibly change this patch's verdict."""
    return bool(plugins & licensed)


# ---------------------------------------------------------------------------
# running one variant over one patch
# ---------------------------------------------------------------------------

def run_gate(binary: str, patch_path: str, plugin_dir: str,
             user_dir: str | None, timeout: float = 180.0) -> str:
    """One verdict, using the same reading of the exit code the gate's caller
    uses: a crash and a refused configuration are not silence."""
    env = dict(os.environ, DYLD_LIBRARY_PATH=patch.SDK)
    argv = [binary, patch_path, plugin_dir] + ([user_dir] if user_dir else [])
    try:
        r = subprocess.run(argv, capture_output=True, text=True,
                           timeout=timeout, env=env)
    except subprocess.TimeoutExpired:
        return UNMEASURED
    except OSError:
        return UNMEASURED
    if r.returncode < 0 or r.returncode == GATE_REFUSED_RC:
        return UNMEASURED
    return AUDIBLE if r.returncode == GATE_AUDIBLE_RC else SILENT


# ---------------------------------------------------------------------------
# the matrix and the controls
# ---------------------------------------------------------------------------

def transition_matrix(before: dict, after: dict) -> dict:
    """Counts of every before -> after pair over the patches both measured."""
    cells: dict[str, int] = {}
    for key in sorted(set(before) & set(after)):
        cells[f"{before[key]}->{after[key]}"] = \
            cells.get(f"{before[key]}->{after[key]}", 0) + 1
    return cells


def changed_keys(before: dict, after: dict) -> list[str]:
    return sorted(k for k in set(before) & set(after) if before[k] != after[k])


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repo", default=os.path.dirname(os.path.dirname(HERE)))
    ap.add_argument("--jobs", type=int, default=4,
                    help="concurrent gate runs; a share of the host, not all "
                         "of it")
    ap.add_argument("--limit", type=int, default=0,
                    help="measure at most this many patches (debugging)")
    ap.add_argument("--out", default=os.path.join(
        HERE, "licence-fix-replay.json"))
    args = ap.parse_args()
    repo = os.path.realpath(args.repo)

    print("REPLAY: the VCV licence fix, measured against its own absence")
    print()

    sha = licence_fix_commit(repo)
    if not sha:
        print("  SKIP   no commit in this history introduces "
              "rack::asset::init() to the gate, so there is no fix to replay "
              "and this proves nothing")
        return 3
    print(f"  fix commit          {sha[:12]}")

    if not os.path.exists(os.path.join(patch.SDK, "include", "rack.hpp")):
        print(f"  SKIP   no Rack SDK at {patch.SDK}; neither gate can be "
              f"built, so this proves nothing about the fix")
        return 3
    plugin_dir = patch._plugin_dir()
    if not plugin_dir:
        print("  SKIP   no unpacked plugin directory, so no third-party "
              "module can be loaded and this proves nothing about licence "
              "resolution")
        return 3

    roots = corpus_roots(repo)
    corpus, refused, unestablished = collect_corpus(roots)
    if not corpus:
        print(f"  SKIP   none of the {len(roots)} allowed directories holds a "
              f"patch whose provenance is ours ({unestablished} excluded for "
              f"unestablished provenance), so there is nothing this may "
              f"measure and it proves nothing")
        return 3
    licensed = licensed_plugin_slugs()
    if not licensed:
        print("  SKIP   this machine holds no cached VCV licence key, so the "
              "fix is a no-op here and a replay would prove nothing")
        return 3

    keys = sorted(corpus)
    if args.limit:
        keys = keys[:args.limit]
    bearing = {}
    for k in keys:
        bearing[k] = is_licence_bearing(
            patch_plugins(corpus[k]["full"]), licensed)
    n_bearing = sum(1 for k in keys if bearing[k])
    print(f"  corpus              {len(keys)} patches, deduplicated by "
          f"content from {len(roots)} directories")
    held, exclusion_why = exclusion_control()
    print(f"  third-party refused {refused} path(s) while walking; "
          f"{'control holds' if held else 'CONTROL FAILED'}: {exclusion_why}")
    if not held:
        print("  SKIP   the third-party exclusion does not refuse the corpus "
              "it exists to refuse; nothing may be measured until it does")
        return 3
    print(f"  provenance excluded {unestablished} patch(es) this repository "
          f"cannot show it generated")
    print(f"  licence-bearing     {n_bearing} of {len(keys)} patches name a "
          f"plugin this machine holds a key for")
    if n_bearing == 0:
        print("  SKIP   no patch in this corpus contains a licensed plugin, "
              "so the fix cannot change any verdict here and a matrix of "
              "zeros would prove nothing")
        return 3

    work = os.path.join(patch.CACHE_DIR, "licence-replay")
    os.makedirs(work, exist_ok=True)
    variants = {}
    for name, tree_ish in (("prefix", f"{sha}^"), ("fixed", sha)):
        src = os.path.join(work, f"{name}-src")
        if not materialize_gate_source(repo, tree_ish, src):
            print(f"  SKIP   could not read the {name} gate source at "
                  f"{tree_ish}; nothing to compare, so this proves nothing")
            return 3
        binary, why = compile_gate(
            src, os.path.join(work, f"patch-gate-{name}"))
        if not binary:
            print(f"  SKIP   the {name} gate {why}; with only one binary "
                  f"there is no comparison and this proves nothing")
            return 3
        variants[name] = binary
    print(f"  binaries            {variants['prefix']}")
    print(f"                      {variants['fixed']}")

    user_dir, key_count = patch.licence_user_dir()
    if not user_dir:
        print("  SKIP   no staged Rack user directory, so the fixed gate "
              "would run exactly like the pre-fix one and this proves "
              "nothing")
        return 3
    print(f"  user dir            {user_dir} ({key_count} keys)")
    print()

    # Four passes. prefix runs twice so the flip rate under identical
    # conditions is measured rather than assumed to be zero.
    passes = [("prefix-a", variants["prefix"], None),
              ("prefix-b", variants["prefix"], None),
              ("fixed-no-user-dir", variants["fixed"], None),
              ("fixed", variants["fixed"], user_dir)]
    results: dict[str, dict[str, str]] = {}
    for label, binary, ud in passes:
        verdicts: dict[str, str] = {}
        with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
            futures = {pool.submit(run_gate, binary,
                                   corpus[k]["full"], plugin_dir,
                                   ud): k for k in keys}
            for fut in concurrent.futures.as_completed(futures):
                verdicts[futures[fut]] = fut.result()
        results[label] = verdicts
        tally = {v: sum(1 for x in verdicts.values() if x == v)
                 for v in (AUDIBLE, SILENT, UNMEASURED)}
        print(f"  {label:<18} audible {tally[AUDIBLE]:>3}  "
              f"silent {tally[SILENT]:>3}  unmeasured {tally[UNMEASURED]:>3}")
    print()

    # --- the noise floor -------------------------------------------------
    noise = changed_keys(results["prefix-a"], results["prefix-b"])
    print("NOISE FLOOR (one binary, two runs, identical conditions)")
    print(f"  {len(noise)} of {len(keys)} patches disagreed with themselves. "
          f"Any effect at or below this size is not distinguishable from the "
          f"gate's own run-to-run variation.")
    for k in noise:
        print(f"    {corpus[k]["root"]}/{corpus[k]["path"]}: "
              f"{results['prefix-a'][k]} then {results['prefix-b'][k]}")
    print()

    # --- attribution control ---------------------------------------------
    attribution = changed_keys(results["prefix-a"],
                               results["fixed-no-user-dir"])
    print("ATTRIBUTION CONTROL (the fixed binary with the fix switched off)")
    if not attribution:
        print(f"  the fixed gate invoked without a user directory agreed with "
              f"the pre-fix gate on all {len(keys)} patches, so the two "
              f"binaries differ only where the licence directory is used")
    else:
        print(f"  WRONG  {len(attribution)} patch(es) changed verdict with "
              f"the fix switched off, so the binaries differ for some reason "
              f"other than licence resolution")
        for k in attribution:
            print(f"    {corpus[k]["root"]}/{corpus[k]["path"]}: "
                  f"{results['prefix-a'][k]} -> "
                  f"{results['fixed-no-user-dir'][k]}")
    print()

    # --- the measurement --------------------------------------------------
    matrix = transition_matrix(results["prefix-a"], results["fixed"])
    changed = changed_keys(results["prefix-a"], results["fixed"])
    recovered = [k for k in changed
                 if results["prefix-a"][k] == SILENT
                 and results["fixed"][k] == AUDIBLE]
    lost = [k for k in changed
            if results["prefix-a"][k] == AUDIBLE
            and results["fixed"][k] == SILENT]
    was_silent = [k for k in keys if results["prefix-a"][k] == SILENT]
    print("TRANSITION MATRIX (pre-fix -> fixed)")
    for cell in sorted(matrix):
        print(f"  {cell:<28} {matrix[cell]}")
    print()
    print(f"RECOVERED  {len(recovered)} of {len(was_silent)} patches the "
          f"pre-fix gate called silent now measure audible")
    print(f"LOST       {len(lost)} patches the pre-fix gate called audible "
          f"now measure silent")
    for k in recovered:
        print(f"    + {corpus[k]["root"]}/{corpus[k]["path"]}")
    for k in lost:
        print(f"    - {corpus[k]["root"]}/{corpus[k]["path"]}")
    print()

    # --- mechanism control ------------------------------------------------
    drift = [k for k in changed if not bearing[k]]
    print("MECHANISM CONTROL (only a licensed plugin can be affected)")
    if not drift:
        print(f"  every one of the {len(changed)} changed verdicts belongs to "
              f"a patch naming a plugin this machine holds a key for")
    else:
        print(f"  WRONG  {len(drift)} patch(es) changed verdict without "
              f"naming any licensed plugin, so that much of the effect is "
              f"not this fix")
        for k in drift:
            print(f"    {corpus[k]["root"]}/{corpus[k]["path"]}: "
                  f"{results['prefix-a'][k]} -> {results['fixed'][k]} "
                  f"(plugins: {sorted(patch_plugins(corpus[k]["full"]))})")
    print()

    controls_ok = not attribution and not drift
    if not controls_ok:
        print("THE NUMBER ABOVE IS NOT ATTRIBUTABLE TO THE FIX. A control "
              "failed; read the control, not the count.")
    elif len(recovered) <= len(noise):
        print(f"THE NUMBER ABOVE IS AT OR BELOW THE NOISE FLOOR "
              f"({len(recovered)} recovered vs {len(noise)} self-"
              f"disagreements). It is not distinguishable from run-to-run "
              f"variation.")
    else:
        print("Both controls held and the effect is above the noise floor.")

    record = {
        "schema": 1,
        "fix_commit": sha,
        "corpus": {
            "patches": len(keys),
            "licence_bearing": n_bearing,
            "third_party_paths_refused": refused,
            "third_party_exclusion_control": exclusion_why,
            "provenance_excluded": unestablished,
            "directories_walked": len(roots),
            "roots": sorted({corpus[k]["root"] for k in keys}),
            # Pulp's own patches only: the examples in this repository, the
            # fixtures beside its tests, and what the generator wrote.
            "entries": [{"sha256": k, "root": corpus[k]["root"],
                         "path": corpus[k]["path"],
                         "provenance": corpus[k]["provenance"],
                         "licence_bearing": bearing[k]} for k in keys],
        },
        "verdicts": results,
        "noise_floor": {"self_disagreements": len(noise), "patches": noise},
        "attribution_control": {"disagreements": len(attribution),
                                "patches": attribution},
        "mechanism_control": {"unlicensed_changes": len(drift),
                              "patches": drift},
        "matrix": matrix,
        "recovered": recovered,
        "lost": lost,
        "previously_silent": len(was_silent),
        "controls_held": controls_ok,
    }
    with open(args.out, "w") as f:
        json.dump(record, f, indent=2, sort_keys=True)
        f.write("\n")
    print(f"\nwrote {args.out}")
    return 0 if controls_ok else 1


if __name__ == "__main__":
    sys.exit(main())
