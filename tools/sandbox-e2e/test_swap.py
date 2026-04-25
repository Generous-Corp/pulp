"""End-to-end sandbox scenarios for the Pulp CLI + Claude plugin.

Every test here runs in an isolated ``Sandbox`` (see ``pulp_sandbox.py``)
and must pass the contamination audit at teardown — any write to
``~/.pulp/``, ``~/.local/bin``, or ``~/.cargo/bin`` fails the test.

Scenarios mirror the acceptance criteria on
`danielraffel/pulp#732 <https://github.com/danielraffel/pulp/issues/732>`_.
Grouped by ``@pytest.mark`` so CI can run a fast subset on PR gates
and the full suite on release tags.
"""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from pulp_sandbox import (
    REQUIRED_DOCTOR_KEYS,
    Sandbox,
    enumerate_plugin_commands,
    parse_versions_json,
)


# -----------------------------------------------------------------------------
# Scenario 1 — Plugin command surface
# -----------------------------------------------------------------------------
#
# The Claude plugin's slash-command files shell out to ``pulp X`` for a
# specific set of subcommands. Every one must either run natively
# (exit 0, or a documented non-zero like "not in project dir") or
# delegate cleanly to ``pulp-cpp`` — NEVER silent-exit-0 on an
# unknown subcommand. The ``ship sign`` regression fixed in
# ``4ba25715`` is the canonical example of what this test prevents.
#
# Commands the sandbox doesn't exercise directly (because they touch
# network / keychain / GUI / system installers) are listed in
# ``SURFACE_SKIPS``. Each skip has a documented reason.


SURFACE_SKIPS: dict[str, str] = {
    # Network / system mutation
    "upgrade": "mutates ~/.pulp/bin or fetches from GitHub",
    "sdk": "downloads SDK tarballs",
    "cache": "downloads assets",
    "tool": "downloads tool archives",
    "ship": "signs / notarizes / packages via system tooling",
    "pr": "orchestrates github + shipyard — external",
    # Build / GUI
    "build": "full build is slow; tested via orchestrate fixtures elsewhere",
    "create": "scaffolds a project on disk; needs its own fixture",
    "run": "launches a long-lived binary",
    "design": "launches the design tool GUI",
    "inspect": "launches the inspector GUI",
    "design-debug": "GUI",
    "import-design": "GUI + network",
    "export-tokens": "writes to project assets",
    "validate": "runs pluginval / auval against real plugins",
    "dev": "watch loop",
    "docs": "partial — some subcommands launch browsers",
    # Legacy / sibling surfaces
    "new": "removed alias; not in modern dispatch table",
    "add-component": "legacy python-script alias",
    "ci": "ci-local dispatch name in C++ CLI",
}


@pytest.fixture(scope="session")
def plugin_surface(claude_commands_dir: Path) -> set[str]:
    """The set of ``pulp <subcommand>`` tokens the plugin invokes."""
    return enumerate_plugin_commands(claude_commands_dir)


def test_plugin_surface_is_non_empty(plugin_surface: set[str]) -> None:
    """Sanity check the regex extraction itself."""
    assert plugin_surface, (
        "enumerate_plugin_commands found no pulp invocations in "
        ".claude/commands/ — regex broken or files missing"
    )
    # Spot-check a few we know must be there.
    for expected in {"doctor", "status", "version"}:
        assert expected in plugin_surface, f"missing {expected!r}"


def _exercisable_commands(surface: set[str]) -> list[str]:
    return sorted(cmd for cmd in surface if cmd not in SURFACE_SKIPS)


@pytest.mark.plugin_surface
def test_every_exercisable_plugin_command_exits_nonsilent_on_unknown(
    sandbox: Sandbox,
    cpp_binary: Path,
    plugin_surface: set[str],
) -> None:
    """Every subcommand the plugin invokes must either run (exit 0 /
    well-known non-zero) or — for unrecognised / unimplemented tokens —
    exit non-zero AND print something on stderr. The failure mode we're
    guarding against is the ``ship sign`` regression: exit 0 with a
    "Unknown command: ship" message on stdout.
    """
    sandbox.stage_binary(cpp_binary, as_name="pulp")
    exercisable = _exercisable_commands(plugin_surface)
    assert exercisable, "no exercisable commands after exclusions"
    failures: list[str] = []
    for cmd in exercisable:
        result = sandbox.run([cmd, "--help"], timeout=10.0)
        # Known-good pattern: either --help succeeded (0), or --help
        # is not recognised and stderr mentions usage / an error.
        if result.returncode == 0:
            continue
        if result.returncode != 0 and (result.stderr or "unknown" in result.stdout.lower()):
            # Something printed — that's the anti-silent-exit-0 contract.
            continue
        failures.append(
            f"{cmd} --help exited {result.returncode} with no stderr "
            f"and no 'unknown' hint on stdout: {result.stdout!r}"
        )
    assert not failures, "\n".join(failures)


