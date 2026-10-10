#!/usr/bin/env python3
"""Generate, gate, and export Vellum extraction closures.

The manifest is derived from CMake's file-api codemodel after a clean configure.
It is intentionally a receipt generator, not a source-authority switch: exports
must land on a protected mirror ref and never rewrite Vellum main.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Iterable

INCLUDE_RE = re.compile(r"^\s*#\s*include\s*[<\"]([^>\"]+)[>\"]", re.M)
FORBIDDEN = re.compile(r"(?:audio|signal|host|midi|format|content)", re.I)
TEXT_EXTS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".m", ".mm", ".cmake"}
RESOURCE_EXTS = {".sksl", ".glsl", ".frag", ".vert", ".ttf", ".otf", ".woff", ".woff2"}


def run(*cmd: str, cwd: Path | None = None) -> str:
    return subprocess.check_output(cmd, cwd=cwd, text=True, stderr=subprocess.STDOUT).strip()


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def find_reply(build: Path, prefix: str) -> Path:
    replies = sorted((build / ".cmake/api/v1/reply").glob(prefix + "*.json"))
    if not replies:
        raise SystemExit(f"missing CMake file-api reply {prefix}*.json; configure after creating query")
    return replies[-1]


def codemodel(build: Path) -> tuple[dict, dict]:
    index = json.loads(find_reply(build, "index-").read_text())
    cm = next((x for x in index["reply"].values() if x.get("kind") == "codemodel"), None)
    if not cm:
        raise SystemExit("CMake file-api index has no codemodel reply")
    model = json.loads((build / ".cmake/api/v1/reply" / cm["jsonFile"]).read_text())
    configurations = model.get("configurations", [])
    if not configurations:
        raise SystemExit("CMake codemodel has no configuration")
    config = configurations[0]
    targets: dict[str, dict] = {}
    for entry in config.get("targets", []):
        data = json.loads((build / ".cmake/api/v1/reply" / entry["jsonFile"]).read_text())
        targets[data["id"]] = data
    return config, targets


def norm_path(raw: str, source: Path, build: Path) -> str:
    p = Path(raw)
    if not p.is_absolute():
        # File-api paths are relative to the codemodel source root unless
        # explicitly rooted.  Resolving against cwd made the manifest depend
        # on where the script was launched.
        p = source / p
    try:
        return p.resolve().relative_to(source.resolve()).as_posix()
    except ValueError:
        try:
            return "@build/" + p.resolve().relative_to(build.resolve()).as_posix()
        except ValueError:
            return str(p.resolve())


def resolve_include(name: str, origin: Path, include_dirs: list[Path], source: Path) -> Path | None:
    candidates = [origin.parent / name] + [d / name for d in include_dirs] + [source / name]
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    return None


def collect_headers(files: set[Path], include_dirs: list[Path], source: Path) -> set[Path]:
    queue = list(files)
    seen = set(files)
    while queue:
        path = queue.pop()
        if path.suffix.lower() not in TEXT_EXTS or not path.is_file():
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        for name in INCLUDE_RE.findall(text):
            found = resolve_include(name, path, include_dirs, source)
            if found and found not in seen:
                seen.add(found)
                queue.append(found)
    return seen


def target_manifest(source: Path, build: Path, names: list[str]) -> dict:
    config, targets = codemodel(build)
    by_name = {data.get("name"): data for data in targets.values()}
    selected: list[dict] = []
    missing: list[str] = []
    all_files: set[Path] = set()
    all_include_dirs: set[Path] = set()
    all_defines: set[str] = set()
    links: set[str] = set()
    generated: set[str] = set()
    resources: set[str] = set()
    external: set[str] = set()
    for name in names:
        data = by_name.get(name)
        if not data:
            missing.append(name)
            continue
        target_files: set[Path] = set()
        source_entries = data.get("sources", [])
        for group in data.get("sourceGroups", []):
            for index in group.get("sourceIndexes", []):
                item = source_entries[index] if index < len(source_entries) else {}
                raw = item.get("path")
                if raw:
                    p = Path(raw)
                    if not p.is_absolute():
                        p = source / p
                    p = p.resolve()
                    target_files.add(p)
                    all_files.add(p)
                    rel = norm_path(raw, source, build)
                    if p.suffix.lower() in RESOURCE_EXTS:
                        resources.add(rel)
                    if rel.startswith("@build/"):
                        generated.add(rel)
        includes: list[str] = []
        defines: list[str] = []
        for group in data.get("compileGroups", []):
            for item in group.get("includes", []):
                raw = item.get("path")
                if raw:
                    p = Path(raw)
                    if not p.is_absolute():
                        p = source / p
                    p = p.resolve()
                    all_include_dirs.add(p)
                    includes.append(norm_path(raw, source, build))
            for item in group.get("defines", []):
                value = item.get("define")
                if value:
                    all_defines.add(value)
                    defines.append(value)
        deps: list[str] = []
        for dep in data.get("dependencies", []):
            dep_data = targets.get(dep.get("id"))
            dep_name = dep_data.get("name") if dep_data else dep.get("id")
            if dep_name:
                links.add(dep_name)
                deps.append(dep_name)
        selected.append({"name": name, "type": data.get("type"), "sources": sorted(norm_path(str(p), source, build) for p in target_files), "include_dirs": sorted(set(includes)), "compile_definitions": sorted(set(defines)), "link_targets": sorted(set(deps))})
    all_files = collect_headers(all_files, sorted(all_include_dirs), source)
    normalized = [norm_path(str(p), source, build) for p in all_files]
    external.update(p for p in normalized if not p.startswith(("@build/",)) and p.startswith("/"))
    files = sorted(p for p in normalized if p not in external)
    for rel in files:
        if Path(rel).suffix.lower() in RESOURCE_EXTS:
            resources.add(rel)
    return {"schema": "pulp.vellum.closure.v1", "source_root": str(source), "build_dir": str(build), "configuration": config.get("name", ""), "targets": selected, "missing_targets": missing, "files": files, "external_inputs": sorted(external), "public_include_dirs": sorted(str(p) for p in all_include_dirs), "compile_definitions": sorted(all_defines), "transitive_link_targets": sorted(links), "resources": sorted(resources), "generated_inputs": sorted(generated)}


def cmd_manifest(args: argparse.Namespace) -> int:
    result = target_manifest(Path(args.source_root).resolve(), Path(args.build_dir).resolve(), args.target)
    Path(args.output).write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(f"closure_manifest={args.output} files={len(result['files'])} missing={len(result['missing_targets'])}")
    return 0


def cmd_gate(args: argparse.Namespace) -> int:
    failed = False
    for raw in args.manifest:
        data = json.loads(Path(raw).read_text())
        name = Path(raw).name
        violations: list[str] = []
        if data.get("missing_targets"):
            violations.extend("missing target: " + x for x in data["missing_targets"])
        for target in data.get("targets", []):
            for link in target.get("link_targets", []):
                if FORBIDDEN.search(link) and link not in args.allow:
                    violations.append(f"forbidden link {target['name']} -> {link}")
        for path in data.get("files", []):
            if path.startswith("@build/"):
                continue
            full = Path(data["source_root"]) / path
            if full.suffix.lower() in TEXT_EXTS and full.is_file():
                text = full.read_text(encoding="utf-8", errors="replace")
                for line_no, line in enumerate(text.splitlines(), 1):
                    if re.search(r"#\s*include\s*[<\"]pulp/(?:audio|signal|host|midi|format|content)/", line, re.I):
                        violations.append(f"forbidden include {path}:{line_no}: {line.strip()}")
        status = "PASS" if not violations else ("REPORT-ONLY" if args.report_only else "FAIL")
        print(f"{name}: {status}")
        for violation in violations:
            print(f"  {violation}")
        failed |= bool(violations)
    return 1 if failed and not args.report_only else 0


def git_blob_rows(repo: Path, commit: str, paths: Iterable[str]) -> list[dict]:
    rows: list[dict] = []
    for path in paths:
        if path.startswith("@build/"):
            continue
        try:
            row = run("git", "-C", str(repo), "ls-tree", commit, "--", path)
        except subprocess.CalledProcessError:
            continue
        if row:
            mode, kind, blob, name = row.split("\t", 1)[0].split(" ") + [row.split("\t", 1)[1]]
            if kind == "blob":
                rows.append({"path": name, "blob": blob})
    return rows


def cmd_export(args: argparse.Namespace) -> int:
    manifest = json.loads(Path(args.manifest).read_text())
    repo = Path(args.repo).resolve()
    out = Path(args.output).resolve()
    if out.exists() and any(out.iterdir()):
        raise SystemExit(f"output must be empty: {out}")
    out.parent.mkdir(parents=True, exist_ok=True)
    filter_repo = shutil.which("git-filter-repo") or shutil.which("git-filter-repo.py")
    if not filter_repo:
        raise SystemExit("git-filter-repo is required for export; refusing fallback projection")
    paths = [p for p in manifest["files"] if not p.startswith("@build/")]
    subprocess.run(["git", "clone", "--no-local", str(repo), str(out)], check=True, stdout=subprocess.DEVNULL)
    mirror_root = args.mirror_root.strip("/")
    if not mirror_root or mirror_root.startswith("@"):
        raise SystemExit("mirror-root must be a non-empty repository-relative directory")
    rename_map = args.rename_map or {}
    cmd = [filter_repo, "--force"]
    for path in paths:
        mirror_path = rename_map.get(path, path)
        if Path(mirror_path).is_absolute() or ".." in Path(mirror_path).parts:
            raise SystemExit(f"rename map escapes repository: {path} -> {mirror_path}")
        cmd.extend(["--path", path, "--path-rename", f"{path}:{mirror_root}/{mirror_path}"])
    subprocess.run(cmd, cwd=out, check=True)
    mirror_sha = run("git", "-C", str(out), "rev-parse", "HEAD")
    rows = git_blob_rows(repo, args.pulp_sha, paths)
    receipt_path = out / "provenance" / "mirror" / "export-receipt.json"
    receipt_path.parent.mkdir(parents=True, exist_ok=True)
    by_path = {row["path"]: row["blob"] for row in rows}
    records = [
        {
            "mirror_path": f"{mirror_root}/{rename_map.get(path, path)}",
            "git_blob_sha": by_path[path],
            "pulp_commit": args.pulp_sha,
            "pulp_path": path,
            "export_receipt": "provenance/mirror/export-receipt.json",
        }
        for path in paths if path in by_path
    ]
    receipt = {
        "schema": "pulp.vellum.export-receipt.v2",
        "pulp_repository": "Generous-Corp/pulp",
        "pulp_commit": args.pulp_sha,
        "mirror_commit": mirror_sha,
        "mirror_root": f"{mirror_root}/",
        "rename_map": rename_map,
        "records": records,
        "generated_inputs": {p: sha256(repo / p) for p in manifest.get("generated_inputs", []) if not p.startswith("@build/") and (repo / p).is_file()},
    }
    receipt_path.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
    print(f"export_receipt={receipt_path} mirror_sha={mirror_sha} blobs={len(records)}")
    return 0


def cmd_verify(args: argparse.Namespace) -> int:
    receipt = json.loads(Path(args.receipt).read_text())
    repo = Path(args.mirror).resolve()
    head = run("git", "-C", str(repo), "rev-parse", "HEAD")
    recorded_head = receipt.get("mirror_commit", receipt.get("mirror_sha"))
    if head != recorded_head:
        print(f"identity=FAIL\n  mirror HEAD {head} != receipt mirror commit {recorded_head}")
        return 1
    expected = {row["mirror_path"]: row["git_blob_sha"] for row in receipt["records"]}
    actual = {row["path"]: row["blob"] for row in git_blob_rows(repo, head, expected)}
    bad = [path for path, blob in expected.items() if actual.get(path) != blob]
    if bad:
        print("identity=FAIL")
        for path in bad[:20]: print(f"  mismatch: {path}")
        return 1
    print(f"identity=PASS blobs={len(expected)}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("manifest"); p.add_argument("--source-root", required=True); p.add_argument("--build-dir", required=True); p.add_argument("--target", action="append", required=True); p.add_argument("--output", required=True); p.set_defaults(func=cmd_manifest)
    p = sub.add_parser("gate"); p.add_argument("--manifest", action="append", required=True); p.add_argument("--allow", action="append", default=[]); p.add_argument("--report-only", action="store_true"); p.set_defaults(func=cmd_gate)
    p = sub.add_parser("export"); p.add_argument("--repo", required=True); p.add_argument("--manifest", required=True); p.add_argument("--pulp-sha", required=True); p.add_argument("--output", required=True); p.add_argument("--mirror-root", default="mirror"); p.add_argument("--rename-map", type=json.loads, default=None); p.set_defaults(func=cmd_export)
    p = sub.add_parser("verify"); p.add_argument("--receipt", required=True); p.add_argument("--mirror", required=True); p.set_defaults(func=cmd_verify)
    args = parser.parse_args()
    return args.func(args)

if __name__ == "__main__":
    raise SystemExit(main())
