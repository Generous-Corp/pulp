#!/usr/bin/env python3
"""Turn a failed release job into an incident entry an agent can act on.

The release reconciler (`release_reconcile.py`) owns the single release
incident issue. A line such as "failing leg: CLI windows-arm64 › Fetch Skia
binaries" says WHERE a release broke but not WHY or what to change, so every
reader has to open the run, scroll the log, and rediscover the fix. This module
reads the failing job's log once and produces:

  - the first meaningful error line (not the generic "exit code 1"),
  - a cause class, and
  - for the classes it recognises, the exact fix and how to verify it.

Everything here is pure (log text and file text in, values out) so the cases
are unit-testable against recorded logs without the GitHub API. The one piece of
live evidence — the digest the upstream release asset carries NOW — is fetched
by the caller and passed in.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field

# Lines the runner prints around every failure. They say a step failed, which
# the step conclusion already told us, and never why.
_GENERIC_ERRORS = (
    re.compile(r"^##\[error\]Process completed with exit code \d+\.?$"),
    re.compile(r"^##\[error\]The operation was canceled\.?$"),
)
_TIMESTAMP = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?Z\s?")
_ERRORISH = re.compile(
    r"(^##\[error\])|(^ERROR\b)|(^error\b)|(\berror(?: C\d{4})?:)|(^FAIL(?:ED)?\b)|(Traceback \(most recent call last\))",
    re.IGNORECASE,
)
_ERROR_LINE_LIMIT = 300

# fetch_skia_for_release.py prints these when a pinned archive's bytes do not
# hash to the pin. The url line is printed on stdout and can land after the
# stderr ERROR block in the merged log, so each field is matched independently.
_DIGEST_MISMATCH = re.compile(r"ERROR: sha256 mismatch")
_EXPECTED = re.compile(r"^\s*expected:\s*([0-9a-f]{64})\s*$", re.MULTILINE)
_ACTUAL = re.compile(r"^\s*actual:\s*([0-9a-f]{64})\s*$", re.MULTILINE)
_URL = re.compile(r"^\s*url:\s*(https://github\.com/\S+)\s*$", re.MULTILINE)
_RELEASE_ASSET_URL = re.compile(
    r"^https://github\.com/([^/]+/[^/]+)/releases/download/(.+)/([^/]+)$"
)
_DISPATCH_GUARD = "Dispatch must run on the tag it publishes"

DIGEST_MISMATCH = "pinned-asset digest mismatch"
PINNED_REVISION = "pinned-revision mismatch"
COMPILE_ERROR = "compile error"
DISPATCH_REF = "re-dispatch from the wrong ref"
UNCLASSIFIED = "unclassified"

PIN_FILE = "tools/deps/manifest.json"

# Archives whose msvc-toolchain.json is known broken but which were accepted
# deliberately, keyed by (repo, release tag, asset, sha256). The provenance
# rule ("do not repin to an archive with unknown/error provenance") still holds
# for every other archive; an entry here only stops the incident from telling
# an agent to undo an accepted pin. Each entry says why it was accepted.
PROVENANCE_EXCEPTIONS: dict[tuple[str, str, str, str], str] = {
    (
        "danielraffel/skia-builder",
        "chrome/m153",
        "skia-build-win-arm64-gpu-release.zip",
        "18e57481e84992fecccbc4ac0f8f283c4a68d90d7978a61ce2e68f21dd34a9c7",
    ): (
        "Documented exception: this is the chrome/m153 win-arm64 archive pinned "
        "by Pulp PR 10170 (sha256 "
        "18e57481e84992fecccbc4ac0f8f283c4a68d90d7978a61ce2e68f21dd34a9c7). Its "
        "msvc-toolchain.json is known broken (toolset and SDK `unknown`, "
        "clang-cl version `error: no input files`); its contents were verified "
        "by archive comparison instead; and it is Skia 37c5ad6a, not the release "
        "lock's 8b8c3872. Keep this pin — do not revert PR 10170 over the "
        "provenance file. The provenance rule still applies to any other archive."
    ),
}


def strip_timestamp(line: str) -> str:
    return _TIMESTAMP.sub("", line.rstrip("\r\n"))


def first_error_line(log_text: str) -> str:
    """Return the first line that says what went wrong, or "".

    Generic runner lines ("Process completed with exit code 1") are skipped
    unless nothing more specific exists, because they are always present and
    never actionable.
    """
    fallback = ""
    for raw in log_text.splitlines():
        line = strip_timestamp(raw).strip()
        if not line or not _ERRORISH.search(line):
            continue
        # `echo "::error::..."` in a shell trace is a command, not an error.
        if line.startswith(("echo ", "+ echo")):
            continue
        if any(p.match(line) for p in _GENERIC_ERRORS):
            fallback = fallback or line
            continue
        return line[:_ERROR_LINE_LIMIT]
    return fallback[:_ERROR_LINE_LIMIT]


@dataclass(frozen=True)
class AssetPin:
    """Where a pinned release asset lives in the pin file, and what it pins."""

    key: str
    url: str
    line: int          # 1-based line of the `"sha256":` entry
    pinned: str        # the digest the pin file currently holds


def find_pin(manifest_text: str, url: str) -> AssetPin | None:
    """Locate the `release_assets` entry whose url is `url`.

    Line numbers come from the text, not a JSON round-trip, because the point
    is to tell a reader which line to edit.
    """
    lines = manifest_text.splitlines()
    url_needle = f'"url": "{url}"'
    for index, line in enumerate(lines):
        if url_needle not in line:
            continue
        key = ""
        for back in range(index - 1, max(index - 4, -1), -1):
            m = re.match(r'\s*"([^"]+)":\s*\{\s*$', lines[back])
            if m:
                key = m.group(1)
                break
        for fwd in range(index + 1, min(index + 4, len(lines))):
            m = re.search(r'"sha256":\s*"([0-9a-f]{64})"', lines[fwd])
            if m:
                return AssetPin(key=key, url=url, line=fwd + 1, pinned=m.group(1))
    return None


def release_asset_coordinates(url: str) -> tuple[str, str, str] | None:
    """Split a release download url into (owner/repo, tag, asset name)."""
    m = _RELEASE_ASSET_URL.match(url)
    return (m.group(1), m.group(2), m.group(3)) if m else None


@dataclass(frozen=True)
class Diagnosis:
    """One failed release job, explained."""

    workflow: str
    tag: str
    run_url: str
    job: str
    step: str
    first_error: str
    cause: str
    fix: tuple[str, ...] = ()
    verify: tuple[str, ...] = ()
    details: dict = field(default_factory=dict)

    @property
    def tag_immutable(self) -> bool:
        """Can no re-run of this tag succeed?

        A digest mismatch is tag-immutable only when the upstream asset itself
        now carries the digest the run observed: then the pin frozen in the
        tag's tree names bytes that no longer exist, and every re-run fails the
        same way. Without that confirmation a mismatch could be a truncated
        download, which a re-run can fix.
        """
        return self.cause == DIGEST_MISMATCH and bool(
            self.details.get("upstream_confirms")
        )

    def render(self) -> list[str]:
        """Markdown lines, indented to sit under an incident bullet."""
        out = [
            f"  - **Workflow:** {self.workflow} — [run]({self.run_url})",
            f"  - **Failing job › step:** `{self.job}` › `{self.step}`",
            f"  - **First error:** `{self.first_error or '(no error line in the job log)'}`",
            f"  - **Cause class:** {self.cause}",
        ]
        if self.fix:
            out.append("  - **Fix:**")
            out.extend(f"    {i}. {step}" for i, step in enumerate(self.fix, 1))
        if self.verify:
            out.append("  - **Verify:**")
            out.extend(f"    - {step}" for step in self.verify)
        return out


def diagnose(
    *,
    workflow: str,
    tag: str,
    run_url: str,
    job: str,
    step: str,
    log_text: str,
    manifest_text: str = "",
    upstream_digest: str | None = None,
) -> Diagnosis:
    """Explain one failed job. `upstream_digest` is the asset's live digest."""
    first = first_error_line(log_text)
    base = dict(workflow=workflow, tag=tag, run_url=run_url, job=job, step=step,
                first_error=first)

    if _DIGEST_MISMATCH.search(log_text):
        return _digest_mismatch(base, log_text, manifest_text, upstream_digest)

    if step == _DISPATCH_GUARD or "Refusing to publish" in log_text:
        return Diagnosis(
            **base,
            cause=DISPATCH_REF,
            fix=(
                f"Re-dispatch with the tag as the ref: `gh workflow run "
                f"{_workflow_file(workflow)} --ref {tag} -f version={tag}`. "
                "A dispatch from `main` is refused because its provenance would "
                "not name the tag commit.",
                "If the tag's own workflow is broken, cut a new tag from the "
                "fixed `main` instead.",
            ),
            verify=(f"The re-dispatched run passes `{_DISPATCH_GUARD}`.",),
        )

    if "does not match pinned revision" in log_text:
        return Diagnosis(
            **base,
            cause=PINNED_REVISION,
            fix=(
                "A pinned runtime revision no longer matches what the tag's tree "
                "records. Update the pin on `main` (see the first error for the "
                "dependency) and cut a new patch tag — the tag is immutable.",
            ),
            verify=("The next SDK tag's `Release CLI` run passes this step.",),
        )

    if re.search(r"error C[2-9]\d{3}:|:\d+:\d+: error:", log_text):
        return Diagnosis(
            **base,
            cause=COMPILE_ERROR,
            fix=(
                "Fix the source named in the first error on `main` and cut a new "
                "patch tag; the tag's sources are immutable, so re-running it "
                "cannot help.",
            ),
            verify=("The next SDK tag's `Release CLI` run passes this job.",),
        )

    return Diagnosis(
        **base,
        cause=UNCLASSIFIED,
        fix=(
            "Open the run, read the failing step around the first error, and "
            "decide whether it is transient (re-run the failed jobs) or a "
            "property of the tag (fix on `main`, cut a new tag).",
        ),
        verify=(f"A later `{workflow}` run for {tag} or a newer tag succeeds.",),
    )