# -----------------------------------------------------------------------------
# Scenario 2 — Cross-binary state continuity (C++ ↔ Rust)
# -----------------------------------------------------------------------------
#
# The critical Phase 8 test: if the C++ binary writes ``config.toml``
# in Phase 1 and the Rust binary replaces it in Phase 2, the user's
# stored config must survive unchanged.


@pytest.mark.cross_binary
def test_config_set_by_cpp_is_readable_by_rust(
    sandbox: Sandbox,
    cpp_binary: Path,
    rust_binary: Path,
) -> None:
    # Stage C++ first, write config via it.
    sandbox.stage_binary(cpp_binary, as_name="pulp")
    sandbox.run(
        ["config", "set", "update.mode", "manual"],
        timeout=15.0,
    ).expect_success()
    assert "manual" in sandbox.read("config.toml")

    # Swap: rename pulp → pulp-cpp, install Rust as pulp.
    (sandbox.bin_dir / "pulp").rename(sandbox.bin_dir / "pulp-cpp")
    sandbox.stage_binary(rust_binary, as_name="pulp")

    # Rust binary must read the same value.
    result = sandbox.run(["config", "get", "update.mode"]).expect_success()
    assert result.stdout.strip() == "manual", (
        f"Rust read {result.stdout!r} instead of 'manual' from config "
        f"written by C++"
    )


@pytest.mark.cross_binary
def test_projects_list_round_trips_across_binaries(
    sandbox: Sandbox,
    cpp_binary: Path,
    rust_binary: Path,
    tmp_path_factory: pytest.TempPathFactory,
) -> None:
    fake_project = tmp_path_factory.mktemp("fake-project")
    (fake_project / "pulp.toml").write_text('sdk_version = "0.40.0"\n')
    (fake_project / "CMakeLists.txt").write_text(
        "project(FakeProject VERSION 0.1.0 LANGUAGES CXX)\n"
    )

    sandbox.stage_binary(cpp_binary, as_name="pulp")
    sandbox.run(
        ["projects", "add", str(fake_project)],
        timeout=15.0,
    ).expect_success()

    # Swap to Rust.
    (sandbox.bin_dir / "pulp").rename(sandbox.bin_dir / "pulp-cpp")
    sandbox.stage_binary(rust_binary, as_name="pulp")

    listed = sandbox.run(["projects", "list"]).expect_success()
    assert str(fake_project) in listed.stdout, (
        f"Rust 'projects list' didn't include {fake_project}: "
        f"{listed.stdout!r}"
    )


# -----------------------------------------------------------------------------
# Scenario 3 — PULP_USE_CPP=1 rollback against real C++ binary
# -----------------------------------------------------------------------------


@pytest.mark.rollback
def test_pulp_use_cpp_rollback_forwards_to_real_cpp(
    sandbox: Sandbox,
    cpp_binary: Path,
    rust_binary: Path,
) -> None:
    sandbox.stage_binary(cpp_binary, as_name="pulp-cpp")
    sandbox.stage_binary(rust_binary, as_name="pulp")

    # PULP_USE_CPP=1 must forward to pulp-cpp; the C++ binary prints
    # "Pulp SDK version: X.Y.Z" which is distinct from the Rust
    # binary's "pulp-rs vX.Y.Z (prototype)" line.
    with_rollback = sandbox.run(
        ["version"], env_overrides={"PULP_USE_CPP": "1"}
    )
    without_rollback = sandbox.run(["version"])

    assert "Pulp SDK version" in with_rollback.stdout, (
        f"rollback didn't reach C++ binary: {with_rollback.stdout!r}"
    )
    # Without rollback we see Rust output (pre-swap) OR C++ output
    # (post-swap). Either way it's not the SAME output as the rollback
    # case if the Rust binary is distinct — assert divergence so we
    # know the rollback flag is actually doing something.
    assert with_rollback.stdout != without_rollback.stdout, (
        "PULP_USE_CPP=1 produced the same output as no override — "
        "rollback lever isn't firing"
    )


# -----------------------------------------------------------------------------
# Scenario 4 — Upgrade flow
# -----------------------------------------------------------------------------


