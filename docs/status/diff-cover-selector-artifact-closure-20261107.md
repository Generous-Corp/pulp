# Local diff coverage selector artifact closure

Status: implemented on the repair branch; awaiting review.

The targeted selector now includes every CMake artifact named by a selected
CTest command, including separate `--forbidden` controls, static archives,
SDK probes, and fixture binaries. `_NOT_BUILT` registrations are selected
only when their planned target has an artifact in the active codemodel.

Focused verification:

```text
python3 tools/scripts/test_diff_cover_targets.py
23 tests passed
```

The repair is intentionally independent of the graph implementation under
review. The graph PR should rerun its diff coverage gate after this selector
repair is available on `main`.
