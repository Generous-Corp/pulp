#!/usr/bin/env python3
"""Replay release-cli.yml's CMake configure for every release leg, before merge.

The release workflow only runs when a tag is pushed, so a change that breaks
one leg's configure is first seen after it has landed and been tagged. This
script lets a pull request run that configure instead. It does not keep its
own copy of the release flags: it reads the `build-cli` job of
.github/workflows/release-cli.yml and replays the named `run:` steps (the
dependency bootstrap, the Skia fetch, the Rust and Node preparation, and every
`cmake -S` the leg runs), with the build commands inside those steps stubbed
out. A flag added to the release configure is therefore exercised here without
anyone remembering to copy it.

    release_configure_matrix.py scope --base SHA --head SHA --repo O/R
        prints `true` when the change can affect a release configure
    release_configure_matrix.py legs
        prints the matrix include list (one entry per active release platform)
    release_configure_matrix.py plan --platform P --out plan.json
        writes the replay plan for one leg (needs PyYAML)
    release_configure_matrix.py run-plan --plan plan.json
        executes a plan (standard library only)
    release_configure_matrix.py run --platform P
        plan + run-plan in one go, for a local reproduction

Steps run as GitHub runs a `shell: bash` step (`bash --noprofile --norc -eo
pipefail`), and lines the steps append to GITHUB_ENV and GITHUB_PATH carry to
the next step, so the replay sees the environment the release leg sees.

When a configure fails, the CMake error is printed as a workflow error
annotation and written to the step summary with the leg name and the local
reproduction command.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "ci"))
sys.path.insert(0, str(REPO / "tools" / "scripts"))

from release_path_changed import COMPARE_FILE_CAP, glob_regex  # noqa: E402

RELEASE_WORKFLOW = REPO / ".github" / "workflows" / "release-cli.yml"
JOB = "build-cli"

# Steps of release-cli.yml's build-cli job that this gate replays, in order.
# The replay stops after the last one, so everything the release leg does to
# reach its configure, and every configure it runs, is here.
REPLAYED_STEPS = (
    "Isolate the build from shared caches",
    "Bootstrap dependencies",
    "Purge host-arch Skia before Intel cross-fetch (darwin-x64)",
    "Fetch Skia binaries",
    "Verify fetched Skia is x86_64 (darwin-x64)",
    "Ensure a working Rust toolchain on PATH",
    "Provide the pinned Rust toolchain",
    "Prepare pinned Node runtime for design import",
    "Configure (CLI, no WebView on Linux)",
    "Prepare SDK build dir (Linux)",
    "Prepare SDK build dir (macOS)",
)

# Steps the gate deliberately does not replay, with the reason. Every build-cli
# step must be in exactly one of REPLAYED_STEPS, MIRRORED_STEPS or this table;
# test_release_configure_matrix.py fails when release-cli.yml gains a step
# that is in none of them, so a new pre-configure step cannot be missed.
SKIPPED_STEPS = {
    "Container pre-setup (ubuntu:22.04 — build toolchain the bare image lacks)":
        "installs the GCC 13 compile toolchain into the glibc-floor container; "
        "about 20 minutes with the Linux dependency install, for a compiler "
        "the configure does not judge. The gate configures linux-x64 on the "
        "bare runner image.",
    "Ensure shared Linux dependency action exists":
        "materializes the dependency action for old-tag backfills; a pull "
        "request checkout always has it",
    "Remove materialized Linux dependency action":
        "the counterpart of the backfill materialization above",
    "Overlay latest release-pipeline files (workflow_dispatch backfill)":
        "runs only on a workflow_dispatch backfill",
    "Ensure release-content verifier helpers exist":
        "selects packaging and verifier helpers; no CMake input reads them",
    "Build": "build, not configure",
    "Normalize CLI binary layout (Unix)": "after the build",
    "Normalize CLI binary layout (Windows)": "after the build",
    "Strip binaries (Unix)": "after the build",
    "Install Linux rpath helper": "packaging",
    "Package CLI (Unix)": "packaging",
    "Package CLI (Windows)": "packaging",
    "Report Linux SDK glibc floor (min-OS validation)": "reads built binaries",
    "Build SDK tarball (Unix)": "packaging",
    "Build SDK tarball (Windows)": "packaging",
    "Verify release archive product matrix (Unix)": "packaging",
    "Verify release archive product matrix (Windows)": "packaging",
    "Record macOS release floor": "after the build",
    "Attest CLI and SDK artifacts": "publishing",
    "Upload CLI artifact": "publishing",
    "Upload SDK artifact": "publishing",
}

# Non-`run:` steps the gate workflow performs itself, as the release does.
MIRRORED_STEPS = {
    "Check out release sources with upstream LF bytes":
        "release-configure.yml runs the same commands before its checkout, "
        "where they take effect (a replay after the checkout would be too late)",
    "actions/checkout": "release-configure.yml checks out with lfs: false",
    "Install Linux dependencies":
        "release-configure.yml runs ./.github/actions/install-linux-build-deps "
        "with the same profile",
}

# The build inside a replayed step is replaced by this line. Each replayed step
# must contain exactly the number of build commands listed here, so a new
# build or install command in a configure step fails the plan instead of
# running for an hour on a pull request.
BUILD_LINE = re.compile(r"^(\s*)PULP_BUILD_JOBS=\S+ tools/ci/governed-build\.sh cmake --build .*$",
                        re.MULTILINE)
EXPECTED_BUILD_LINES = {
    "Prepare SDK build dir (Linux)": 1,
    "Prepare SDK build dir (macOS)": 1,
}

# Release runners. The darwin-x64 leg is cross-compiled on Apple Silicon in the
# release too (release_build_matrix.py), so its sentinel maps to the same
# hosted arm64 image as darwin-arm64.
HOSTED_RUNNER = {
    "macos-15": "macos-15",
    "macos-15-xcompile": "macos-15",
    "ubuntu-24.04": "ubuntu-24.04",
    "ubuntu-24.04-arm": "ubuntu-24.04-arm",
    "windows-latest": "windows-latest",
    "windows-11-arm": "windows-11-arm",
}

RUNNER_OS = {"darwin": "macOS", "linux": "Linux", "windows": "Windows"}

# Paths whose change can alter a release leg's configure. The trigger itself is
# unfiltered (a required check must always report); this list decides whether
# the legs run or the aggregate passes as out of scope.
SCOPE_GLOBS = (
    "CMakeLists.txt",
    "**/CMakeLists.txt",
    "**/*.cmake",
    "**/*.cmake.in",
    "**/*.in",
    "tools/cmake/**",
    "tools/deps/**",
    "tools/toolchain/**",
    "external/**",
    ".gitmodules",
    "setup.sh",
    ".mise.toml",
    ".mise.toolchain.toml",
    "mise.toolchain.lock",
    "experimental/pulp-rs/rust-toolchain.toml",
    "tools/ci/dependency_pin_map.json",
    "tools/ci/install_linux_build_deps.py",
    "tools/ci/linux_build_deps.json",
    "tools/ci/provide_pinned_rust.sh",
    "tools/ci/release_configure_matrix.py",
    "tools/ci/release_path_changed.py",
    ".github/actions/install-linux-build-deps/**",
    ".github/workflows/release-cli.yml",
    ".github/workflows/release-configure.yml",
    "tools/scripts/checkout_preflight.py",
    "tools/scripts/fetch_skia_for_release.py",
    "tools/scripts/prepare_node_runtime.py",
    "tools/scripts/release_build_matrix.py",
    "tools/scripts/release_product_matrix.json",
    "tools/scripts/release_artifact_contents.py",
)
# The release profile configures tests and examples OFF, so their CMake files
# never reach a release configure.
SCOPE_EXCLUDES = ("test/**", "examples/**")

# The version-at-land bump rewrites one line of the root CMakeLists.txt. A
# change to that line alone cannot change a configure's outcome.
VERSION_LINE = re.compile(r"^\s*VERSION\s+\d+\.\d+\.\d+\s*$")


class PlanError(RuntimeError):
    pass


# ── scope ──────────────────────────────────────────────────────────────────


def in_scope(path: str) -> bool:
    if any(glob_regex(g).match(path) for g in SCOPE_EXCLUDES):
        return False
    return any(glob_regex(g).match(path) for g in SCOPE_GLOBS)


def version_line_only(patch: str | None) -> bool:
    """True when a root CMakeLists.txt patch changes only the project VERSION."""
    if not patch:
        return False
    changed = [
        line[1:]
        for line in patch.splitlines()
        if line[:1] in "+-" and not line.startswith(("+++", "---"))
    ]
    return bool(changed) and all(VERSION_LINE.match(line) for line in changed)


def decide(files: list[dict]) -> tuple[bool, list[str]]:
    """Return (run, reasons) for compare-API file entries {filename, patch}."""
    if len(files) >= COMPARE_FILE_CAP:
        return True, [f"{len(files)} changed files: the compare API stops listing at "
                      f"{COMPARE_FILE_CAP}, so nothing can be ruled out"]
    hits = []
    for entry in files:
        name = entry.get("filename", "")
        if not in_scope(name):
            continue
        if name == "CMakeLists.txt" and version_line_only(entry.get("patch")):
            continue
        hits.append(name)
    return bool(hits), hits


def compare_files(repo: str, base: str, head: str) -> list[dict]:
    gh = shutil.which("gh")
    if not gh:
        raise PlanError("gh is not on PATH")
    out = subprocess.run(
        [gh, "api", f"repos/{repo}/compare/{base}...{head}",
         "--jq", "[.files[] | {filename, patch}]"],
        check=True, capture_output=True, text=True,
    ).stdout
    return json.loads(out or "[]")


def cmd_scope(args: argparse.Namespace) -> int:
    if args.files:
        files = json.loads(Path(args.files).read_text(encoding="utf-8"))
    else:
        try:
            files = compare_files(args.repo, args.base, args.head)
        except (PlanError, subprocess.CalledProcessError, json.JSONDecodeError) as exc:
            # A scope that cannot be read runs the legs: a skipped required
            # check must never be the result of an unreadable diff.
            print(f"scope unreadable ({exc}); running every leg", file=sys.stderr)
            print("true")
            return 0
    run, reasons = decide(files)
    for reason in reasons:
        print(f"in scope: {reason}", file=sys.stderr)
    if not run:
        print("no changed path can affect a release configure", file=sys.stderr)
    print("true" if run else "false")
    return 0


# ── legs ───────────────────────────────────────────────────────────────────


def legs() -> list[dict[str, str]]:
    from release_artifact_contents import DEFAULT_MATRIX_PATH, ProductMatrix
    from release_build_matrix import build_include

    out = []
    for leg in build_include(ProductMatrix.load(DEFAULT_MATRIX_PATH)):
        runner = HOSTED_RUNNER.get(leg["os"])
        if runner is None:
            raise PlanError(f"release leg {leg['platform']} uses runner label {leg['os']!r}, "
                            "which has no hosted equivalent in HOSTED_RUNNER")
        out.append({"platform": leg["platform"], "runs_on": runner})
    return out


def cmd_legs(args: argparse.Namespace) -> int:
    include = legs()
    if args.github_output:
        print(f"include={json.dumps(include)}")
    else:
        print(json.dumps(include, indent=2))
    return 0


# ── plan ───────────────────────────────────────────────────────────────────


def runner_os(platform: str) -> str:
    try:
        return RUNNER_OS[platform.split("-", 1)[0]]
    except KeyError as exc:
        raise PlanError(f"unknown platform {platform!r}") from exc


def release_tag() -> str:
    """A tag-shaped label for this checkout's version, as a tag push sees it."""
    text = (REPO / "CMakeLists.txt").read_text(encoding="utf-8")
    m = re.search(r"project\(\s*Pulp\b[^)]*?\bVERSION\s+(\d+\.\d+\.\d+)", text, re.S)
    if not m:
        raise PlanError("cannot read the project VERSION from CMakeLists.txt")
    return f"v{m.group(1)}"


