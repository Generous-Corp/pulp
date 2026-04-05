---
name: test
description: Run the Pulp test suite
---

Run the Pulp test suite. Accepts an optional filter pattern as $ARGUMENTS.

If $ARGUMENTS is provided, use it as a test filter:
```bash
ctest --test-dir build --output-on-failure -R "$ARGUMENTS"
```

Otherwise run all tests:
```bash
ctest --test-dir build --output-on-failure
```

If tests fail, read the output and diagnose the issue. Known pre-existing flaky test: AudioWorkgroup timeout — skip investigating that one (use `--exclude-regex AudioWorkgroup` if it blocks).
