#!/usr/bin/env python3
"""Controls for the runner-selector audit.

Every case here is a shape that a previous guard got wrong in production. The
point of the file is not coverage for its own sake — it is that a guard against
"a tool is blind to the thing it appears to check" is worth nothing unless the
guard itself is run against both a known-bad and a known-good control. Each
test names the instance it descends from.
"""
from __future__ import annotations

import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import workflow_runner_selector_audit as audit

WORKFLOWS = pathlib.Path(__file__).resolve().parents[2] / ".github" / "workflows"


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def test_flags_the_live_unwrapped_selector() -> int:
    """The shape that actually hung a required gate on 2026-08-16.

    No `fromJSON` anywhere in the selector. This is the easy case and every
    prior guard caught it; it is here so a regression that breaks the easy case
    is not mistaken for the hard ones passing.
    """
    text = """
jobs:
  gate:
    runs-on: >-
      ${{ (vars.PULP_AUTO_LINUX_RUNS_ON_JSON || 'ubuntu-latest') || 'ubuntu-latest' }}
    steps:
      - run: true
"""
    offenders = audit.audit_text("live.yml", text)
    check(len(offenders) == 1, f"expected exactly one offender, got {offenders}")
    check(
        offenders[0].variable == "PULP_AUTO_LINUX_RUNS_ON_JSON",
        f"wrong variable reported: {offenders[0]}",
    )
    return 1


def test_flags_the_second_variable_when_only_the_first_is_wrapped() -> int:
    """Instance 4 — the false NEGATIVE that shipped as the durable guard.

    `if "fromJSON" in block: continue` tests presence, not coverage. On this
    shape it sees `fromJSON`, skips the whole block, and never looks at the
    bare second variable — which is the one that hangs the gate. This is the
    exact two-variable shape the required workflows use, so the weak guard was
    blind to its own target class.
    """
    text = """
jobs:
  gate:
    runs-on: >-
      ${{ fromJSON(vars.PULP_LOCAL_LINUX_RUNS_ON_JSON || '"ubuntu-latest"') ||
          (vars.PULP_AUTO_LINUX_RUNS_ON_JSON || 'ubuntu-latest') }}
    steps:
      - run: true
"""
    offenders = audit.audit_text("two-vars.yml", text)
    check(
        [offender.variable for offender in offenders]
        == ["PULP_AUTO_LINUX_RUNS_ON_JSON"],
        f"the unwrapped second variable must be the only offender, got {offenders}",
    )

    # The control that proves the mechanism, not the outcome: the substring
    # test the old guard used passes this same input, so a green here is only
    # meaningful because the old approach is demonstrably red.
    check("fromJSON" in text, "control invalid: this block must contain fromJSON")
    return 1


def test_does_not_flag_nested_parentheses() -> int:
    """Instance 3 — the false POSITIVE on a correct file.

    `nightly-intel.yml` wraps three alternatives in one `fromJSON(...)`, so a
    flat regex looking for `fromJSON(vars.X)` cannot span it and reports a
    correct selector as broken. Paren matching handles arbitrary nesting.
    """
    text = """
jobs:
  native:
    runs-on: ${{ fromJSON((github.event_name == 'workflow_dispatch' && inputs.use_physical_intel && '["self-hosted","macOS","X64"]') || (github.event_name != 'workflow_dispatch' && vars.PULP_NATIVE_INTEL_RUNS_ON_JSON) || '"macos-15-intel"') }}
    steps:
      - run: true
"""
    offenders = audit.audit_text("nightly-intel.yml", text)
    check(not offenders, f"nested-parenthesis selector must be accepted, got {offenders}")
    return 1


def test_quoted_parenthesis_does_not_desynchronise_the_span() -> int:
    """A parenthesis inside a string literal is a character, not grouping.

    The trusted-workflow guards call `format('{0}/.github/workflows/x.yml@...')`
    inside their selectors. A depth counter that is not quote-aware can end a
    span early and then report every following variable as unwrapped.
    """
    text = """
jobs:
  gate:
    runs-on: >-
      ${{ fromJSON(
        github.workflow_ref == format('{0}/.github/workflows/v.yml@refs/heads/main :-(', github.repository) &&
        (vars.PULP_LOCAL_LINUX_RUNS_ON_JSON || '"ubuntu-latest"') ||
        '"ubuntu-latest"'
      ) }}
    steps:
      - run: true
"""
    offenders = audit.audit_text("quoted.yml", text)
    check(not offenders, f"quoted parenthesis must not break span matching, got {offenders}")
    return 1


def test_ignores_env_and_with_contexts() -> int:
    """These variables are legitimately unparsed outside `runs-on`.

    Several workflows hand the raw JSON string to a script through `env:`, which
    is correct and must not be reported. Scoping is what makes the check
    actionable rather than noisy.
    """
    text = """
jobs:
  gate:
    runs-on: ubuntu-latest
    steps:
      - run: ./resolve.sh
        env:
          CONFIGURED_JSON: ${{ vars.PULP_COVERAGE_LINUX_RUNS_ON_JSON }}
          TRUSTED_JSON: ${{ vars.PULP_AUTO_LINUX_RUNS_ON_JSON || '' }}
"""
    offenders = audit.audit_text("env.yml", text)
    check(not offenders, f"env-context variables must be ignored, got {offenders}")
    return 1


def test_multiline_block_stops_at_the_next_key() -> int:
    """Block continuation is indentation-based, so a later key is not absorbed.

    If the block scanner over-ran into sibling keys, an unwrapped variable in a
    following `env:` would be reported as a `runs-on` offence and the check
    would cry wolf until someone stopped believing it.
    """
    text = """
jobs:
  gate:
    runs-on: >-
      ${{ fromJSON(vars.PULP_LOCAL_LINUX_RUNS_ON_JSON || '"ubuntu-latest"') }}
    env:
      RAW: ${{ vars.PULP_AUTO_LINUX_RUNS_ON_JSON }}
    steps:
      - run: true
"""
    offenders = audit.audit_text("multiline.yml", text)
    check(not offenders, f"block must stop before env:, got {offenders}")
    return 1


def test_repository_workflows_are_clean() -> int:
    """The live tree must pass, or the guard cannot be enabled in CI."""
    offenders = audit.audit_directory(WORKFLOWS)
    check(
        not offenders,
        "repository workflows have unparsed runner selectors:\n  "
        + "\n  ".join(offender.describe() for offender in offenders),
    )
    return 1


def main() -> int:
    checks = 0
    for name, test in sorted(globals().items()):
        if name.startswith("test_") and callable(test):
            checks += test()
    print(f"runner-selector-audit: {checks} selector controls passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