class Context:
    """The expression context of a tag-push run of one release leg."""

    def __init__(self, platform: str, tag: str):
        self.values = {
            "matrix.platform": platform,
            # The gate configures linux-x64 on the bare image (see SKIPPED_STEPS).
            "matrix.container": "",
            "runner.os": runner_os(platform),
            "github.event_name": "push",
            "github.ref_name": tag,
            "inputs.version": "",
            "inputs.source_ref": "",
        }

    def atom(self, text: str) -> str | bool:
        text = text.strip()
        m = re.fullmatch(r"'([^']*)'", text)
        if m:
            return m.group(1)
        m = re.fullmatch(r"hashFiles\('([^']+)'\)", text)
        if m:
            return "present" if (REPO / m.group(1)).is_file() else ""
        if text in self.values:
            return self.values[text]
        raise PlanError(f"unsupported expression term {text!r}")

    def compare(self, text: str) -> str | bool:
        m = re.fullmatch(r"(.+?)\s*(==|!=)\s*(.+)", text.strip())
        if not m:
            return self.atom(text)
        left, op, right = self.atom(m.group(1)), m.group(2), self.atom(m.group(3))
        return (left == right) if op == "==" else (left != right)

    def evaluate(self, expr: str) -> str | bool:
        """GitHub's `a && b || c` subset: && binds tighter, values short-circuit."""
        expr = expr.strip()
        if expr.startswith("${{") and expr.endswith("}}"):
            expr = expr[3:-2].strip()
        if "(" in expr.replace("hashFiles(", ""):
            raise PlanError(f"unsupported grouping in expression {expr!r}")
        result: str | bool = False
        for alternative in re.split(r"\s*\|\|\s*", expr):
            value: str | bool = True
            for term in re.split(r"\s*&&\s*", alternative):
                value = self.compare(term)
                if not value:
                    break
            if value:
                return value
            result = value
        return result

    def substitute(self, text: str) -> str:
        def repl(m: re.Match[str]) -> str:
            value = self.evaluate(m.group(1))
            if isinstance(value, bool):
                return "true" if value else "false"
            return value
        return re.sub(r"\$\{\{(.*?)\}\}", repl, text)


