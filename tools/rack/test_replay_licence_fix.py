#!/usr/bin/env python3
"""The licence-fix replay must be able to report a failure.

    python3 tools/rack/test_replay_licence_fix.py

Hermetic: nothing here builds a gate or loads a plugin. What is checked is the
part of the replay that decides what a run MEANS -- the exclusion of somebody
else's patches, the transition matrix, and the two controls that are the only
reason its headline number is worth anything.

Each case runs in both directions. A control that cannot fail passes every run
while the thing it exists to catch walks straight past it, which is the exact
failure that made the original replay unciteable.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import replay_licence_fix as R                               # noqa: E402


def test_the_crawled_corpus_is_refused():
    """The one rule that is not about measurement at all.

    A third-party patch may not be read, hashed, or written down. The check
    has to reject a path under the crawled corpus AND accept Pulp's own, or it
    is either useless or it excludes the whole corpus and reports zero.
    """
    home = os.path.expanduser("~")
    crawled = f"{home}/Library/Application Support/Forge Modular/corpus/" \
              f"patchstorage/patches/somebody-elses.vcv"
    reference = f"{home}/Library/Application Support/Forge Modular/.corpus/" \
                f"local/notes.vcv"
    ours = f"{home}/Library/Application Support/Forge Modular/patches/mine.vcv"
    repo = os.path.join(os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))),
        "examples/forge-modular/patches/kick.vcv")
    return [
        ("a crawled third-party patch is refused",
         R.is_third_party(crawled), f"{crawled} was not refused"),
        ("the reference corpus is refused",
         R.is_third_party(reference), f"{reference} was not refused"),
        # The opposite direction. Refusing everything would also produce an
        # empty corpus, and an empty corpus reports zero recoveries -- which
        # reads like a fix that did nothing.
        ("a generated Pulp patch is not refused",
         not R.is_third_party(ours), f"{ours} was refused"),
        ("a repository example is not refused",
         not R.is_third_party(repo), f"{repo} was refused"),
    ]


def test_the_matrix_counts_every_cell():
    before = {"a": R.SILENT, "b": R.SILENT, "c": R.AUDIBLE, "d": R.AUDIBLE,
              "e": R.SILENT}
    after = {"a": R.AUDIBLE, "b": R.SILENT, "c": R.AUDIBLE, "d": R.SILENT,
             "e": R.UNMEASURED}
    matrix = R.transition_matrix(before, after)
    return [
        ("a recovery is counted", matrix.get("silent->audible") == 1,
         f"silent->audible was {matrix.get('silent->audible')}"),
        ("a loss is counted", matrix.get("audible->silent") == 1,
         f"audible->silent was {matrix.get('audible->silent')}"),
        ("an unchanged silent patch is counted",
         matrix.get("silent->silent") == 1,
         f"silent->silent was {matrix.get('silent->silent')}"),
        ("an unchanged audible patch is counted",
         matrix.get("audible->audible") == 1,
         f"audible->audible was {matrix.get('audible->audible')}"),
        # A patch the second pass could not measure is its own cell, never
        # folded into silence. Reading UNMEASURED as SILENT would have
        # invented a loss here.
        ("a patch that became unmeasured is its own cell",
         matrix.get("silent->unmeasured") == 1,
         f"silent->unmeasured was {matrix.get('silent->unmeasured')}"),
        ("a patch only one side measured is not counted",
         sum(matrix.values()) == 5,
         f"the matrix totals {sum(matrix.values())} over 5 shared patches"),
    ]


def test_a_patch_measured_by_only_one_pass_is_dropped():
    """Half a measurement is not a transition.

    Counting a patch the other pass never judged would let a crash in one pass
    manufacture a recovery.
    """
    before = {"a": R.SILENT, "only-before": R.SILENT}
    after = {"a": R.AUDIBLE, "only-after": R.AUDIBLE}
    matrix = R.transition_matrix(before, after)
    changed = R.changed_keys(before, after)
    return [
        ("only the shared patch is in the matrix",
         matrix == {"silent->audible": 1}, f"matrix was {matrix}"),
        ("only the shared patch is reported changed",
         changed == ["a"], f"changed was {changed}"),
    ]


def test_the_mechanism_control_discriminates():
    """A verdict can only move for this fix if a licensed plugin is present.

    Both directions, because a control that calls everything licence-bearing
    passes the whole sweep and one that calls nothing licence-bearing makes
    the harness SKIP -- and neither has looked at anything.
    """
    licensed = {"VCV-Console", "PathSet-Grains"}
    return [
        ("a patch naming a licensed plugin is in scope",
         R.is_licence_bearing({"Core", "VCV-Console"}, licensed),
         "a patch with VCV-Console was ruled out of scope"),
        ("a patch of free modules is out of scope",
         not R.is_licence_bearing({"Core", "Fundamental", "Bogaudio"},
                                  licensed),
         "a patch with no licensed plugin was ruled in scope"),
        ("an empty patch is out of scope",
         not R.is_licence_bearing(set(), licensed),
         "a patch naming no plugin at all was ruled in scope"),
        # The instrument itself: with no keys on the machine NOTHING is in
        # scope, which is why the harness SKIPs rather than reporting zero.
        ("nothing is in scope on a machine with no keys",
         not R.is_licence_bearing({"VCV-Console"}, set()),
         "a plugin was ruled licensed against an empty key set"),
    ]


def test_the_attribution_control_discriminates():
    """The fixed binary with the fix switched off must match the old one.

    Modelled as the same disagreement finder the replay uses. It has to be
    silent when they agree and loud when they do not; a finder that always
    returns nothing would clear a binary that differs for a reason that has
    nothing to do with licences.
    """
    prefix = {"a": R.SILENT, "b": R.AUDIBLE, "c": R.SILENT}
    agrees = {"a": R.SILENT, "b": R.AUDIBLE, "c": R.SILENT}
    differs = {"a": R.AUDIBLE, "b": R.AUDIBLE, "c": R.SILENT}
    return [
        ("an agreeing binary raises nothing",
         R.changed_keys(prefix, agrees) == [],
         f"agreement reported {R.changed_keys(prefix, agrees)}"),
        ("a binary that differs is named",
         R.changed_keys(prefix, differs) == ["a"],
         f"disagreement reported {R.changed_keys(prefix, differs)}"),
    ]


def test_the_fix_commit_is_found_by_content():
    """A hash written down here would stop resolving on the next rebase.

    That is not hypothetical: the binaries this replay reconstructs were lost
    exactly that way. So the commit is located by the text it introduced, and
    the negative direction -- a repository where nothing introduced that text
    -- has to come back None rather than guessing.
    """
    import subprocess
    repo = os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__))))
    sha = R.licence_fix_commit(repo)

    def gate_at(tree_ish: str) -> str:
        r = subprocess.run(
            ["git", "show", f"{tree_ish}:tools/rack/patch_gate.cpp"],
            cwd=repo, capture_output=True, text=True)
        return r.stdout if r.returncode == 0 else ""

    # A well-formed hash is not a right one. Ask the two trees the answer
    # names: the commit's own gate has to CALL asset::init() and its parent's
    # has to not, or the replay would compile two gates that differ in
    # something else and call the difference a licence effect.
    after = gate_at(sha or "HEAD")
    before = gate_at(f"{sha}^" if sha else "HEAD")
    results = [
        ("a repository with the fix yields a commit",
         bool(sha) and len(sha or "") == 40, f"got {sha!r} from {repo}"),
        ("the named commit's gate resolves licence keys",
         "rack::asset::init()" in after,
         f"{sha} does not call asset::init() in the gate"),
        ("its parent's gate does not",
         bool(before) and "rack::asset::init()" not in before,
         f"{sha}^ already called asset::init(), so it is not the fix"),
    ]
    # The opposite direction, in a directory that is not a git repository at
    # all: no answer, not a wrong one.
    results.append(("a directory with no history yields no commit",
                    R.licence_fix_commit("/") is None,
                    "a non-repository produced a commit hash"))
    return results


def test_provenance_has_to_be_established():
    """Being in a directory we like is not evidence of anything.

    A patch downloaded and dropped into the generator's output folder is
    indistinguishable by content from one the generator wrote -- both are Rack
    documents with modules and cables and no author anywhere. So each source
    has to produce its own sidecar, and the check has to accept the file that
    has one AND reject the identical file that does not. Only rejecting proves
    nothing (a gate that rejects everything reports an empty corpus, which
    reads as a fix that did nothing); only accepting proves less than nothing.
    """
    import json
    import tempfile
    out = []
    with tempfile.TemporaryDirectory() as tmp:
        kept = os.path.join(tmp, "patches")
        os.makedirs(kept)
        open(os.path.join(kept, "ours.vcv"), "w").write("{}")
        open(os.path.join(kept, "ours.why.json"), "w").write("{}")
        open(os.path.join(kept, "dropped-in.vcv"), "w").write("{}")
        out += [
            ("a kept patch with its why.json is ours",
             R.provenance(os.path.join(kept, "ours.vcv"),
                          "generated/patches", None) is not None,
             "a generated patch with its sidecar was excluded"),
            ("a bare patch in the same folder is not",
             R.provenance(os.path.join(kept, "dropped-in.vcv"),
                          "generated/patches", None) is None,
             "a patch with no sidecar was admitted from the output folder"),
        ]

        att = os.path.join(tmp, "attempts", "1234-a-name")
        os.makedirs(att)
        open(os.path.join(att, "attempt01-bright-drone.vcv"), "w").write("{}")
        open(os.path.join(att, "attempt01-bright-drone.txt"), "w").write("no")
        open(os.path.join(att, "attempt02-lonely.vcv"), "w").write("{}")
        open(os.path.join(att, "borrowed.vcv"), "w").write("{}")
        acid = os.path.join(tmp, "attempts", "attempt03-acid-proof")
        os.makedirs(acid)
        open(os.path.join(acid, "acid-source.vcv"), "w").write("{}")
        open(os.path.join(acid, "borrowed.vcv"), "w").write("{}")
        out += [
            ("a rejected attempt with its artifacts is ours",
             R.provenance(os.path.join(att, "attempt01-bright-drone.vcv"),
                          "generated/attempts", None) is not None,
             "an attempt with its own log was excluded"),
            ("an attempt-shaped name with no artifacts is not",
             R.provenance(os.path.join(att, "attempt02-lonely.vcv"),
                          "generated/attempts", None) is None,
             "a file named like an attempt but with nothing beside it was "
             "admitted"),
            ("an unrelated file in an attempt folder is not",
             R.provenance(os.path.join(att, "borrowed.vcv"),
                          "generated/attempts", None) is None,
             "an arbitrary patch in an attempt folder was admitted"),
            ("the acid proof's own patch is ours",
             R.provenance(os.path.join(acid, "acid-source.vcv"),
                          "generated/attempts", None) is not None,
             "the acid proof's patch was excluded"),
            ("an unrelated file in the acid folder is not",
             R.provenance(os.path.join(acid, "borrowed.vcv"),
                          "generated/attempts", None) is None,
             "an arbitrary patch in the acid folder was admitted"),
        ]

        def project(name: str, meta: dict) -> str:
            d = os.path.join(tmp, "projects", name)
            os.makedirs(d)
            open(os.path.join(d, "patch.vcv"), "w").write("{}")
            json.dump(meta, open(os.path.join(d, "project.json"), "w"))
            return os.path.join(d, "patch.vcv")

        mine = project("mine", {"remixed_from": "", "chat_history": [1]})
        remix = project("remix", {"remixed_from": "someone-else",
                                  "chat_history": [1]})
        store = project("store", {"remixed_from": "",
                                  "marketplace_read_only": True,
                                  "chat_history": [1]})
        empty = project("empty", {"remixed_from": "", "chat_history": []})
        out += [
            ("a project built in this app is ours",
             R.provenance(mine, "generated/projects", None) is not None,
             "a locally built project was excluded"),
            ("a project remixed from someone else is not",
             R.provenance(remix, "generated/projects", None) is None,
             "a remix of another author's project was admitted"),
            ("a read-only marketplace project is not",
             R.provenance(store, "generated/projects", None) is None,
             "a marketplace project was admitted"),
            ("a project with no conversation is not",
             R.provenance(empty, "generated/projects", None) is None,
             "a project with no local history was admitted"),
        ]

    # A repository root asks git, so an untracked file in the examples folder
    # is excluded even though the folder itself is allowlisted.
    repo = os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__))))
    tracked = os.path.join(repo, "tools/rack/patch.py")
    untracked = os.path.join(repo, "tools/rack/not-a-real-file.vcv")
    out += [
        ("a tracked file in a checkout is ours",
         R.provenance(tracked, "repo/examples", repo) is not None,
         "a tracked file was excluded from its own repository"),
        ("an untracked file in the same folder is not",
         R.provenance(untracked, "repo/examples", repo) is None,
         "an untracked file was admitted from a repository root"),
        ("a checkout root with no checkout to ask is not",
         R.provenance(tracked, "repo/examples", None) is None,
         "a repository root admitted a file with no checkout to ask"),
        ("a directory nobody allowlisted is not",
         R.provenance(tracked, "somewhere/else", repo) is None,
         "an unknown root label admitted a file"),
    ]
    return out


def test_the_exclusion_control_can_fail():
    """A refusal count of zero is not evidence the refusal works.

    The allowlist never walks the crawled corpus, so nothing gets refused on a
    healthy run and the counter reads zero -- which is exactly what a refusal
    that quietly stopped working also reads. The control asks the predicate
    directly, and it has to go red when the predicate stops refusing.
    """
    original = R.is_third_party
    try:
        R.is_third_party = lambda path: False
        broken_ok, broken_why = R.exclusion_control()
        R.is_third_party = original
        live_ok, live_why = R.exclusion_control()
    finally:
        R.is_third_party = original
    crawled = os.path.join(os.path.expanduser("~"), "Library",
                           "Application Support", "Forge Modular", "corpus")
    results = [("the working exclusion holds", live_ok, live_why)]
    if os.path.isdir(crawled):
        results.append(
            ("an exclusion that refuses nothing is caught",
             not broken_ok,
             "the control passed while the predicate refused nothing"))
        results.append(
            ("the control says it actually asked",
             "refused" in live_why and "says nothing" not in live_why,
             f"the control reported {live_why!r} on a machine that has the "
             f"corpus at {crawled}"))
    else:
        # No corpus here, so the control cannot be exercised. Say so rather
        # than passing a check that never ran.
        results.append(
            ("the control admits it proved nothing",
             "says nothing" in live_why,
             f"no corpus at {crawled}, but the control reported {live_why!r}"))
    return results


CASES = [
    test_the_crawled_corpus_is_refused,
    test_the_matrix_counts_every_cell,
    test_a_patch_measured_by_only_one_pass_is_dropped,
    test_the_mechanism_control_discriminates,
    test_the_attribution_control_discriminates,
    test_the_fix_commit_is_found_by_content,
    test_provenance_has_to_be_established,
    test_the_exclusion_control_can_fail,
]


def main() -> int:
    bad = ran = 0
    for case in CASES:
        try:
            results = case()
        except Exception as error:                          # noqa: BLE001
            print(f"  ERROR  {case.__name__}: {error!r}")
            bad += 1
            ran += 1
            continue
        for name, ok, detail in results:
            ran += 1
            if not ok:
                bad += 1
                print(f"  WRONG  {name}\n         {detail}")
    print(f"licence-fix replay: {ran - bad}/{ran} correct")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
