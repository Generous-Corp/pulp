# Windows REAPER editor acceptance packet

`windows_editor_smoke.ps1` is the Windows counterpart to the macOS-only
`reaper_smoke.py` editor path. It starts one portable REAPER process, stages one
exact VST3 or CLAP artifact in a temporary directory, inserts a
**format-qualified** FX (`VST3:` or `CLAP:`), and records a receipt. It never
installs into the system plugin directory or kills an existing REAPER process.

```powershell
.\windows_editor_smoke.ps1 `
  -ReaperExe 'C:\Program Files\REAPER\reaper.exe' `
  -Format vst3 `
  -PluginPath 'C:\build\Spectr.vst3' `
  -PluginName 'Spectr' `
  -EvidenceDir 'C:\evidence\spectr-arm64-reaper' `
  -ParamIndex 0 `
  -ContextX 900 -ContextY 500
```

Exit codes are **0 PASS**, **1 FAIL**, and **3 INCONCLUSIVE**. The last state is
used for missing scan-cache evidence, a headless session, or an absent status
handshake; it is never treated as a pass. The receipt records source/staged
paths, a staged file hash manifest, format, host status, scan-cache match, and
screenshot paths. Supplying `-ContextX/-ContextY` adds a right-click
context-menu capture. `-ParamIndex` drives one real REAPER parameter; `-1`
records that modulation was not attempted.

REAPER's Windows scan cache stores the plug-in basename and loader metadata,
not the scan directory. The packet therefore requires the exact staged
basename and plug-in name in the cache, then records the staged path and hash
as the separate artifact binding.

## Audio and performance companion gates

The GUI smoke does not assert audio correctness. For the same exact artifact,
run the cross-platform Pulp audio harness from the Windows SDK build:

```powershell
pulp audio render --plugin C:\build\Spectr.vst3 --format vst3 `
  --out C:\evidence\spectr-wet.wav --duration-ms 2000 `
  --input-signal noise:123 --sample-rate 48000 --block 128 `
  --wav-format float32 --json
pulp audio validate summarize C:\evidence\spectr-wet.wav --json
pulp audio validate doctor C:\evidence\spectr-wet.wav --thd --json
```

Repeat with Spectr's documented bypass/dry parameter and `--latency-report`,
then require the delayed null to pass. Keep wet, bypass, modulation-on, and
modulation-off WAVs plus JSON reports. The offline harness proves the bundle's
audio path; only the REAPER run proves host insertion and editor behavior.

For UI/GPU timing, use a separate **dev-only** traced build with the exact same
source and SDK SHA, never the release installer:

```powershell
$env:PULP_TRACE_PATH='C:\evidence\spectr-arm64-reaper.pftrace'
$env:PULP_TRACE_SECONDS='20'
& 'C:\Program Files\REAPER\reaper.exe' -portable -cfgfile C:\evidence\run\reaper.ini
pulp trace doctor
pulp trace query 'select name,dur from slice order by dur desc limit 30' `
  --trace C:\evidence\spectr-arm64-reaper.pftrace
```

Run this packet once on ARM64 and once on x64. Compare p95/p99 frame/render
spans and host-reported xruns against a predeclared baseline; do not invent a
threshold after observing the result. Perfetto does not instrument the live
audio thread, so no trace replaces WAV/null/xrun evidence.

## Installer status

This packet accepts a portable plugin artifact. It does not imply that an NSIS
installer exists. An installer is a separate gate: run `pulp ship package` only
when NSIS is available, record the `.exe` SHA/size and install/uninstall logs,
then rerun this packet against the installed VST3/CLAP paths.