def load_job(workflow: Path) -> dict:
    try:
        import yaml
    except ImportError as exc:  # pragma: no cover - environment dependent
        raise PlanError("planning needs PyYAML (python3 -m pip install pyyaml)") from exc
    doc = yaml.safe_load(workflow.read_text(encoding="utf-8"))
    return doc["jobs"][JOB]


def step_key(step: dict) -> str:
    if "name" in step:
        return step["name"]
    uses = step.get("uses", "")
    return uses.split("@", 1)[0]


def unclassified_steps(job: dict) -> list[str]:
    """build-cli steps this gate neither replays, mirrors nor skips by name."""
    known = set(REPLAYED_STEPS) | set(MIRRORED_STEPS) | set(SKIPPED_STEPS)
    return [step_key(s) for s in job["steps"] if step_key(s) not in known]


def plan(platform: str, workflow: Path = RELEASE_WORKFLOW) -> dict:
    job = load_job(workflow)
    steps = job["steps"]
    job_env = {k: str(v) for k, v in (job.get("env") or {}).items()}

    unknown = unclassified_steps(job)
    if unknown:
        raise PlanError(f"release-cli.yml {JOB} has step(s) this gate does not classify: "
                        f"{unknown}. Add each to REPLAYED_STEPS (it can change a configure), "
                        "MIRRORED_STEPS or SKIPPED_STEPS in tools/ci/release_configure_matrix.py")
    by_name = {step_key(s): s for s in steps}
    missing = [name for name in REPLAYED_STEPS if name not in by_name]
    if missing:
        raise PlanError(f"release-cli.yml {JOB} has no step named {missing}; "
                        "update REPLAYED_STEPS in tools/ci/release_configure_matrix.py")

    tag = release_tag()
    ctx = Context(platform, tag)
    planned = []
    for name in REPLAYED_STEPS:
        step = by_name[name]
        if "run" not in step:
            raise PlanError(f"replayed step {name!r} is not a run step")
        shell = step.get("shell", "bash")
        if shell != "bash":
            raise PlanError(f"replayed step {name!r} uses shell {shell!r}; only bash is replayed")
        condition = step.get("if")
        if condition is not None and not ctx.evaluate(str(condition)):
            continue
        script = ctx.substitute(step["run"])
        expected = EXPECTED_BUILD_LINES.get(name, 0)
        script, count = BUILD_LINE.subn(
            r'\1echo "release-configure: build skipped (configure-only replay)"', script)
        if count != expected:
            raise PlanError(f"step {name!r} has {count} build command(s), expected {expected}; "
                            "update EXPECTED_BUILD_LINES after checking it builds nothing else")
        commands = "\n".join(line for line in script.splitlines()
                              if not line.lstrip().startswith("#"))
        if re.search(r"cmake\s+--build|cmake\s+--install|\bctest\b", commands):
            raise PlanError(f"step {name!r} still builds, installs or tests after stubbing")
        env = {k: ctx.substitute(str(v)) for k, v in (step.get("env") or {}).items()}
        planned.append({"name": name, "script": script, "env": env})

    configures = sum(1 for s in planned if re.search(r"cmake\s+-S\s", s["script"]))
    if configures == 0:
        raise PlanError(f"plan for {platform} runs no CMake configure")
    return {
        "platform": platform,
        "runner_os": ctx.values["runner.os"],
        "release_tag": tag,
        "job_env": job_env,
        "steps": planned,
    }


