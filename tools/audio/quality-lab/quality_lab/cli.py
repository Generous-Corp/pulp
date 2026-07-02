"""CLI entry — parse + dispatch only; the work lives in `pipeline` (§14.4 boundary).

P0a surface (the first-class `pulp audio quality` verb arrives in P1):

    python -m quality_lab.cli run-p0a --out report.json [--mode good|bad]
                                      [--smear-ms 8] [--latency-ms 5]
"""
from __future__ import annotations

import argparse
import json
import sys

from . import pipeline


def _cmd_run_p0a(args: argparse.Namespace) -> int:
    report = pipeline.run_p0a(
        smear=(args.mode == "bad"),
        latency_ms=args.latency_ms,
        smear_ms=args.smear_ms,
    )
    text = json.dumps(report, indent=2)
    if args.out:
        with open(args.out, "w") as f:
            f.write(text)
    det = report["detectors"][0]
    print(
        f"[quality-lab P0a] mode={args.mode} verdict={report['verdict']} "
        f"transient_sharpness scalar={det['scalar']:.3f} fired={det['fired']}"
    )
    for w in report["worst_regions"][:3]:
        print(f"  worst: t={w['time_s']:.3f}s severity={w['severity']:.3f} {w['label']}")
    if args.out:
        print(f"[quality-lab P0a] wrote {args.out}")
    return 0


def _cmd_run(args: argparse.Namespace) -> int:
    case = pipeline.TONAL_CASE if args.case == "tonal" else pipeline.P0A_CASE
    if args.out_dir:
        report = pipeline.run_and_export(args.degradation, args.out_dir, case=case,
                                         latency_ms=args.latency_ms, smear_ms=args.smear_ms)
    else:
        report = pipeline.run(args.degradation, case=case,
                              latency_ms=args.latency_ms, smear_ms=args.smear_ms)
    if getattr(args, "review", False):
        from . import reviewer
        reviewer.attach(report)  # advisory only — never changes the verdict
    if args.out:
        with open(args.out, "w") as f:
            f.write(json.dumps(report, indent=2))
    print(f"[quality-lab] degradation={args.degradation} verdict={report['verdict']}")
    for d in report["detectors"]:
        flag = "FIRE" if d["fired"] else "ok"
        # Experimental detectors are advisory — flag them so a reader never reads their
        # FIRE as part of the gate (the verdict already excludes them).
        adv = "" if d.get("participates_in_verdict", True) else f"  (advisory:{d.get('maturity','?')})"
        print(f"  {d['name']:20s} {d['scalar']:.3f} {d['unit']:20s} [{flag}]{adv}")
    if report.get("listening", {}).get("regions"):
        print(f"  listening: {len(report['listening']['regions'])} region clip(s) in {args.out_dir}")
    for rv in report.get("advisory", {}).get("reviewers", []):
        if rv.get("status") == "ok":
            arts = ", ".join(rv.get("suspected_artifacts", [])) or "(none named)"
            print(f"  reviewer (advisory, not a gate): {arts} — conf={rv.get('confidence')}")
        else:
            print(f"  reviewer (advisory): {rv.get('status')} — {rv.get('reason','')}")
    return 0


def _cmd_engine(args: argparse.Namespace) -> int:
    if args.input:
        report = pipeline.run_real_audio(args.input, ratio=args.ratio, character=args.character)
    else:
        report = pipeline.run_real_engine(ratio=args.ratio, character=args.character)
    if report["verdict"] == "SKIPPED":
        print(f"[quality-lab engine] SKIPPED — {report['reason']}")
        return 0
    if report["verdict"] == "ERROR":
        print(f"[quality-lab engine] ERROR — {report['engine'].get('reason')}")
        return 1
    print(f"[quality-lab engine] REAL OfflineStretch ratio={args.ratio} "
          f"character={args.character} verdict={report['verdict']}")
    for d in report["detectors"]:
        flag = "FIRE" if d["fired"] else "ok"
        # Experimental detectors are advisory — flag them so a reader never reads their
        # FIRE as part of the gate (the verdict already excludes them).
        adv = "" if d.get("participates_in_verdict", True) else f"  (advisory:{d.get('maturity','?')})"
        print(f"  {d['name']:20s} {d['scalar']:.3f} {d['unit']:20s} [{flag}]{adv}")
    if args.out:
        with open(args.out, "w") as f:
            f.write(json.dumps(report, indent=2))
    return 0


