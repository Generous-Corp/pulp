# Windows warm-build optimization

## Observed cost

The ARM64EC Windows VM build at `C:\pulp-arm64ec-vsbuild2` exposed repeated
CMake regeneration during an otherwise incremental MSBuild. The build output
repeatedly reported that `generate.stamp` was out of date. A stale generated
SIMD header then caused the first full solution attempt to fail at the
`float_lanes`/`double_lanes` queries; touching the generated header and
rebuilding the affected audio target completed with zero errors in 33 minutes.
The full solution was then restarted after the repair and passed that earlier
failure point.

The expensive behavior is timestamp-driven configuration churn, not a lack of
MSVC workers. Repeating a full `Rebuild` also discards incremental compiler
work and should be reserved for a deliberate clean validation.

## Standard warm path

Use [`tools/ci/windows-warm-build.ps1`](../../tools/ci/windows-warm-build.ps1)
for the reusable Windows project image:

```powershell
.\tools\ci\windows-warm-build.ps1 -Source C:\pulp-source `
  -Build C:\pulp-build\warm -Platform ARM64EC
```

The helper:

- fingerprints `HEAD`, staged/unstaged changes, and untracked files;
- records the selected platform, Release configuration, MSBuild worker count,
  and Visual Studio environment;
- configures only when that state changes;
- enables `CMAKE_SUPPRESS_REGENERATION` only in the matching warm tree; and
- builds incrementally with four workers by default.

The normal CI configure path remains unchanged. A source or toolchain change
must cause a new configure; `-Reconfigure` is the explicit escape hatch when
the image's SDK or generator state changed without an environment-variable
change. Keep source, build, SDK, and cache directories on the VM's fast volume.

## Dependency policy

`mise` is useful for repeatable developer tasks and future image manifests, but
it is not yet the authority for Pulp's Windows compiler, Visual Studio SDK,
Skia/Dawn artifacts, or CI provisioning. Do not make a warm image depend on a
live mise registry. Adopt host-tool pins only with a generated cross-platform
`mise.lock`, a Windows canary, and a recorded image/tool manifest. Until then,
keep the existing explicit Visual Studio and SDK provisioning contract.

## Capacity boundary

This helper is a local VM workflow. It does not register a GitHub runner,
change `runs-on`, or consume the M1/M3/M5 macOS gate pool. Ephemeral runner
automation remains separately opt-in and must continue to use a copy-on-write
overlay that is discarded after each job.
