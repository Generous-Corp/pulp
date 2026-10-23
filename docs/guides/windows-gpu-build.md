# Windows GPU build loop

This guide describes the bounded Windows loop used to build a Pulp consumer
against a prebuilt Skia/Dawn SDK. It is intended for a disposable Proxmox or
UTM development VM. It does not register a runner and it does not change the
macOS CI pool.

## Retained ARM UTM desktop

The retained ARM64EC guest is a development/demo VM, not a runner. Open its
desktop through the installed macOS helper so the guest uses the active RDP
session rather than UTM's emulated framebuffer:

```sh
cd "$HOME/Library/Application Support/Pulp/WindowsDemo"
./tools/windows/open-utm-rdp.sh
```

The helper starts the selected guest when needed, repairs RDP over the
authenticated SSH channel, and reads the credential from
`~/.config/pulp/secrets/spectr-windows-utm-admin`. Its UTM UUID, forwarded
ports, and Windows account are machine-local configuration; discover them with
`utmctl list` and the helper's installed README instead of copying values from
this guide. The account is written as `PULP-WIN\admin` (the domain and user are
passed separately to RDP). If the helper's configured UUID is absent from
`utmctl list`, stop and repair the local installation or set the documented
`SPECTR_WINDOWS_UTM_UUID` override after verifying the guest identity. Once the
RDP desktop is visible, start REAPER from its shortcut and insert
`VST3: Spectr (Pulp)`.

Interactive evidence must be driven from that RDP desktop. An SSH service
session cannot safely create a visible plugin window, so a successful SSH
command or Task Scheduler result is not a screenshot or interaction receipt.
Keep the selected UTM guest running only for the bounded test/demo session and
stop that same UTM guest with `utmctl stop <verified-uuid>` afterward. Proxmox
VM 300 is a separate x64 guest; stop it only after its own build and host
processes have returned to idle.

## Inputs

Keep the source checkout and the large SDK/build directories on a dedicated
volume. The current Proxmox layout is:

```text
B:\pulp-gpu-source       Pulp checkout
B:\pulp-gpu-sdk-x64      installed Pulp GPU SDK
B:\pulp-fetchcontent     FetchContent cache
B:\pulp-gpu-build-x64    Pulp SDK build tree
B:\spectr-gpu-build-x64  consumer build tree
```

The SDK must contain `lib\cmake\Pulp\PulpConfig.cmake`, the Skia external
build, and the complete VST3 SDK. A development SDK may be used for local
experiments, but it is not distribution evidence until it has a stamped
`sdk-provenance.json`.

## Configure and build a consumer

Run from an elevated-free PowerShell session as the VM build user. The build
parallelism is deliberately bounded; do not replace it with an unbounded
machine-core count.

Open the Visual Studio developer environment first. An SSH-launched PowerShell
does not inherit the MSVC system include paths, even when `cl.exe` is installed;
without this step the compiler reports missing headers such as `algorithm` and
`cstddef`.

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64
```

```powershell
$src = 'C:\spectr-win-proof'
$build = 'B:\spectr-gpu-build-x64'
$pulp = 'B:\pulp-gpu-sdk-x64\lib\cmake\Pulp'

cmake -S $src -B $build -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DPulp_DIR=$pulp `
  -DSPECTR_ENABLE_TEST_SEAMS=OFF `
  -DSPECTR_BUILD_WEBVIEW_REFERENCE=OFF `
  -DSPECTR_SHARED_PRODUCT_ACCEPTANCE=OFF `
  -DSPECTR_GPU_AUDIO_PERF_FIXTURE=OFF

cmake --build $build --parallel 2 --target `
  Spectr_VST3 Spectr_CLAP Spectr_Standalone Spectr-artifact-test Spectr-native-shot
```

The configure output must identify the Skia external build and the Dawn
Graphite provider. After the build, record SHA-256 values for the VST3, CLAP,
standalone executable, `wgpu_native.dll`, and the build-info receipt. Keep the
receipt beside the artifacts so a host test cannot accidentally use an older
headless binary.

For a VST3 bundle, copy the VST3 binary to
`Contents\x86_64-win\Spectr.vst3` inside the installed bundle. Copying a DLL to
the bundle root leaves the host loading the previous binary. Keep the runtime
sidecars and build-info receipt at the bundle root.

## Evidence ladder

Use the following order and report each result separately:

1. `Spectr-native-shot` proves the Windows native Skia/Dawn UI and interaction
   seams. On the current Proxmox VM the adapter is Microsoft Basic Render
   Driver/WARP, so this is Windows UI proof, not physical-GPU performance proof.
2. Install the VST3 into the common VST3 directory and rescan REAPER in an
   interactive desktop session. The host must report the plugin and its editor
   must visibly render the native UI. A scan cache entry alone is insufficient.
3. Exercise modulation and an audio signal in REAPER, capturing the host log
   and a screenshot. Repeat with Ableton only after REAPER is green.
4. For ARM64, repeat the same ladder in UTM. Do not infer ARM64 host support
   from an x64 bridge or from a headless artifact.

The artifact-test executable is useful for latency and adapter checks, but a
run that initializes the plugin and then stops producing output is
`INCONCLUSIVE`; never turn that into a pass by increasing the timeout.

## Cleanup

Only remove named disposable build directories or processes owned by the test.
Do not stop a user's REAPER session and do not use a machine-wide process kill
on a shared desktop. Preserve the previous installed plugin until the new
artifact has been hash-checked and staged.