def cmd_plan(args: argparse.Namespace) -> int:
    data = plan(args.platform, Path(args.workflow))
    Path(args.out).write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    print(f"{args.platform}: {len(data['steps'])} steps, "
          f"{sum(1 for s in data['steps'] if 'cmake -S' in s['script'])} configure step(s)")
    return 0


# ── run ────────────────────────────────────────────────────────────────────


def find_bash() -> str:
    if os.name == "nt":
        # `bash` on a Windows PATH can be WSL's launcher; GitHub's bash shell
        # is Git for Windows'.
        for candidate in (r"C:\Program Files\Git\bin\bash.exe",
                          r"C:\Program Files\Git\usr\bin\bash.exe"):
            if Path(candidate).is_file():
                return candidate
    found = shutil.which("bash")
    if not found:
        raise PlanError("bash is not on PATH")
    return found


def read_env_file(path: Path) -> dict[str, str]:
    """Parse GITHUB_ENV syntax: KEY=VALUE and KEY<<DELIM ... DELIM."""
    out: dict[str, str] = {}
    if not path.is_file():
        return out
    lines = path.read_text(encoding="utf-8").splitlines()
    i = 0
    while i < len(lines):
        line = lines[i]
        i += 1
        if not line.strip():
            continue
        if "<<" in line and ("=" not in line or line.index("<<") < line.index("=")):
            key, delim = line.split("<<", 1)
            value = []
            while i < len(lines) and lines[i] != delim:
                value.append(lines[i])
                i += 1
            i += 1
            out[key] = "\n".join(value)
        elif "=" in line:
            key, value = line.split("=", 1)
            out[key] = value
        else:
            raise PlanError(f"unparseable GITHUB_ENV line {line!r}")
    return out


