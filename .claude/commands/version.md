---
name: version
description: Show, bump, or check version consistency
---

Manage Pulp project versions. Use $ARGUMENTS for subcommands.

Show current version:
```bash
./build/tools/cli/pulp version
```

Bump version (if requested):
```bash
./build/tools/cli/pulp version bump $ARGUMENTS
```

Check version consistency:
```bash
./build/tools/cli/pulp version check
```

After bumping, remind the user to rebuild (the CLI constant is derived from CMake) and to tag when ready.
