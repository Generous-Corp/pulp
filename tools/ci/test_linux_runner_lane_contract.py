#!/usr/bin/env python3
"""Lane-identity contract for the Proxmox ephemeral Linux runners.

Each wrapper in `tools/ci/` exports a lane identity and execs one shared
delegate. Those exports were read by nothing: the delegate rebuilt `LABELS` from
scratch, took its group from host config, and appended the trusted label
unconditionally — so starting the pr-safe pool registered a runner that was
indistinguishable from a trusted one, and would have run unreviewed contributor
code on the pool reserved for merged code.

The reason it went unnoticed is the shape worth pinning: the existing tests
assert the wrappers' reference *strings*, which pass against a delegate that
ignores every variable those strings name. So these drive the delegate and
assert what it would actually REGISTER.

The trusted lane was correct by coincidence — its exports happen to equal the
delegate's hardcoded defaults. `exercise_unset_environment_is_todays_behaviour`
is what keeps that true by contract, because the delegate is re-exec'd
unattended on the live fleet after every job.
"""
from __future__ import annotations

import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
DELEGATE = ROOT / "tools/ci/pulp-ephemeral-runner.sh"
TRUSTED_WRAPPER = ROOT / "tools/ci/proxmox-trusted-ephemeral-runner-linux.sh"
PR_SAFE_WRAPPER = ROOT / "tools/ci/proxmox-pr-safe-ephemeral-runner-linux.sh"

TRUSTED_LABEL = "pulp-auto-linux-x64"
PR_SAFE_LABEL = "pulp-pr-safe-linux-x64"

# Linux-only tools the delegate insists on before it will use an org group.
FAKE_TOOLS = (
    "timeout", "iptables-save", "ip6tables-save", "ipset", "ebtables-save",
)


class Harness:
    """A temp dir holding fake secrets, tools, and a fake group verifier."""

    def __init__(self, stack: tempfile.TemporaryDirectory) -> None:
        self.dir = pathlib.Path(stack.name)
        (self.dir / "pat").write_text("fake-pat\n")
        (self.dir / "org-pat").write_text("fake-org-pat\n")
        self.firewall_dir = self.dir / "firewall"
        self.firewall_dir.mkdir()
        self.bin = self.dir / "bin"
        self.bin.mkdir()
        for tool in FAKE_TOOLS:
            self._script(self.bin / tool, "#!/bin/sh\nexit 0\n")
        self._script(
            self.bin / "pve-firewall",
            "#!/bin/sh\nprintf 'Status: enabled/running\\n'\n",
        )
        self._script(self.bin / "gh", "#!/bin/sh\nexit 0\n")

    @staticmethod
    def _script(path: pathlib.Path, body: str) -> None:
        path.write_text(body)
        path.chmod(0o755)

    def verifier(self, group_name: str) -> pathlib.Path:
        """A stand-in for the real group verifier that names a group."""
        path = self.dir / f"verify-{group_name}.py"
        self._script(
            path,
            "#!/usr/bin/env python3\n"
            f"print({group_name!r})\n",
        )
        return path

    def env(self, **overrides: str) -> dict[str, str]:
        env = dict(os.environ)
        env["PATH"] = f"{self.bin}:{env.get('PATH', '')}"
        env["PULP_LINUX_PAT_FILE"] = str(self.dir / "pat")
        env["PULP_LINUX_ORG_PAT_FILE"] = str(self.dir / "org-pat")
        env["PULP_LINUX_FIREWALL_DIR"] = str(self.firewall_dir)
        env["PULP_LINUX_FIREWALL_STATUS_BIN"] = "pve-firewall"
        env["PULP_LINUX_GH_CLI"] = "gh"
        # Inherited state must never leak into a lane assertion.
        for stale in (
            "PULP_RUNNER_LABELS",
            "PULP_RUNNER_NAME_PREFIX",
            "PULP_LINUX_RUNNER_GROUP_POLICY",
            "PULP_LINUX_RUNNER_GROUP_ID",
            "PULP_LINUX_GROUP_VERIFIER",
        ):
            env.pop(stale, None)
        env.update(overrides)
        return env


