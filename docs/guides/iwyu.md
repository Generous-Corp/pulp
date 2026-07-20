# IWYU (include-what-you-use)

Pulp runs IWYU as an **advisory** CI check on the Linux Clang lane to
catch transitive-include bugs at analysis time instead of waiting for
the cross-platform matrix to reject them.

This guide covers the Phase 2 advisory gate. See
[issue #594](https://github.com/Generous-Corp/pulp/issues/594) for the
full 3-tier plan (Phase 1 tightened the coverage-build gate, Phase 2
is this advisory IWYU check, Phase 3 will flip it to blocking once the
false-positive rate settles).

## What we're catching

Apple Clang's libc++ tolerates a large number of transitive includes —
if `<vector>` happens to pull in `<memory>`, code that uses `std::unique_ptr`
without `#include <memory>` compiles fine on macOS. Linux Clang
(libstdc++) and MSVC are stricter; the same code fails on those
lanes.

Three real incidents on 2026-04-21 alone:

| Incident      | File                                      | Missing   |
|---------------|-------------------------------------------|-----------|
| [#540](https://github.com/Generous-Corp/pulp/issues/540)   | `core/signal/include/pulp/signal/fft.hpp` | `<memory>`    |
| Slice 4 test  | `test/test_cli_version_diag.cpp`          | `<atomic>`    |
| [#593](https://github.com/Generous-Corp/pulp/issues/593)   | `core/state/include/pulp/state/state_tree.hpp` | `<algorithm>` |

Each bug was only detected by CI after landing on a feature branch.
IWYU rejects them at analysis time.

## How the gate runs

- **Workflow**: `.github/workflows/iwyu.yml`
- **Runner**: `ubuntu-24.04` (Linux Clang 18 via the distro `iwyu` apt package)
- **Advisory until**: **2026-05-05** — `continue-on-error: true`. PRs get
  inline annotations on the diff but merge is not blocked.
- **Scope**: PR events analyze files changed vs `origin/<base>`;
  push-to-main events run a full repo scan and upload the report as
  an artifact so we can track FP trends.
- **Not run on macOS** — libc++ hides the bug class; false negatives
  would dilute the gate.
- **Not run on Windows** — IWYU is Clang-only; MSVC already catches
  transitive-include bugs at compile time in `build.yml`.

## Output

Findings surface two ways:

1. **Inline PR annotations** — each missing-include suggestion appears
   as a `warning` on line 1 of the offending file, visible on the PR
   "Files changed" tab.
2. **Job summary** — a markdown block listing every finding, rendered
   at the bottom of the job log.

Example annotation (from a local dry-run):

```
::warning file=core/signal/include/pulp/signal/fft.hpp,line=1,title=IWYU: missing include (advisory, issue #594)::add `<memory>` — for unique_ptr
```

## Local usage

You can validate a branch before pushing:

```bash
# Install (one-time)
sudo apt-get install iwyu         # Ubuntu/Debian
brew install include-what-you-use  # macOS (advisory — may false-negative on libc++)

# Configure + analyze
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
iwyu_tool.py -p build core/signal/src/fft.cpp \
    -- -Xiwyu --mapping_file=.iwyu-mappings.imp \
    | python3 tools/scripts/iwyu_annotate.py
```

On macOS you'll typically see fewer findings than the CI lane — that
is the nature of libc++. Don't count on local IWYU to stand in for
the Linux gate.

## Mappings file

`.iwyu-mappings.imp` at the repo root maps Pulp-specific includes to
their canonical public header. The main use cases:

- **CHOC amalgamated headers** — CHOC ships each utility as a single
  header; IWYU's private/public model has nowhere else to redirect.
- **libstdc++ detail headers** — IWYU sometimes suggests
  `<bits/shared_ptr.h>` instead of `<memory>`. The mapping rewrites
  those suggestions to the public header.

Prefer fixing the code (adding the missing include) over adding a new
mapping. Every mapping papers over an analysis that would otherwise be
correct.

## False-positive tracking

During the advisory window (through 2026-05-05) we collect the
push-to-main scan artifacts so we can see the repo-wide FP rate. Flip
to blocking requires:

- FP rate < 5% of total findings across a one-week window
- No Pulp subsystem that cannot reasonably satisfy the gate

Known FP sources so far (to extend as they come up):

- **CHOC headers** — amalgamated; redirected in `.iwyu-mappings.imp`.
- **libstdc++ detail paths** — redirected in `.iwyu-mappings.imp`.
- **`<choc/text/choc_JSON.h>` / `choc_Value.h`** — choc JSON types
  pull in iostream-like APIs that IWYU wants to rewrite; ignore.

When flipping to blocking (Phase 3 of #594):

1. Edit `.github/workflows/iwyu.yml`: set `continue-on-error: false`
2. Update this guide's "Advisory until" line to "Blocking since &lt;date&gt;"
3. Reference the flip in the PR description
4. Close #594 only after the blocking gate has held for a week

## Related

- Issue #594 — the full 3-tier plan
- Issue #593 — coverage-build gate hardening (Phase 1)
- `docs/guides/coverage.md` — sibling CI gate (coverage diff advisory)
- `docs/guides/local-ci.md` — end-to-end CI overview
