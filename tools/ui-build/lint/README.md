# Clean-output lint

`clean_output_lint.py` is the dependency-free source gate for
`pulp import-design --emit source`. It reports stable JSON findings under the
`pulp-clean-output-v1` schema and exits non-zero for importer output that loses
semantic names, leaves static style objects or colour literals inline, emits
duplicate markup, creates non-semantic click targets, or depends on an
unseeded clock/random source.

When `--manifest` is supplied for a captured output corpus, the gate also
binds the manifest to the complete source tree: every supported source file
must appear exactly once with a canonical sorted relative path and a matching
SHA-256. Missing, extra, duplicate, non-canonical, or symlinked entries fail
closed so a corpus cannot silently drift outside the recorded artifact set.

```sh
python3 tools/ui-build/lint/clean_output_lint.py native-ui/src --json
pulp ui lint --source native-ui/src --json
```

`pulp ui lint` is the user-facing dispatcher for the same linter. A captured
corpus can pass `--manifest`; every listed entry is hash-checked, while only
entries with the `owned-source` or `emitted-source` role are linted. Generated
or vendor entries such as `generated-vendor` remain covered by the manifest
integrity check but are excluded from the semantic source report. Non-source
conformance inputs may use the `conformance-fixture` role; they are also
hash-checked without being treated as TSX source. Supported source files must
still appear in the manifest as a complete set.

When a corpus claims a source fixture with `source_kind`, its manifest must
also name `source_fixture_sha256` and `source_fixture_output`. The linter
checks the repository fixture bytes and requires the copied output entry to
carry the same digest, so a checked-in conformance copy cannot drift from the
input it claims to represent.

Duplicate markup is reported for repeated complete, balanced JSX subtrees even
when the copies span lines. The scanner stays conservative for malformed TSX;
the TypeScript compiler remains responsible for syntax validation.

Component/function size is reported only when `--enforce-size` is supplied;
the program plan keeps those thresholds advisory until DP-2 has measured the
Spectr corpus. Each finding has a planted-control test in
`test_clean_output_lint.py`.