def lane(harness: Harness, **overrides: str) -> tuple[int, dict[str, str]]:
    """Resolve a lane identity through the delegate itself."""
    completed = subprocess.run(
        ["bash", str(DELEGATE), "--print-lane"],
        env=harness.env(**overrides),
        capture_output=True,
        text=True,
    )
    fields: dict[str, str] = {}
    for line in completed.stdout.splitlines():
        key, _, value = line.partition("=")
        fields[key] = value
    # die() reports through log(), which prints to stdout — assert on both.
    fields["_output"] = completed.stdout + completed.stderr
    return completed.returncode, fields


def wrapper_exports(path: pathlib.Path) -> dict[str, str]:
    """Read a wrapper's declared identity, so the test tracks the source."""
    exports: dict[str, str] = {}
    for line in path.read_text().splitlines():
        line = line.strip()
        for key in ("LABELS=", "PULP_RUNNER_NAME_PREFIX=", "PULP_LINUX_RUNNER_GROUP_POLICY="):
            marker = f"export {key}" if line.startswith("export ") else key
            if line.startswith(marker):
                exports[key.rstrip("=")] = line.split("=", 1)[1].strip().strip('"')
    return exports


def exercise_pr_safe_never_registers_trusted(harness: Harness) -> int:
    """The whole point: a pr-safe lane must never advertise the trusted label."""
    exports = wrapper_exports(PR_SAFE_WRAPPER)
    code, fields = lane(
        harness,
        PULP_RUNNER_LABELS=exports["LABELS"],
        PULP_RUNNER_NAME_PREFIX=exports["PULP_RUNNER_NAME_PREFIX"],
        PULP_LINUX_RUNNER_GROUP_POLICY=exports["PULP_LINUX_RUNNER_GROUP_POLICY"],
    )
    assert code == 0, fields["_output"]
    labels = fields["labels"].split(",")
    assert PR_SAFE_LABEL in labels, labels
    assert TRUSTED_LABEL not in labels, (
        f"pr-safe lane advertised the TRUSTED label: {fields['labels']}"
    )
    assert fields["name_prefix"] == "pulp-pr-safe-ephemeral", fields["name_prefix"]
    assert fields["policy"] == "pr-safe", fields["policy"]
    return 4


def exercise_unset_environment_is_todays_behaviour(harness: Harness) -> int:
    """The live-fleet regression guard.

    The delegate is re-exec'd unattended after every job, so an empty
    environment must register exactly what it registered before any of this
    existed.
    """
    code, fields = lane(harness)
    assert code == 0, fields["_output"]
    assert fields["labels"] == (
        "self-hosted,Linux,X64,pulp-build-linux-x64,pulp-host-macpro"
    ), fields["labels"]
    assert fields["name_prefix"] == "pulp-ci-ephemeral", fields["name_prefix"]
    assert fields["policy"] == "trusted", fields["policy"]
    return 3


def exercise_trusted_wrapper_matches_the_defaults(harness: Harness) -> int:
    """The trusted lane was correct by coincidence; pin that it stays correct."""
    exports = wrapper_exports(TRUSTED_WRAPPER)
    code, explicit = lane(
        harness,
        PULP_RUNNER_LABELS=exports["LABELS"],
        PULP_RUNNER_NAME_PREFIX=exports["PULP_RUNNER_NAME_PREFIX"],
        PULP_LINUX_RUNNER_GROUP_POLICY=exports["PULP_LINUX_RUNNER_GROUP_POLICY"],
    )
    assert code == 0, explicit["_output"]
    _, implicit = lane(harness)
    assert explicit["labels"] == implicit["labels"], (explicit, implicit)
    assert explicit["name_prefix"] == implicit["name_prefix"], (explicit, implicit)
    return 2


