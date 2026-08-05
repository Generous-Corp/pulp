---
name: run
description: Launch the standalone plugin host for quick iteration
---

Launch a standalone plugin binary for testing.

```bash
./build/pulp run [target]
```

If no target is specified, runs the default standalone binary. Use `-- args...` to pass arguments to the launched binary.

Examples:
```bash
pulp run                          # launch default standalone
pulp run PulpGain                 # launch specific plugin standalone
pulp run --headless --screenshot /tmp/out.png  # capture screenshot instead
pulp run --inspect                  # GPU-enabled desktop Development Inspector
pulp run --inspect=develop --inspect-runtime-eval  # high-risk live-realm evaluation
pulp run PulpGain -- --debug-port=9222         # pass args to launched binary
```

`--inspect[=<profile>]` requires a GPU-enabled desktop build. GPU-off and
mobile builds keep standalone inspector activation disabled.

`--inspect-runtime-eval` is a separate high-risk acknowledgement for arbitrary
JavaScript execution in the host process. It requires the `develop` profile, or
a custom profile that also names `runtime.eval` and `session.control`. No
profile or saved preference implies it.

If the binary doesn't exist, build first with `/build`.
