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
```

Component/function size is reported only when `--enforce-size` is supplied;
the program plan keeps those thresholds advisory until DP-2 has measured the
Spectr corpus. Each finding has a planted-control test in
`test_clean_output_lint.py`.
