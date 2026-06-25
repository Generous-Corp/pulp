---
name: test
description: Run the Pulp test suite
---

Run the Pulp test suite through the CLI so project-root resolution,
cold-start builds, FetchContent cache preflight, and ctest passthrough match
the documented `pulp test` behavior. If the installed CLI is not on PATH in a
source checkout, use `./build/pulp test` instead.

If $ARGUMENTS is empty:
```bash
pulp test
```

If $ARGUMENTS is a plain test-name pattern, use it as a ctest `-R` filter:
```bash
pulp test -R "$ARGUMENTS"
```

If $ARGUMENTS already contains ctest flags, forward them verbatim:
```bash
pulp test $ARGUMENTS
```

If tests fail, read the output and diagnose the issue. Known pre-existing flaky test: AudioWorkgroup timeout — skip investigating that one (use `--exclude-regex AudioWorkgroup` if it blocks).
