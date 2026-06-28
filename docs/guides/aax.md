# AAX Setup

AAX is an optional plugin format for Pro Tools. Pulp supports AAX Native targets on macOS and Windows when you provide the Avid AAX SDK locally. The SDK is not bundled — your local AAX toolchain stays outside the repo.

## Platform Scope

- Supported: macOS and Windows
- Unsupported: Linux and Ubuntu
- Current target: AAX Native only
- Out of scope: DSP, AudioSuite, redistributing any Avid or PACE assets

If you request `AAX` in `FORMATS` on Linux or Ubuntu, CMake now fails with a
direct error instead of silently ignoring it.

## What To Download

Sign in at <https://developer.avid.com/aax/> and download:

- `AAX SDK`
- `DigiShell and AAX Validator` for your platform

Optional:

- `AAX Plug-In Page Table Editor` if you need page-table/control-surface mapping later

Not needed for normal Pulp integration and should stay out of this repo:

- `AAX Developer Tools` beta bundles
- Pro Tools installers
- HD Driver
- Avid Cloud Client Services

Those extra packages may be useful for deeper host-specific workflows later, but
they are not required for the initial AAX integration, build, or
validator loop.

## Suggested Install Locations

Use a stable user-local path so the CLI can auto-discover the tools:

macOS / Linux-style home paths:

```text
~/SDKs/avid/aax-sdk/current
~/SDKs/avid/aax-validator/current
```

Windows:

```text
%USERPROFILE%\SDKs\avid\aax-sdk\current
%USERPROFILE%\SDKs\avid\aax-validator\current
```

Environment variables override auto-discovery:

```bash
export PULP_AAX_SDK_DIR=~/SDKs/avid/aax-sdk/current
export PULP_AAX_VALIDATOR_DIR=~/SDKs/avid/aax-validator/current
```

On Windows:

```powershell
$env:PULP_AAX_SDK_DIR="$env:USERPROFILE\SDKs\avid\aax-sdk\current"
$env:PULP_AAX_VALIDATOR_DIR="$env:USERPROFILE\SDKs\avid\aax-validator\current"
```

`PULP_AAX_SDK_DIR` should point at the unpacked SDK root containing
`Interfaces/AAX.h`.

`PULP_AAX_VALIDATOR_DIR` should point at the extracted validator root containing
`CommandLineTools/`.

## Build Workflow

Keep AAX off by default. Enable it only when you actually want to build `.aaxplugin`
targets:

```bash
cmake -S . -B build \
  -DPULP_ENABLE_AAX=ON \
  -DPULP_AAX_SDK_DIR=$HOME/SDKs/avid/aax-sdk/current
cmake --build build --target MyPlugin_AAX
```

Projects that want AAX must include `AAX` in `FORMATS` and provide:

- `MANUFACTURER_CODE`
- `AAX_PRODUCT_CODE`
- `AAX_NATIVE_CODE`
- `aax_entry.cpp`

If the SDK is missing, `pulp status` and `pulp doctor` point you back to the
Avid download page instead of guessing. `pulp create` follows the same optional
boundary: default macOS/Windows plugin scaffolds include `AAX` and
`aax_entry.cpp` only when an AAX SDK is configured via `PULP_AAX_SDK_DIR` or
auto-discovered in a standard user-local SDK path; otherwise the generated
project omits AAX and can opt in later.

## Validation Workflow

When DigiShell + AAX Validator are installed:

```bash
pulp validate
pulp validate --all
```

Behavior:

- `pulp validate` runs a faster describe-validation probe for each built `.aaxplugin`
- `pulp validate --all` runs the broader AAX validator suite
- Run one full AAX validator suite at a time; DigiShell may reuse a local port and collide with parallel runs
- If the validator is missing, the CLI reports `SKIPPED` and prints the Avid
  download guidance

On macOS, you may need to remove quarantine attributes from the extracted
validator tools if the shell refuses to execute them.

Some Avid tooling may depend on your local Avid/PACE/iLok setup. Start with the
SDK plus DigiShell/AAX Validator first, and add more tooling only when your
workflow actually needs it.

## Rules

- Never commit the AAX SDK, DigiShell, validator binaries, Avid examples, or generated vendor files into this repo
- Never unpack the AAX SDK inside the Pulp source tree
- Never copy implementation text from Avid sample code into Pulp
- Keep all vendor assets developer-supplied and out-of-tree
- Treat local AAX tools as optional extensions, not bundled dependencies

The repo audit tooling is designed to help maintainers catch this:

- `python3 tools/deps/audit.py --strict`

## CLI Behavior

Pulp now handles AAX as a first-class optional format:

- `pulp status` reports detected AAX SDKs on macOS/Windows and explicit unsupported status on Linux/Ubuntu
- `pulp doctor` reports optional AAX SDK and validator discovery on macOS/Windows
- `pulp create` scaffolds `aax_entry.cpp` on macOS/Windows only when the
  optional AAX SDK is configured or auto-discovered; otherwise the default
  project omits AAX and builds the other supported formats
- `pulp validate` uses the validator when present and explains what to install when it is not

That keeps the day-to-day developer UX clear without bundling any Avid code.
