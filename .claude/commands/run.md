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
pulp run -- --screenshot /tmp/out.png  # capture screenshot instead
```

If the binary doesn't exist, build first with `/build`.
