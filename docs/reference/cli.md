# CLI Reference

The `pulp` CLI wraps common build, test, validation, and shipping operations.

Source: `tools/cli/pulp_cli.cpp`

## Commands

### build

**Status**: usable

Configure and build the project. Auto-detects when CMake reconfiguration is needed.

```bash
pulp build                    # Build all targets
pulp build --target PulpGain_VST3  # Build specific target
pulp build -j8                # Parallel jobs
```

Extra arguments are passed through to `cmake --build`.

### test

**Status**: usable

Run the test suite via CTest. Builds first if no build directory exists.

```bash
pulp test                     # Run all tests
pulp test -R Gain             # Run tests matching "Gain"
```

Extra arguments are passed through to `ctest`.

### status

**Status**: usable

Show project information: root directory, git branch, build state, source file counts, example count, and available plugin format SDKs.

```bash
pulp status
```

### validate

**Status**: usable

Run plugin format validators on all built plugins.

```bash
pulp validate
```

Checks:
- **CLAP**: uses `clap-validator` if installed, otherwise falls back to CTest dlopen checks
- **AU**: uses `auval` on macOS

Prints a summary with pass/fail/skip counts.

### create

**Status**: usable

Create a new plugin project from templates. Checks environment, scaffolds source files, builds, and runs tests.

```bash
pulp create "My Gain"                              # effect plugin (default)
pulp create "My Synth" --type instrument           # instrument plugin
pulp create "My Gain" --template gain              # gain template with UI script
pulp create "My FX" --manufacturer "Acme Audio"    # custom manufacturer
pulp create "My FX" --output ~/projects/my-fx      # custom output directory
pulp create "My FX" --no-build                     # scaffold only, skip build
```

The `--template` flag selects a named template directory (`tools/templates/<name>/`). Templates can include a `ui/` directory with JS scripts that are scaffolded alongside the C++ source. Available templates: `effect` (default), `instrument`, `gain`.

What it does:
1. Runs `pulp doctor` checks (fails fast if environment is broken)
2. Scaffolds source files from templates (processor, format entries, test, CMakeLists.txt, optional UI scripts)
3. Adds the project to `examples/CMakeLists.txt`
4. Configures, builds the test target, and runs tests
5. Reports plugin artifact locations

Default formats are platform-gated:
- **macOS**: VST3, AU, CLAP, Standalone
- **Linux**: VST3, CLAP, LV2, Standalone
- **Windows**: VST3, CLAP, Standalone

### doctor

**Status**: usable

Diagnose environment issues. Checks C++20 compiler, CMake version, git-lfs, LFS file state, external SDKs (VST3, AudioUnit), and platform-specific dependencies.

```bash
pulp doctor             # show all checks
pulp doctor --fix       # auto-fix issues where possible
pulp doctor --ci        # non-interactive, exit codes only
pulp doctor --dry-run   # show what --fix would do
```

Checks are platform-gated — only relevant checks run on each OS:
- **macOS**: compiler, CMake, git-lfs, LFS files, VST3 SDK, AudioUnitSDK, build state
- **Linux**: compiler, CMake, git-lfs, LFS files, VST3 SDK, ALSA dev headers, build state
- **Windows**: compiler, CMake, git-lfs, LFS files, VST3 SDK, build state

Exit code is 0 if all checks pass, 1 if any fail.

### ship

**Status**: experimental

Signing and packaging subcommands.

```bash
pulp ship sign --identity "Developer ID Application: ..."
pulp ship sign --identity "..." --entitlements path/to/entitlements.plist
pulp ship package --version 1.0.0
pulp ship check
```

**Subcommands**:

| Subcommand | What it does |
|------------|-------------|
| `sign`     | Code-sign all built plugin bundles (VST3, CLAP, AU) |
| `package`  | Create `.pkg` installers in `artifacts/` |
| `check`    | Check signing status of all built plugins |

`sign` requires `--identity`. The default entitlements file is `ship/templates/entitlements.plist`.

`package` creates per-format `.pkg` files using `pkgbuild`. macOS only.

### docs

**Status**: usable

Browse local documentation and status manifests. All subcommands read from local files in `docs/` only -- no web calls.

```bash
pulp docs                         # Show help
pulp docs index                   # List available docs
pulp docs search <query>          # Search docs for a string
pulp docs open <slug>             # Print a doc by slug
pulp docs show support <thing>    # Look up support status
pulp docs show command <name>     # Look up a CLI command
pulp docs show cmake <name>       # Look up a CMake function
pulp docs show style              # Show code style rules
pulp docs check                   # Validate docs consistency
```

**Subcommands**:

| Subcommand | What it does |
|------------|-------------|
| `index` | Print a readable list of available docs from `docs-index.yaml` |
| `search <query>` | Case-insensitive search across all Markdown files in `docs/` |
| `open <slug>` | Resolve slug via `docs-index.yaml` and display the file |
| `show support <thing>` | Look up platform/format/subsystem support from `support-matrix.yaml` |
| `show command <name>` | Look up a CLI command from `cli-commands.yaml` |
| `show cmake <name>` | Look up a CMake function from `cmake-functions.yaml` |
| `show style` | Display style rules from `style-rules.yaml` with links to policy docs |
| `check` | Validate docs consistency: manifest links, index completeness, status vocabulary, module dependencies vs CMake |

### clean

**Status**: usable

Remove the build directory.

```bash
pulp clean
```

### help

Print usage information.

```bash
pulp help
```

## Caveats

- The CLI finds the project root by walking up from the current directory looking for a directory with both `CMakeLists.txt` and `core/`.
- The `ship` subcommands are macOS-specific (they use `codesign` and `pkgbuild`).
- There is no `pulp create` scaffolding command yet.
