# Windows REAPER editor evidence

`windows_editor_smoke.ps1` starts an isolated REAPER profile, stages an exact
VST3/CLAP artifact, inserts a format-qualified FX, and captures its editor.
Its PASS is scoped to host insertion, editor capture and optional host parameter
readback. Product control semantics, actual UI modulation, animation and live
audio continuity require a product scenario and remain `NOT_EVALUATED` here.

```powershell
.\windows_editor_smoke.ps1 `
  -ReaperExe 'C:\Program Files\REAPER\reaper.exe' `
  -SeedProfile "$env:APPDATA\REAPER" `
  -Format vst3 -PluginPath 'C:\build\Example.vst3' -PluginName 'Example' `
  -EvidenceDir 'C:\evidence\example-arm64-reaper' `
  -ParamIndex 0 -AutomationSec 8 -TimeoutSec 60
```

Use an initialized, registered profile whose first-run dialogs have already
been dismissed. The harness copies the non-secret profile into a private
temporary profile outside the evidence directory, preserves the license only in
that private profile, and removes old VST/CLAP scan caches before the exact
staged plugin is scanned. Windows REAPER's `-cfgfile` resource directory
controls startup and cache resolution; copying only the INI can fall back to
stale user cache entries and load an old Spectr backup. Cleanup removes that
copied profile. It never edits the source profile or prints license contents.
Exported evidence contains no profile or license. Keep user REAPER processes
and projects untouched.

Run this from the logged-in Windows desktop session that owns the RDP or UTM
display. The harness refuses Session 0 or another non-interactive context by
requiring an Explorer process in the current Windows session. Its receipt
records the session ID and Explorer PIDs, binds every owned REAPER process to
that session, and records the foreground window session during editor
activation. This prevents a successful host handshake from being mistaken for
a visible editor when the runner is detached from the desktop.

For CLAP, pass required adjacent files explicitly, for example
`-SidecarPath 'C:\build\icudtl.dat'`. VST3 bundles are copied recursively.
The receipt contains a named SHA256/size manifest for staged files, the actual
REAPER PE architecture, and the exact loaded plugin module path/hash/machine.
The cache basename only establishes scan progress; it is never artifact proof.
The isolated scan root is last in the VST3 scan list for duplicate-class
resolution. Default roots retain REAPER's literal `%COMMONPROGRAMFILES%` and
`%LOCALAPPDATA%` tokens: expanded equivalents can cause REAPER to append its
literal defaults after the staged root. The private INI is written as UTF-8
without a BOM, including on Windows PowerShell 5.1. A mismatching live module
makes the run INCONCLUSIVE.

The host executable determines the VST scan key: `vstpath64` for the x64 ABI and
`vstpath_winarm64` for ARM64/ARM64EC/ARM64X. ARM64EC can retain an AMD64 COFF
header; validated CHPE load-config metadata distinguishes that hybrid from an
AMD64 image without CHPE metadata. Receipts preserve raw COFF fields, metadata
status and derived ABI separately. Unavailable/unsupported metadata refuses scan
routing. These fields do not assert every instruction's ISA or native ARM64
execution. CLAP uses `clap_path`.
Startup, scan and parameter action have separate bounded budgets. A unique run
token prevents stale host status from satisfying the handshake. Terminal
status retains insertion evidence, actual normalized parameter observations,
changes and readback error, and a saved project. Host automation is not proof
that an LFO was assigned through the plugin UI.

Exit code **0** means scoped PASS; **3** means INCONCLUSIVE. Every failure after
the evidence directory is created writes a terminal receipt, including cleanup
errors. Cleanup verifies retained process/start identity. The Lua driver binds
the actual floating editor HWND; the wrapper requires that window to be present,
visible and owned. Before requesting activation, it revalidates process/start,
loaded PE and HWND ownership. It then observes actual foreground ownership over
at most 51 checks separated by 100 ms, revalidating ownership each time;
activation API success alone is insufficient. The receipt retains the activation
result and last foreground HWND/PID even on refusal. Foreground and pointer targets
must belong to that editor or its children/popups before capture/input. Optional `-ContextX/-ContextY`
captures before and after a guarded right click; inspect the image to establish
that a menu painted. Screenshot existence alone does not prove menu contents.

## Fixture controls

```powershell
pwsh -NoProfile -File .\test_windows_editor_smoke.ps1
```

With Lua 5.3+ and an existing scratch directory:

```sh
lua test_windows_editor_smoke.lua windows_editor_smoke.lua /absolute/scratch/path
```

These run parser, PE architecture, path quoting, status/readback, stale token,
wrong-module and durable failure controls. The Lua test executes the actual
driver with a mocked host, including ignored parameter writes. They establish
harness behavior; a fresh Windows runtime receipt and image review are still
required for each architecture and exact PE.

## Product interaction and audio companion gates

Keep product names, control geometry, settings schema, preset comparison and
modulation targets in a product scenario. Require before/after screenshots and
fresh token-acknowledged host state for knobs, dropdown choices, actual context
menu selection, drawn bands, settings and preset save/change/reload. Prove an
assigned LFO from enabled/target/depth readback and successive changing frames
of the target region. API parameter oscillation is a different test.

For continuity during interactions, record real-time wet output plus a known
continuous dry reference in REAPER. Retain device/backend, latency, transport
position versus monotonic wall time, last underrun timestamps, actual recorded
files and exact binary identities. Check finite/unclipped output, unexpected
interior silence, reference corruption, capture length and transport drift.
Offline render/null tests supplement this evidence. Dummy Audio establishes
only bounded host-engine behavior, not physical speaker/hardware continuity.

For UI/GPU timing use a separate development trace build from the same source.
On Windows, start the complete private profile with
`-newinst -cfgfile <ini> -new -nosplash <startup.lua>`. A minimal profile can
stall before startup-script dispatch; the harness copies the non-secret profile
and removes stale scan caches before launching. Neither `-portable` nor
`-script` is supported.
Compare the same assigned modulation with mouse still versus band drawing, and
settings mouse-still versus scrolling. Buffer input QPC/UTC timing records and
capture screenshots/state outside the measured windows. Verify received event
spans and actual changed state because Windows can coalesce pointer messages.
Queue-inclusive input latency is not the duration of a window-procedure span.
Keep guest processing time distinct from VNC/RDP/mobile display transport.

Perfetto does not instrument the live audio thread. Absent DSP spans do not
prove zero xruns. Verify trace completeness and use live recording/host telemetry
for audio. An incomplete modulation setup can still have a diagnostic band-draw
capture, explicitly marked modulation unverified; full acceptance remains open.

Run fresh evidence on ARM64 and x64, then repeat against the final release
artifact. Portable plugin evidence does not establish installer acceptance;
install/uninstall and installed-path rebinding are separate gates.