def _cmd_corpus(args: argparse.Namespace) -> int:
    from . import corpus
    cdir = args.dir or corpus.default_corpus_dir()
    if args.corpus_cmd == "seed":
        m = corpus.seed(cdir)
        print(f"[quality-lab corpus] seeded {len(m['sources'])} synthetic source(s) in {cdir}")
    elif args.corpus_cmd == "add":
        try:
            e = corpus.add_source(cdir, args.file, name=args.name, material_class=args.material_class,
                                  license_id=args.license, expected=args.expect, family=args.family)
        except (ValueError, FileNotFoundError) as exc:
            print(f"[quality-lab corpus] REJECTED — {exc}")
            return 1
        print(f"[quality-lab corpus] added '{e['name']}' ({e['material_class']}, {e['license']})")
    else:  # list
        m = corpus.load_manifest(cdir)
        print(f"[quality-lab corpus] {len(m['sources'])} source(s) in {cdir}:")
        for s in m["sources"]:
            print(f"  {s['name']:24s} {s['material_class']:11s} {s['license']:12s} "
                  f"{s['kind']:9s} — {s['expected_artifacts']}")
    return 0


def _cmd_engine_baseline(args: argparse.Namespace) -> int:
    from . import engine, engine_baseline
    if not engine.available():
        print("[quality-lab engine-baseline] SKIPPED — stretchcli not found "
              "(cmake --build build --target stretchcli, or set "
              "PULP_STRETCHCLI=/path/to/stretchcli)")
        return 0
    if args.capture:
        path = engine_baseline.write_baseline(engine_baseline.capture())
        print(f"[quality-lab engine-baseline] captured baseline -> {path}")
        return 0
    deviations = engine_baseline.check()
    if not deviations:
        print("[quality-lab engine-baseline] OK — engine matches committed baseline")
        return 0
    print(f"[quality-lab engine-baseline] REGRESSION — {len(deviations)} deviation(s):")
    for d in deviations:
        tag = " (WORSE)" if d.get("worse") else ""
        print(f"  {d['case']} {d['detector']}: {d.get('baseline')} -> {d.get('current')} "
              f"(delta {d.get('delta')}){tag}")
    return 1


def _cmd_loop(args: argparse.Namespace) -> int:
    from . import loop
    # Deterministic demo pass over the synthetic degradations (the loop's real candidates
    # are engine/corpus renders; this proves the skeleton + proposal transaction).
    cands = [loop.score_case(d, d) for d in ("identity", "smear", "dull", "fizz", "grainy")]
    result = loop.run_iteration(cands)
    print(f"[quality-lab loop] champion={result['champion']} (experimental — proposes, never decides)")
    for r in result["ranked"]:
        print(f"  {r['label']:10s} total_badness={r['total_badness']:.3f}")
    if args.corpus_dir:
        path = loop.propose_labels(args.corpus_dir, [
            {"name": result["champion"], "proposed_expected_artifacts": "(none — clean champion)",
             "evidence": "tuning-loop demo pass"}])
        print(f"  wrote label proposals -> {path} (apply to MANIFEST.json by hand)")
    return 0