def exercise_org_group_label_rules(harness: Harness) -> int:
    """In an org group, only the trusted lane may carry the trusted label."""
    checks = 0
    code, fields = lane(
        harness,
        PULP_LINUX_RUNNER_GROUP_ID="3",
        PULP_LINUX_GROUP_VERIFIER=str(harness.verifier("pulp-trusted-build")),
    )
    assert code == 0, fields["_output"]
    assert TRUSTED_LABEL in fields["labels"].split(","), fields["labels"]
    assert fields["group"] == "pulp-trusted-build", fields["group"]
    checks += 2

    # The append must not duplicate a label the wrapper already declared.
    exports = wrapper_exports(TRUSTED_WRAPPER)
    code, fields = lane(
        harness,
        PULP_RUNNER_LABELS=exports["LABELS"],
        PULP_LINUX_RUNNER_GROUP_POLICY="trusted",
        PULP_LINUX_RUNNER_GROUP_ID="3",
        PULP_LINUX_GROUP_VERIFIER=str(harness.verifier("pulp-trusted-build")),
    )
    assert code == 0, fields["_output"]
    assert fields["labels"].split(",").count(TRUSTED_LABEL) == 1, fields["labels"]
    checks += 1
    return checks


def exercise_lane_group_mismatch_fails_closed(harness: Harness) -> int:
    """A pr-safe lane landing in a non-pr-safe group must refuse to register."""
    checks = 0
    code, fields = lane(
        harness,
        PULP_RUNNER_LABELS=wrapper_exports(PR_SAFE_WRAPPER)["LABELS"],
        PULP_LINUX_RUNNER_GROUP_POLICY="pr-safe",
        PULP_LINUX_RUNNER_GROUP_ID="3",
        PULP_LINUX_GROUP_VERIFIER=str(harness.verifier("pulp-trusted-build")),
    )
    assert code != 0, f"pr-safe lane accepted a trusted group: {fields}"
    assert "refusing to register" in fields["_output"], fields["_output"]
    checks += 2

    # The matching group is accepted, so the guard is not simply always-refuse.
    code, fields = lane(
        harness,
        PULP_RUNNER_LABELS=wrapper_exports(PR_SAFE_WRAPPER)["LABELS"],
        PULP_LINUX_RUNNER_GROUP_POLICY="pr-safe",
        PULP_LINUX_RUNNER_GROUP_ID="5",
        PULP_LINUX_GROUP_VERIFIER=str(harness.verifier("pulp-pr-safe-build")),
    )
    assert code == 0, fields["_output"]
    assert TRUSTED_LABEL not in fields["labels"].split(","), fields["labels"]
    checks += 2

    code, fields = lane(
        harness,
        PULP_LINUX_RUNNER_GROUP_POLICY="nonsense",
        PULP_LINUX_RUNNER_GROUP_ID="3",
        PULP_LINUX_GROUP_VERIFIER=str(harness.verifier("pulp-trusted-build")),
    )
    assert code != 0, "an unknown lane policy was accepted"
    assert "unknown runner group policy" in fields["_output"], fields["_output"]
    checks += 2
    return checks


def main() -> int:
    if shutil.which("bash") is None:  # pragma: no cover - bash is required
        print("linux-runner-lane-contract: bash unavailable; skipping")
        return 0
    checks = 0
    with tempfile.TemporaryDirectory(prefix="pulp-lane-") as stack_dir:
        harness = Harness(type("S", (), {"name": stack_dir})())
        checks += exercise_pr_safe_never_registers_trusted(harness)
        checks += exercise_unset_environment_is_todays_behaviour(harness)
        checks += exercise_trusted_wrapper_matches_the_defaults(harness)
        checks += exercise_org_group_label_rules(harness)
        checks += exercise_lane_group_mismatch_fails_closed(harness)
    print(f"linux-runner-lane-contract: {checks} lane-identity checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