def skia_guard() -> None:
    """Refuse to replay the Skia purge through a symlink into a shared cache."""
    skia = REPO / "external" / "skia-build"
    for path in (skia, skia / "build"):
        if path.is_symlink():
            raise PlanError(f"{path} is a symlink; the darwin-x64 purge would delete "
                            "through it. Run the replay from a checkout with a real "
                            "external/skia-build directory.")


CMAKE_ERROR = re.compile(r"^CMake Error")


def cmake_errors(log: str) -> list[str]:
    """Each `CMake Error` line plus its indented message paragraph."""
    blocks: list[str] = []
    lines = log.splitlines()
    for i, line in enumerate(lines):
        if not CMAKE_ERROR.match(line):
            continue
        block = [line]
        for follow in lines[i + 1:]:
            if follow.strip() == "" and len(block) > 1:
                break
            if follow.strip() == "":
                continue
            if not follow.startswith((" ", "\t")):
                break
            block.append(follow)
        blocks.append("\n".join(block))
    return blocks


def annotate(platform: str, step: str, blocks: list[str], rc: int) -> None:
    repro = f"python3 tools/ci/release_configure_matrix.py run --platform {platform}"
    title = f"Release {platform} configure fails: this change would break every release tag"
    detail = blocks[0] if blocks else f"step exited {rc} (no CMake Error line in the log)"
    escaped = detail.replace("%", "%25").replace("\r", "").replace("\n", "%0A")
    print(f"::error title={title}::{step}%0A{escaped}%0AReproduce: {repro}")
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as fh:
            fh.write(f"## :x: Release leg `{platform}` would fail at configure\n\n")
            fh.write(f"Step replayed from `release-cli.yml`: **{step}** (exit {rc}).\n\n")
            fh.write("release-cli.yml's `release` job needs every leg, so a tag cut from "
                     "this change would publish nothing.\n\n")
            for block in blocks[:3] or [detail]:
                fh.write(f"```\n{block}\n```\n\n")
            fh.write(f"Reproduce on a matching host:\n\n```\n{repro}\n```\n")


