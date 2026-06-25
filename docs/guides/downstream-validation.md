# Downstream Validation

Pulp has two downstream validation layers:

- The in-repo installed-SDK smoke in `.github/workflows/install-consumer-smoke.yml`.
  It builds and installs Pulp, then configures a minimal `find_package(Pulp)`
  consumer. This catches SDK packaging and CMake export regressions.
- The P0.4 downstream manifest in
  `tools/validation/downstream/consumer-validation.json`. It records the external
  repos, reviewed SHAs, dependency surfaces, and commands that must be run before
  API, schema, ABI, or installed-SDK refactors are treated as ready.

The manifest is a checklist harness, not a mandatory per-PR build of every
external repo. Run the relevant entries when a change touches the listed
dependency surface.

## Canonical SDK Recipe

Use one installed SDK prefix per Pulp commit under test:

```bash
export PULP_SOURCE_DIR=/path/to/pulp
export PULP_SHA=$(git -C "$PULP_SOURCE_DIR" rev-parse --short HEAD)
export PULP_BUILD_DIR="$PULP_SOURCE_DIR/../pulp-build-sdk-$PULP_SHA"
export PULP_SDK_PREFIX="$PULP_SOURCE_DIR/../pulp-sdk-$PULP_SHA"

cmake -S "$PULP_SOURCE_DIR" -B "$PULP_BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PULP_SDK_PREFIX"
cmake --build "$PULP_BUILD_DIR" -j
cmake --install "$PULP_BUILD_DIR" --prefix "$PULP_SDK_PREFIX"
```

Every downstream configure should point at the installed package:

```bash
cmake -S "$CHECKOUT" -B "$CHECKOUT/build-pulp-sdk" \
  -DCMAKE_BUILD_TYPE=Release \
  -DPulp_DIR="$PULP_SDK_PREFIX/lib/cmake/Pulp"
```

Use a clean downstream build directory for each Pulp commit. Do not reuse object
files compiled against a different SDK prefix.

## Manifest Check

Validate the checklist itself:

```bash
python3 tools/scripts/verify_downstream_validation_manifest.py
```

On a developer machine, add local checkout reporting:

```bash
python3 tools/scripts/verify_downstream_validation_manifest.py --check-local
```

Use `--require-clean` when you are about to run the external commands and want
missing, non-git, or dirty checkouts to fail before the build starts:

```bash
python3 tools/scripts/verify_downstream_validation_manifest.py \
  --check-local --require-clean
```

Without `--require-clean`, missing or dirty external checkouts are advisory. The
manifest schema still validates so CI can guard the checklist without needing
private or optional sibling repos on disk.

## Consumer Boundaries

`pulp-view-embed`, `pulp-embed-iplug2`, and `pulp-embed-juce` validate the flat
embed ABI, host lifecycle, parameter bridge behavior, and installed SDK
consumption.

ProjectIR is separate from Pulp DesignIR: DesignIR governance can inform
ProjectIR governance, but tests for one schema do not prove the other. The
ProjectIR project-importer consumers live in private sibling repos and are
intentionally not tracked in this public manifest; the `project_importer`
category and ProjectIR/DesignIR boundary checks remain in the validators so a
future public importer is governed correctly.
