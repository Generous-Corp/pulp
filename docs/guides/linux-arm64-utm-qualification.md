# Linux ARM64 qualification in an Apple Silicon VM

This guide describes a repeatable qualification lane for Pulp plugins on an
Apple Silicon Mac without replacing or dual-booting macOS. The reference
machine is an AArch64 Arch/Omarchy guest in UTM using Apple's Hypervisor. The
guest and the plugin use ARM64 instructions directly; this is not an x86
emulation lane.

The lane is useful for source, packaging, editor, and host integration checks.
It does not replace native Linux hardware qualification, and a VM GPU result
must not be generalized to every Linux GPU.

## Build against the portable SDK

Use a published Pulp SDK and keep the SDK's portable ICU setting disabled. A
release SDK built with `PULP_SYSTEM_ICU=OFF` does not require the guest's ICU
major version to match the SDK build host.

```sh
cmake -S . -B build-arm64 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DPulp_DIR=/opt/pulp-sdk-<version>/lib/cmake/Pulp
ninja -C build-arm64 <standalone> <plugin>_VST3 <plugin>_CLAP
```

Record the source SHA, SDK version and SHA-256 of every final binary. Do not
label a binary ARM64EC, x86_64 or release-ready from the directory name alone;
inspect the machine type and keep each architecture's receipt separate.

## Install per-user plugins

For a per-user smoke test, install the produced bundles here:

- VST3: `~/.vst3/<Name>.vst3/Contents/aarch64-linux/<Name>.so`
- CLAP: `~/.clap/<Name>.clap`

Keep the standalone executable outside the plugin directories. A package or
archive is only a transport artifact until a host loads the installed module.

## Host checks

Use a native ARM64 host for each DAW. The following checks are separate gates:

1. **Discovery:** the host scanner finds the bundle and its factory/class ID.
2. **Instantiation:** a project creates an instance of the exact installed
   module; capture the module path and hash while the host is running.
3. **Editor:** the editor opens and reports its real logical/physical size.
4. **Automation:** change a parameter through the host and record the before
   and after value.
5. **Audio:** render or play a known input through the instance and retain a
   non-silent output receipt. A scanner result or editor screenshot is not an
audio result.
6. **Unload:** close the project, unload the module and relaunch the host.

REAPER's ARM64 Linux build can be launched in the guest with the graphical
session environment (`XDG_RUNTIME_DIR`, `WAYLAND_DISPLAY`, `DISPLAY`, and the
user D-Bus address). If Vulkan is exposed by the VM, Pulp's Dawn/Skia path
should report the adapter and backend in the host log. If no adapter is
available, qualify the CPU fallback separately.

Ardour's native ARM64 VST3 scanner is a useful discovery gate. It does not by
itself prove editor, automation or audio behavior.

Bitwig's current official Linux package is x86_64/amd64. Do not install it in
an ARM64 guest and call that an ARM64 qualification result; use a native
x86_64 machine or VM for that lane.

## VM-specific evidence

Record these fields for every run:

- guest kernel and architecture (`uname -a`)
- VM/UTM version and graphics mode
- host/DAW version and architecture
- exact installed module path and SHA-256
- Pulp SDK version and portable-ICU setting
- discovery, instantiation, editor, automation, audio and unload results
- any GPU adapter/backend and whether CPU fallback was used

A release may include a Linux artifact only after the exact source, package,
installed-host and runtime receipts are complete for the target architecture.
Keep an ARM64 VM qualification archive separate from public release assets
until that bar is met.