def _cmd_compare(args: argparse.Namespace) -> int:
    import json
    from . import compare
    try:
        report = compare.compare_files(
            args.reference, args.candidate,
            profile=args.profile, reference_role=args.reference_role,
            threshold=args.threshold,
        )
    except ValueError as exc:
        # A caller-contract error — in practice an out-of-range --threshold for this axis (profile
        # and reference_role are argparse-constrained). Emit a structured `invalid` report so a
        # delegating caller (the MCP tool) reads a clean verdict instead of a raw traceback that
        # its "empty report" heuristic would misread as "the tool isn't installed".
        report = compare.invalid_report(args.profile, args.reference_role, str(exc))
    print(f"[quality-lab compare] {report['verdict']}: {report['summary']}")
    if args.json:
        with open(args.json, "w") as fh:
            json.dump(report, fh, indent=2)
        print(f"  wrote report -> {args.json}")
    # Advisory: exit nonzero ONLY when we could not measure (invalid input/tool failure),
    # never for a judgment — a regression_suspected verdict is advice, not a gate failure.
    return 2 if report["verdict"] == compare.VERDICT_INVALID else 0


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="quality-lab", description="Audio Quality Lab")
    sub = p.add_subparsers(dest="cmd", required=True)

    rp = sub.add_parser("run-p0a", help="run the P0a drum-break gate slice")
    rp.add_argument("--out", default="", help="write report.json to this path")
    rp.add_argument("--mode", choices=["good", "bad"], default="bad")
    rp.add_argument("--smear-ms", type=float, default=8.0, dest="smear_ms")
    rp.add_argument("--latency-ms", type=float, default=5.0, dest="latency_ms")
    rp.set_defaults(func=_cmd_run_p0a)

    rn = sub.add_parser("run", help="run all detectors on a degradation; optionally export clips")
    rn.add_argument("--case", choices=["drum", "tonal"], default="drum",
                    help="which QualityCase family to run (drum=percussive, tonal=sustained pad)")
    rn.add_argument("--degradation", choices=["identity", "smear", "dull", "fizz", "grainy", "noisy"], default="smear")
    rn.add_argument("--out", default="", help="write report.json to this path")
    rn.add_argument("--out-dir", default="", dest="out_dir",
                    help="write reference/candidate WAVs + worst-region clip pairs here")
    rn.add_argument("--smear-ms", type=float, default=8.0, dest="smear_ms")
    rn.add_argument("--latency-ms", type=float, default=5.0, dest="latency_ms")
    rn.add_argument("--review", action="store_true",
                    help="run the opt-in advisory reviewer (PULP_QLAB_REVIEWER_CMD); never a gate")
    rn.set_defaults(func=_cmd_run)

    re = sub.add_parser("engine", help="validate the REAL Pulp stretch engine (stretchcli)")
    re.add_argument("--input", default="", help="a REAL audio WAV (reference-free dry-input check); "
                    "omit to use the synthetic drum corpus")
    re.add_argument("--ratio", type=float, default=2.0)
    re.add_argument("--character", default="clean",
                    choices=["clean", "varispeed", "phase_vocoder", "granular"])
    re.add_argument("--out", default="", help="write report.json to this path")
    re.set_defaults(func=_cmd_engine)

    cp = sub.add_parser("corpus", help="manage the versioned, license-guarded corpus (P0b)")
    cp.add_argument("--dir", default="", help="corpus directory (default: committed corpus)")
    csub = cp.add_subparsers(dest="corpus_cmd", required=True)
    csub.add_parser("list", help="list corpus sources")
    csub.add_parser("seed", help="seed the synthetic families")
    ca = csub.add_parser("add", help="add a real audio source (permissive license required)")
    ca.add_argument("--file", required=True)
    ca.add_argument("--name", required=True)
    ca.add_argument("--class", dest="material_class", required=True)
    ca.add_argument("--license", required=True)
    ca.add_argument("--expect", required=True, help="one-line: what should sound wrong")
    ca.add_argument("--family", default="tonal")
    cp.set_defaults(func=_cmd_corpus)

    eb = sub.add_parser("engine-baseline",
                        help="regression gate vs the real engine: --capture or --check")
    eb.add_argument("--capture", action="store_true", help="(re)write the committed baseline")
    eb.set_defaults(func=_cmd_engine_baseline)

    lp = sub.add_parser("loop",
                        help="experimental: one tuning-loop pass (rank candidates; proposes, never decides)")
    lp.add_argument("--corpus-dir", default="", dest="corpus_dir",
                    help="write label proposals to <dir>/LABEL_PROPOSALS.json (never MANIFEST.json)")
    lp.set_defaults(func=_cmd_loop)

    from . import compare
    cmp_ = sub.add_parser(
        "compare",
        help="advisory before/after judgment over two WAVs (agent-facing; not a gate)")
    cmp_.add_argument("reference", help="reference (before) WAV")
    cmp_.add_argument("candidate", help="candidate (after) WAV")
    cmp_.add_argument("--profile", choices=list(compare.PROFILES), default="tonal-balance",
                      help="measurement axis: tonal-balance (LTAS centroid shift) or "
                           "added-hf (band-relative >=8kHz fraction ratio, dB)")
    cmp_.add_argument("--reference-role", choices=["peer", "golden"], default="peer",
                      dest="reference_role",
                      help="golden = reference is known-good (enables regression_suspected); "
                           "peer = neutral material_change_detected only")
    cmp_.add_argument("--threshold", type=float, default=None,
                      help="materiality threshold override (defaults to the axis's own default)")
    cmp_.add_argument("--json", default="", help="write the full report JSON to this path")
    cmp_.set_defaults(func=_cmd_compare)

    args = p.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
