# Windows MIDI Services SDK provisioning (`windows-midi2-gate`)

This directory provisions the **Windows MIDI Services SDK**
(`Microsoft.Windows.Devices.Midi2`) so Pulp's opt-in WinRT MIDI 2.0 backend
(`core/midi/platform/win/winrt_midi_device.cpp`, gated by
`PULP_HAS_WINRT_MIDI`) can be **compile-verified** on GitHub-hosted
`windows-latest`. It is a CI-only build dependency — never part of the default
Pulp build, and never a runtime requirement.

## Why this is out-of-band

The `Microsoft.Windows.Devices.Midi2` C++/WinRT projection is **not** in the
base Windows SDK. It ships in a NuGet package that is distributed **only via
the GitHub releases of `microsoft/MIDI`** (not nuget.org). The package
contains the type-metadata `.winmd`, not pre-generated C++ headers; the
`cppwinrt.exe` tool must run on the `.winmd` to generate
`winrt/Microsoft.Windows.Devices.Midi2*.h`. The package also ships two
hand-written bootstrap headers under `build/native/include/winmidi/init/`
(the `MidiDesktopAppSdkInitializer`).

## How CI provisions it: the official vcpkg port

Rather than hand-rolling `nuget restore` + `cppwinrt.exe`, the lane uses
Microsoft's own **vcpkg port** `microsoft-windows-devices-midi2`, which:

1. downloads the SDK NuGet from the `microsoft/MIDI` GitHub release
   (`rc-4` → `Microsoft.Windows.Devices.Midi2.1.0.17-rc.4.25.nupkg`, sha512
   verified by the port itself),
2. pulls `cppwinrt` as a host dependency and runs it to generate the
   projection headers from the `.winmd`,
3. installs the generated `winrt/` headers + the `winmidi/init/` bootstrap
   headers, and
4. exports an IMPORTED CMake target `Microsoft::Windows::Devices::Midi2`
   carrying the projection include dirs.

This mirrors `microsoft/MIDI`'s own `samples/cpp-winrt-cmake` sample, which is
the supported C++/CMake consumption path.

`windows-latest` GitHub runners ship vcpkg pre-installed with `VCPKG_ROOT`
set, so no vcpkg bootstrap is needed.

### Pins (`vcpkg.json`)

- `builtin-baseline`: `2bffa0a8d92272dd2858db00226476698badc233` — the
  `microsoft/vcpkg` commit (2026-05-14, PR #51703) that registered the port at
  `1.0.17-rc.4.25`.
- `overrides`: pins `microsoft-windows-devices-midi2` to `1.0.17-rc.4.25`
  (the latest `microsoft/MIDI` **release** asset). The port's own sample
  `packages.config` tracks a newer in-flight build (`1.0.23-rc.5.17`) that is
  not yet a downloadable GitHub release; we pin to the released asset.
- Windows SDK floor: the port hard-requires Windows SDK **>= 10.0.26100.0**.
  `windows-latest` ships 10.0.26100, which satisfies the floor — but only just,
  so this is a watch point (see "Likely failure points").

## Consuming it from Pulp

`core/midi/CMakeLists.txt` (under `if(PULP_HAS_WINRT_MIDI)`) does:

```cmake
find_package(microsoft-windows-devices-midi2 CONFIG QUIET)
# → links Microsoft::Windows::Devices::Midi2 + OneCoreUap when found,
#   else falls back to a manual -DPULP_WINRT_MIDI_INCLUDE=<cppwinrt-output>
#   path (links WindowsApp + runtimeobject), else hard-fails with guidance.
```

The CI job configures Pulp with the vcpkg toolchain and the midi2 install
prefix so `find_package` resolves the IMPORTED target:

```
-DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
-DVCPKG_MANIFEST_DIR=tools/ci/midi2
-DPULP_HAS_WINRT_MIDI=ON
```

(Pulp keeps its own non-vcpkg deps via `setup.sh`; the vcpkg toolchain here
only adds the midi2 + cppwinrt projection to the find path.)

## Likely failure points (for the parent driving CI iteration)

1. **Windows SDK floor.** If `windows-latest` ever falls below 10.0.26100.0,
   the port aborts with `Need a Windows SDK version that is at least ...`.
   Fix: bump the runner image or install the SDK component explicitly.
2. **Backend API drift.** The drafted `winrt_midi_device.cpp` was written
   against an assumed `winrt::Windows::Devices::Midi2` surface; the real SDK
   namespace is `winrt::Microsoft::Windows::Devices::Midi2` and several
   method signatures differ (init via `MidiDesktopAppSdkInitializer`,
   `SendSingleMessageWords(...)` not `SendSingleMessageWordArray(...)`,
   `MidiClock::TimestampConstantSendImmediately()` not literal `0`). The
   include/namespace/init corrections are applied; deeper signature mismatches
   are the expected first wave of compile errors to iterate on.
3. **vcpkg cache cold-start.** First run builds the projection from the
   `.winmd`; add an `actions/cache` for `~/AppData/Local/vcpkg/archives` if the
   ~minutes of cppwinrt generation dominates.