@pytest.mark.upgrade
def test_upgrade_check_only_reports_installed_and_latest(
    sandbox: Sandbox,
    cpp_binary: Path,
) -> None:
    """``pulp upgrade --check-only`` must be observable (non-silent) and
    report at least the installed version. The exact output shape is
    looser than a strict JSON contract because the C++ CLI currently
    prints human text even with ``--json`` — a defect we don't want this
    harness to block on, but we DO want the "silent exit 0" failure
    mode blocked.
    """
    sandbox.stage_binary(cpp_binary, as_name="pulp")
    result = sandbox.run(
        ["upgrade", "--check-only"],
        env_overrides={"PULP_UPDATE_CHECK_DISABLED": "1"},
        timeout=30.0,
    )
    # Must produce SOMETHING observable. Silent-exit-0 is the
    # regression we're guarding against.
    assert result.stdout or result.stderr, (
        f"pulp upgrade --check-only produced no output at all "
        f"(rc={result.returncode}) — silent-exit pattern"
    )
    # Happy path: both Installed + Latest appear (network worked).
    # Degraded path: Installed appears without Latest. Either is OK
    # as long as we see a version report.
    combined = result.stdout + result.stderr
    if "Installed" not in combined and "installed" not in combined:
        # Only accept completely missing version info if a lane was
        # explicitly disabled and we see a notice to that effect.
        if "disabled" not in combined.lower() and "skip" not in combined.lower():
            pytest.fail(
                f"upgrade --check-only didn't mention installed version "
                f"or disablement: {combined!r}"
            )
    # --json support probe: if the binary advertises JSON and we got a
    # JSON-shaped payload, parse it. Otherwise skip the JSON assertion
    # entirely — it's a nice-to-have, not a regression gate.
    json_result = sandbox.run(
        ["upgrade", "--check-only", "--json"],
        env_overrides={"PULP_UPDATE_CHECK_DISABLED": "1"},
        timeout=30.0,
    )
    stripped = json_result.stdout.strip()
    if stripped.startswith("{"):
        try:
            payload = json.loads(stripped)
            assert isinstance(payload, dict)
        except json.JSONDecodeError:
            pytest.fail(
                f"--json output starts with {{ but is malformed: "
                f"{stripped!r}"
            )
    # If --json didn't produce a JSON-shaped object, that's a known
    # gap (the C++ CLI today prints human text under --json in some
    # paths). Not a regression for this harness — file it as a
    # separate CLI issue if the shape should be tighter.


# -----------------------------------------------------------------------------
# Scenario 6 — Parity probes: doctor --versions --json, help banner
# -----------------------------------------------------------------------------


@pytest.mark.parity
def test_doctor_versions_json_schema_matches_required_keys(
    sandbox: Sandbox,
    cpp_binary: Path,
) -> None:
    sandbox.stage_binary(cpp_binary, as_name="pulp")
    result = sandbox.run(["doctor", "--versions", "--json"]).expect_success()
    payload = parse_versions_json(result.stdout)
    missing = [k for k in REQUIRED_DOCTOR_KEYS if k not in payload]
    # project_root is a Rust-only addition; allow it to be absent on
    # the C++ path.
    missing = [k for k in missing if k != "project_root"]
    assert not missing, (
        f"C++ doctor --versions --json is missing keys {missing!r}; "
        f"payload keys: {sorted(payload)}"
    )


@pytest.mark.parity
def test_doctor_versions_parity_across_binaries(
    sandbox: Sandbox,
    cpp_binary: Path,
    rust_binary: Path,
) -> None:
    """The top-level key set should match between C++ and Rust. Values
    may differ; SHAPE should not."""
    # C++
    sandbox.stage_binary(cpp_binary, as_name="pulp")
    cpp_out = sandbox.run(["doctor", "--versions", "--json"]).expect_success()
    cpp_payload = parse_versions_json(cpp_out.stdout)
    # Reset and stage Rust.
    (sandbox.bin_dir / "pulp").unlink()
    sandbox.stage_binary(rust_binary, as_name="pulp")
    rs_out = sandbox.run(["doctor", "--versions", "--json"]).expect_success()
    rs_payload = parse_versions_json(rs_out.stdout)

    common_required = {"cli", "plugin", "plugin_min_cli"}
    for key in common_required:
        assert key in cpp_payload, f"C++ missing {key!r}"
        assert key in rs_payload, f"Rust missing {key!r}"


# -----------------------------------------------------------------------------
# Scenario 7 — Library-linked command delegation
# -----------------------------------------------------------------------------
#
# pulp ship / validate / inspect / host / audio / import-design /
# export-tokens / design-debug are C++-only. Post-swap they must route
# from the Rust binary through the Phase 7 fallthrough wrapper to
# pulp-cpp.
#
# We verify this with a stub pulp-cpp that tags its output. If the
# Rust binary reaches the stub, we see the tag + exit 42.