def run_plan(data: dict, workdir: Path) -> int:
    bash = find_bash()
    platform = data["platform"]
    env = dict(os.environ)
    env.update(data.get("job_env", {}))
    env.setdefault("RUNNER_TEMP", str(workdir / "runner-temp"))
    Path(env["RUNNER_TEMP"]).mkdir(parents=True, exist_ok=True)
    env.setdefault("GITHUB_RUN_ID", "local")
    env.setdefault("GITHUB_RUN_ATTEMPT", "1")
    in_actions = os.environ.get("GITHUB_ACTIONS") == "true"
    for index, step in enumerate(data["steps"]):
        name = step["name"]
        skia_guard()
        script = workdir / f"step-{index:02d}.sh"
        script.write_text(step["script"], encoding="utf-8", newline="\n")
        env_file = workdir / f"step-{index:02d}.env"
        path_file = workdir / f"step-{index:02d}.path"
        env_file.write_text("", encoding="utf-8")
        path_file.write_text("", encoding="utf-8")
        step_env = dict(env)
        step_env.update(step.get("env", {}))
        step_env["GITHUB_ENV"] = str(env_file)
        step_env["GITHUB_PATH"] = str(path_file)
        print(f"::group::{platform}: {name}" if in_actions else f"==> {platform}: {name}",
              flush=True)
        log_path = workdir / f"step-{index:02d}.log"
        with open(log_path, "w", encoding="utf-8", errors="replace") as log:
            proc = subprocess.Popen(
                [bash, "--noprofile", "--norc", "-eo", "pipefail", script.as_posix()],
                cwd=REPO, env=step_env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, encoding="utf-8", errors="replace",
            )
            assert proc.stdout is not None
            for line in proc.stdout:
                sys.stdout.write(line)
                log.write(line)
            rc = proc.wait()
            proc.stdout.close()
        if in_actions:
            print("::endgroup::", flush=True)
        if rc != 0:
            blocks = cmake_errors(log_path.read_text(encoding="utf-8", errors="replace"))
            annotate(platform, name, blocks, rc)
            print(f"FAIL: {platform}: {name} exited {rc}", file=sys.stderr)
            return 1
        env.update(read_env_file(env_file))
        for line in path_file.read_text(encoding="utf-8").splitlines():
            if line.strip():
                env["PATH"] = line.strip() + os.pathsep + env["PATH"]
    print(f"OK: {platform}: every release configure step passed "
          f"({len(data['steps'])} steps replayed from release-cli.yml)")
    return 0


def cmd_run_plan(args: argparse.Namespace) -> int:
    data = json.loads(Path(args.plan).read_text(encoding="utf-8"))
    with tempfile.TemporaryDirectory(prefix="release-configure-") as tmp:
        return run_plan(data, Path(tmp))


def cmd_run(args: argparse.Namespace) -> int:
    data = plan(args.platform, Path(args.workflow))
    with tempfile.TemporaryDirectory(prefix="release-configure-") as tmp:
        return run_plan(data, Path(tmp))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("scope")
    p.add_argument("--repo")
    p.add_argument("--base")
    p.add_argument("--head")
    p.add_argument("--files", help="JSON list of {filename, patch} (instead of the API)")
    p.set_defaults(func=cmd_scope)

    p = sub.add_parser("legs")
    p.add_argument("--github-output", action="store_true")
    p.set_defaults(func=cmd_legs)

    for name, func in (("plan", cmd_plan), ("run", cmd_run)):
        p = sub.add_parser(name)
        p.add_argument("--platform", required=True)
        p.add_argument("--workflow", default=str(RELEASE_WORKFLOW))
        if name == "plan":
            p.add_argument("--out", required=True)
        p.set_defaults(func=func)

    p = sub.add_parser("run-plan")
    p.add_argument("--plan", required=True)
    p.set_defaults(func=cmd_run_plan)

    args = parser.parse_args(argv)
    if args.command == "scope" and not args.files and not (args.repo and args.base and args.head):
        parser.error("scope needs --files, or --repo, --base and --head")
    try:
        return args.func(args)
    except PlanError as exc:
        print(f"::error title=release configure plan::{exc}"
              if os.environ.get("GITHUB_ACTIONS") == "true" else f"error: {exc}",
              file=sys.stderr if os.environ.get("GITHUB_ACTIONS") != "true" else sys.stdout)
        return 2


if __name__ == "__main__":
    sys.exit(main())