def _workflow_file(workflow: str) -> str:
    return {"Release CLI": "release-cli.yml",
            "Sign and Release": "sign-and-release.yml"}.get(workflow, workflow)


def _digest_mismatch(
    base: dict, log_text: str, manifest_text: str, upstream_digest: str | None
) -> Diagnosis:
    log_text = "\n".join(strip_timestamp(line) for line in log_text.splitlines())
    expected = _EXPECTED.search(log_text)
    actual = _ACTUAL.search(log_text)
    url_m = _URL.search(log_text)
    old = expected.group(1) if expected else "(not in log)"
    new = actual.group(1) if actual else "(not in log)"
    url = url_m.group(1) if url_m else ""
    pin = find_pin(manifest_text, url) if url and manifest_text else None
    coords = release_asset_coordinates(url) if url else None
    upstream_confirms = bool(upstream_digest and actual and upstream_digest == new)

    details = {"pinned": old, "observed": new, "url": url,
               "upstream_digest": upstream_digest or "",
               "upstream_confirms": upstream_confirms}
    fix: list[str] = []
    if pin:
        details.update(pin_file=PIN_FILE, pin_line=pin.line, pin_key=pin.key)
        location = f"`{PIN_FILE}` line {pin.line} (release_assets `{pin.key}`)"
        if pin.pinned == new:
            fix.append(
                f"`main` already pins the new digest at {location}; the failed "
                f"tag froze the old pin. Cut a new patch tag from `main`."
            )
        else:
            fix.append(
                f"Pin to change: {location}: `{pin.pinned}` → `{new}` (the "
                f"tag pinned `{old}`)."
            )
    else:
        fix.append(
            f"Pin to change: the `sha256` next to `{url or 'the asset url'}` in "
            f"`{PIN_FILE}`: `{old}` → `{new}`."
        )

    if coords:
        repo, rel_tag, asset = coords
        api = f"repos/{repo}/releases/tags/{rel_tag}"
        status = (
            f"confirmed: the upstream asset now carries `sha256:{upstream_digest}`"
            if upstream_confirms
            else (f"NOT confirmed: the upstream asset carries `sha256:{upstream_digest}`, "
                  "not the observed digest — suspect a corrupt or truncated download "
                  "and re-run before repinning")
            if upstream_digest
            else "upstream digest not checked"
        )
        fix.insert(0, (
            f"Provenance check BEFORE repinning ({status}): "
            f"`gh api {api} --jq '.assets[]|select(.name==\"{asset}\")|{{digest,updated_at}}'` "
            f"must print `sha256:{new}`; read the release notes "
            f"(`gh api {api} --jq .body`) for an asset-replacement entry naming the "
            f"old and new digest and why the bytes changed."
        ))
        exception = PROVENANCE_EXCEPTIONS.get((repo, rel_tag, asset, new))
        if exception:
            details["provenance_exception"] = True
            fix.insert(1, exception)
        elif "-win-" in asset:
            fix.insert(1, (
                f"Windows archive: `gh release download {rel_tag} --repo {repo} "
                f"--pattern {asset}` then `unzip -p {asset} 'build/*/msvc-toolchain.json'` "
                "— the toolset, SDK and compiler versions must be real values, with "
                "no `unknown` and no `error`. Do not repin to an archive whose "
                "provenance is broken."
            ))
    fix.append(
        "Land the repin on `main` and cut a new patch tag: the failed tag's pin "
        "is frozen in its tree, so re-dispatching it cannot succeed."
    )
    return Diagnosis(
        **base,
        cause=DIGEST_MISMATCH,
        fix=tuple(fix),
        verify=(
            f"The next SDK tag's `Release CLI` run passes `{base['job']} › {base['step']}` "
            "and publishes; this incident then closes once that tag supersedes the failed one.",
        ),
        details=details,
    )
