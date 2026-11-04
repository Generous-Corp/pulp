# Windows plugin delivery contract

This is the acceptance contract for a Pulp plugin that ships on Windows. It
applies to native ARM64, ARM64EC/ARM64X, and native x64 builds. A successful
compiler or installer run is only the source and packaging layers; it does not
prove that a DAW can load the installed module.

## Keep the architecture lanes separate

Publish the architecture in the artifact name and manifest, and keep the
payload in the matching VST3 architecture directory:

| Lane | PE machine | Intended consumer |
| --- | --- | --- |
| native ARM64 | `0xAA64` | ARM64 Windows processes and native ARM64 hosts |
| ARM64EC (sometimes packaged as ARM64X) | ARM64 host plus CHPE/EC metadata | x64-compatible Windows processes on ARM64 Windows |
| native x64 | `0x8664` | Intel/AMD Windows |

Do not infer one lane from another, or copy an x64 payload into an ARM64
package. The installer must remove stale architecture directories before
copying the selected payload so an old module cannot win a DAW scan.

ARM64EC has an additional provider and link requirement. Skia/Dawn archives
must themselves contain an explicitly compatible EC ABI; a pure ARM64 archive
cannot be relabeled and is the primary cause of the familiar seven unresolved
STL thunk symbols. Configure and package guards must inspect provider
provenance and fail closed when an ARM64EC consumer sees only ARM64 members.

After the provider is correct, MSVC still emits STL helper calls using the EC
thunk ABI. The ARM64 import library carries compatible members but also has
runtime directives that conflict with Pulp's static-runtime (`/MT`) SDK closure.
Pulp's Windows SDK therefore ships a deterministic thunk-only archive: it
retains the ARM64EC members, strips only their `.drectve` sections, and
excludes the locale implementation supplied by the static CRT. Link that
archive through the exported Pulp canvas target; do not silence unresolved
symbols or relabel a native ARM64 library as ARM64EC.

## Required provenance

Every release candidate records:

1. the source commit and clean-tree result;
2. the selected lane and PE/CHPE metadata;
3. SHA-256 for the packaged module and the installed module;
4. the installer exit code and an installed-location receipt; and
5. the exact host and module loaded by the DAW.

The installed module hash must be bound to the host receipt. A ZIP listing,
file timestamp, or successful installer exit is not runtime evidence.

## Installed-consumer acceptance

Use a fresh Windows profile and perform the following sequence for each lane:

1. Run the installer and verify the receipt and installed VST3 path.
2. Rescan the exact VST3 directory in REAPER (or another target DAW).
3. Instantiate the plugin and record the module hash, host architecture, and
   editor window handle/foreground ownership.
4. Change a parameter and verify that the observed value follows the request
   within the host's reported precision; save and restore state.
5. Render a short audio fixture and verify non-silent output and stable sample
   rate/channel count.
6. Close and relaunch the host, then repeat load and unload.

An editor screenshot is useful product evidence, but it cannot replace the
module hash, parameter/state, audio, and relaunch receipts. Mark any missing
layer `NOT_EVALUATED` instead of calling the lane passed.

## Unsigned private testing and public release

An unsigned binary may be tested in a disposable private VM after the user
accepts the Windows Defender and SmartScreen risk. Do not add Defender
exclusions, rename executables, or weaken the installer to hide a quarantine.
For a public release, either use a trusted Authenticode/Trusted Signing
identity or obtain Microsoft false-positive clearance. A quarantined standalone
executable is a release blocker even when the VST3 module itself passes DAW
acceptance.

## RDP, UTM, and Perfetto diagnosis

Remote display can make a correct renderer feel slow. Capture the exact build
under test with Pulp tracing enabled and record guest dimensions, DPI, active
session, editor bounds, and the RDP/UTM transport. Use the UI/GPU categories
(`render,gpu,text,js,layout,canvas`) and a large ring (at least
`PULP_TRACE_RING_KB=262144` for script-heavy captures). Before analysis, verify
that the trace has no data loss, buffer wrap, or unfinished slices and that it
contains positive-control UI events.

Separate wall time from CPU time: a long wall-time frame with little guest CPU
may be remote transport or scheduling, while long paint/GPU/JavaScript spans
inside the guest are renderer work. Keep this diagnosis independent from the
audio callback and never move tracing onto the real-time audio thread.

Keep timed captures separate from functional screenshots and state dumps. Do
not compile, capture screenshots, or write per-frame diagnostic files during
the timed workload. Require the intended editor to mount: a successful process
exit from a fallback editor does not validate the scripted interface.

Record the actual renderer adapter. In a Windows ARM VM, D3D12 may use the
Microsoft Basic Render Driver (WARP), which executes GPU work in software.
Compare CPU recording time with explicitly enabled GPU timestamp measurements;
a cheap paint span does not establish a cheap frame. Qualify native GPU
performance separately from software rendering and remote display transport.

For expensive static leaf artwork, `View::set_raster_cached(true)` (or the
script bridge's `setRasterCached(id, true)`) retains pixels while preserving
invalidation on content, geometry, scale, and device changes. Select the node
through an authored semantic marker rather than a generated importer ID. Keep
live analyzer and modulation layers drawing. Confirm actual reuse through the
`view_raster_cache_hit`, `view_raster_cache_miss`, and
`view_raster_cache_bypass` trace events, then compare the same binary with the
cache off, on, and off again under the same audio and interaction workload.

Audio capture advances independently of animation time. `HostFramePump` can
report a nominal resume delta after a long frame; using that delta to throttle
audio draining can skip another slow frame and overflow a bounded capture
buffer. Drain `VisualizationBridge` on every delivered frame and throttle UI
publication separately. For a latest-window analyzer, retain its bounded
backlog policy and verify sequence progress and dropped-frame counts under
load; a larger buffer alone does not prove the scheduling defect fixed.

Check initial, maximized, and restored windows using native parent/editor
client dimensions as well as screenshots. Measure settling latency: a stale
frame during resize and persistent clipping need different fixes. Preserve
complete parsed state snapshots, since copying a live JSON file during its
rewrite can capture an empty or partial document.

## Optional dependencies stay optional

Platform- or application-specific dependencies belong in the plugin or its
explicit opt-in feature. “Do not add any of them to Pulp core for an
application that does not use them” means that an application which does not
enable a feature must not pay for, link, initialize, or package that feature's
dependency. This keeps the core SDK portable and prevents one Windows plugin's
packaging choices from becoming every plugin's transitive requirement.
