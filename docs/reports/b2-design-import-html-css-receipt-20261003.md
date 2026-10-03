# Phase 2 B2 design-import HTML/CSS extraction receipt

Date: 2026-10-03
Implementation commit: `b23b5c87ca5899226f77e6fd902694848b76a278`
Branch: `codex/b2-design-import-html-css-20261002`
Base: protected `origin/main` at `b91ad1d7e67b909d3cc8cfae50344c0f941cbb1d`

## File scope

The implementation commit contains exactly these three files:

- `core/view/src/design_import.cpp` — removes the extracted pure helper block (361 lines).
- `core/view/src/design_import_claude_css.cpp` — adds the private HTML/CSS helper translation unit (379 lines).
- `core/view/cmake/PulpViewSources.cmake` — registers the new translation unit.

No roadmap, `DesignIR`, adapter interface, network/cache policy, source contract,
asset identity, ordering, or generated-output files changed.

Moved symbols:

- `css_prop_to_camel_case`
- `strip_css_comments`
- `parse_css_declarations`
- `skip_css_string`
- `find_matching_brace`
- `collect_classnames_from_css`
- `extract_html_style_blocks`
- `extract_bundler_template_html`
- `extract_claude_classnames`
- `looks_like_bundler_entry`
- `serialize_claude_classnames`

The boundary is stable because these functions only scan HTML/CSS text and
produce deterministic `std::map`/JSON results; they do not read or mutate
`DesignIR`, runtime state, asset caches, network policy, or parser provenance.

## Validation

- `pulp build --target pulp-test-group-design-import-tool` — passed in the clean
  origin/main worktree; compiled and linked both `design_import.cpp` and
  `design_import_claude_css.cpp`.
- `./build/test/pulp-test-group-design-import-tool '*claude*' --reporter compact`
  — 17 cases, 14 passed, 3 skipped only because
  `PULP_CLAUDE_BUNDLE_FIXTURE` was unset; 76 assertions passed.
- Focused CTest helper run (`extract_claude_classnames` and
  `looks_like_bundler_entry`) — 16/16 passed.
- Design-import/Claude/intake/staging/runtime/round-trip CTest selection —
  74/74 passed.
- Network/cache/offline CTest selection — 6/6 passed.
- `python3 tools/import-validation/check-source-contracts.py --strict` — no
  findings; no parser/runtime mapping changed.
- `git diff --check` — clean.

The first push's diff-coverage step was skipped by the host governor because
only `-j2` was available and the estimate was hours; the documented
`PULP_DISABLE_PREPUSH_DIFF_COVER=1` bypass was used for the successful push.
CI remains responsible for the full diff-coverage check.