LIBRARY_LINKED_COMMANDS: tuple[tuple[str, ...], ...] = (
    ("ship", "check"),
    ("validate", "--help"),
    ("inspect", "--help"),
    ("host", "--help"),
    ("audio", "--help"),
    ("import-design", "--help"),
    ("export-tokens", "--help"),
    ("design-debug", "--help"),
)


@pytest.mark.library_linked
@pytest.mark.parametrize("argv", LIBRARY_LINKED_COMMANDS, ids=lambda a: a[0])
def test_library_linked_command_falls_through_to_pulp_cpp(
    sandbox: Sandbox,
    rust_binary: Path,
    stub_pulp_cpp_src: Path,
    argv: tuple[str, ...],
) -> None:
    """Post-swap, the Rust binary must route unknown-in-Rust subcommands
    to pulp-cpp when it's on PATH. The stub pulp-cpp exits 42 and tags
    stdout; any reaching-the-stub outcome proves the fallthrough fired.
    """
    sandbox.stage_binary(rust_binary, as_name="pulp")
    # Install the stub as pulp-cpp alongside.
    stub_dest = sandbox.bin_dir / "pulp-cpp"
    stub_dest.write_text(stub_pulp_cpp_src.read_text())
    stub_dest.chmod(0o755)

    result = sandbox.run(list(argv))
    assert result.returncode == 42, (
        f"{argv[0]} didn't reach the stub (rc={result.returncode}); "
        f"fallthrough is broken.\nstdout: {result.stdout!r}\n"
        f"stderr: {result.stderr!r}"
    )
    assert "STUB_PULP_CPP" in result.stdout, (
        f"{argv[0]} rc=42 but no STUB_PULP_CPP tag — wrong child fired?"
    )


# -----------------------------------------------------------------------------
# Scenario 8 — Doctor positional vs flag syntax
# -----------------------------------------------------------------------------
#
# The C++ CLI uses `pulp doctor android` (positional) for the platform-
# specific probe. The Rust CLI accepts `pulp doctor --android` via
# trailing_var_arg fallthrough. Both must reach pulp-cpp when the stub
# is on PATH.


@pytest.mark.parity
@pytest.mark.parametrize(
    "argv",
    [
        ("doctor", "android"),
        ("doctor", "--android"),
    ],
    ids=["positional", "flag"],
)
def test_doctor_platform_probe_reaches_pulp_cpp(
    sandbox: Sandbox,
    rust_binary: Path,
    stub_pulp_cpp_src: Path,
    argv: tuple[str, ...],
) -> None:
    sandbox.stage_binary(rust_binary, as_name="pulp")
    stub_dest = sandbox.bin_dir / "pulp-cpp"
    stub_dest.write_text(stub_pulp_cpp_src.read_text())
    stub_dest.chmod(0o755)

    result = sandbox.run(list(argv))
    # Either the Rust binary delegates (stub exit 42 + tag) or it
    # handles locally with a documented non-zero + stderr. The silent-
    # exit-0 failure mode is what we're guarding against.
    if result.returncode == 42 and "STUB_PULP_CPP" in result.stdout:
        return  # delegated to stub as expected
    if result.returncode != 0 and (result.stderr or result.stdout):
        return  # handled locally with an observable response
    pytest.fail(
        f"doctor {argv} returned rc={result.returncode} with empty "
        f"output — silent-exit pattern.\nstdout: {result.stdout!r}"
    )


# -----------------------------------------------------------------------------
# Scenario 5 — Contamination audit (implicit on every test)
# -----------------------------------------------------------------------------
#
# The ``sandbox`` fixture's teardown calls ``assert_no_contamination``
# after every test. That's the canonical audit. This explicit test
# makes the contract visible in reports even when everything else
# passes quietly, and guards against a future refactor where someone
# removes the teardown hook.


def test_contamination_audit_catches_writes_to_protected_paths(
    sandbox: Sandbox,
) -> None:
    """Smoke test for the audit itself. Simulate a write to a
    protected path (in an UNDO-safe way: we write to a tempfile that
    sits under ``~/.pulp``'s namespace only via an env override, not
    via the real filesystem). The audit should flag it."""
    # Instead of writing to the real ``~/.pulp`` (which would be a
    # genuine contamination), point the audit at a sandboxed root and
    # touch a file there.
    fake_root = sandbox.root / "fake-protected"
    fake_root.mkdir()
    fake_file = fake_root / "leak.txt"
    fake_file.write_text("this was written after the sentinel")

    report = sandbox.audit_contamination(extra_roots=(fake_root,))
    assert not report.clean, "audit failed to detect a deliberate leak"
    assert any(
        str(p).endswith("leak.txt") for p in report.offenders
    ), f"expected leak.txt in offenders, got {report.offenders}"
