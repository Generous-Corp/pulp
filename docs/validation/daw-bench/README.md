# DAW Bench — Manual Validation Scripts

This directory holds the per-DAW manual test scripts for the
**PulpHostBench** validation plugin (`examples/host-bench-plugin/`).

The goal is to graduate `Speculative` rows in
`core/format/include/pulp/format/host_quirks.hpp::HostQuirksMeta`
into `Validated` by observing the host's behavior in a real DAW
session.

## How the package fits together

```
   examples/host-bench-plugin/
       ↓ builds → AU + VST3 + CLAP bundles + Standalone host
                  emits one log per session under
                  ~/Library/Logs/PulpHostBench/
                  (macOS — Linux + Windows use platform-default
                  log dirs; see bench_logger.hpp)

   docs/validation/daw-bench/
       11 numbered .md scripts                     ← YOU ARE HERE
       (Logic, Cubase x2, Live, Bitwig, Reaper x2,
        AUM, Studio One, Wavelab, FL Studio)

   tools/scripts/promote_quirk_tiers.py
       Reads the .log files written during the scripts above.
       Emits a unified diff against
       core/format/include/pulp/format/host_quirks.hpp that
       promotes the rows the logs confirmed.
```

## End-to-end workflow (~30 min for one DAW)

1. **Build the bench plugin** in your Pulp checkout:
   ```bash
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPULP_ENABLE_GPU=OFF
   cmake --build build --target PulpHostBench_CLAP PulpHostBench_Standalone
   # macOS, with AudioUnitSDK installed:
   cmake --build build --target PulpHostBench_AU
   # macOS, with VST3 SDK installed:
   cmake --build build --target PulpHostBench_VST3
   ```
2. **Install** into the platform plugin folders:
   - AU:   `cp -R build/AU/PulpHostBench.component ~/Library/Audio/Plug-Ins/Components/`
   - VST3: `cp -R build/VST3/PulpHostBench.vst3 ~/Library/Audio/Plug-Ins/VST3/`
   - CLAP: `cp -R build/CLAP/PulpHostBench.clap ~/Library/Audio/Plug-Ins/CLAP/`
3. **Clear stale logs** so this session's events stand alone:
   ```bash
   rm -rf ~/Library/Logs/PulpHostBench/   # macOS
   ```
4. **Open the per-DAW script** for whatever you're benching:
   - [`01-logic-pro-au.md`](01-logic-pro-au.md)
   - [`02-cubase-12-vst3.md`](02-cubase-12-vst3.md)
   - [`03-cubase-9-vst3.md`](03-cubase-9-vst3.md)
   - [`04-live-vst3.md`](04-live-vst3.md)
   - [`05-bitwig-vst3.md`](05-bitwig-vst3.md)
   - [`06-reaper-vst3.md`](06-reaper-vst3.md)
   - [`07-reaper-clap.md`](07-reaper-clap.md)
   - [`08-aum-auv3.md`](08-aum-auv3.md)
   - [`09-studio-one-vst3.md`](09-studio-one-vst3.md)
   - [`10-wavelab-vst3.md`](10-wavelab-vst3.md)
   - [`11-fl-studio-vst3.md`](11-fl-studio-vst3.md)
5. **Follow the numbered steps** in the chosen script. Each step
   says "do action X in the DAW", "now check the log file for
   line Y". The script names the specific quirk row(s) being
   confirmed.
6. **Fill in the Result table** at the bottom of the script.
   Mark each row Confirmed / Refuted / Not Triggered. Save the
   resulting `.md` so it can be pasted back into the follow-up PR.
7. **Run the aggregator** when you've benched as many hosts as
   you plan to in this round:
   ```bash
   python3 tools/scripts/promote_quirk_tiers.py \
       ~/Library/Logs/PulpHostBench/*.log \
       --quirks-header core/format/include/pulp/format/host_quirks.hpp \
       --output /tmp/promote.patch
   git apply /tmp/promote.patch
   git diff core/format/include/pulp/format/host_quirks.hpp
   ```
8. **Open a PR** that includes the patch plus the filled-in
   scripts under `docs/validation/daw-bench/results/<date>/`.

## What the bench plugin logs

Every script's "check the log" steps reference one of these stable
event names:

| Event                          | Trigger                                                            |
|--------------------------------|--------------------------------------------------------------------|
| `session_start`                | Processor instance constructed                                     |
| `session_end`                  | Processor instance destroyed                                       |
| `processor_construct`          | Includes plugin version + detected host                            |
| `processor_destruct`           | Symmetric counterpart                                              |
| `define_parameters`            | Host built the parameter list                                      |
| `prepare`                      | Host called `prepare()` — sample rate + max buffer + channels      |
| `release`                      | Host called `release()`                                            |
| `suspend` / `resume`           | Host stalled/resumed processing (preset load, sample-rate change)  |
| `bus_layout_proposal`          | Host asked "do you support inputs=X outputs=Y?"                    |
| `transport_changed`            | Host transport flipped play state or jumped position               |
| `tempo_changed`                | Host BPM changed                                                   |
| `serialize_plugin_state`       | Host wrote a project snapshot                                      |
| `deserialize_plugin_state`     | Host restored a project snapshot                                   |
| `view_opened` / `closed`       | Editor opened or closed                                            |
| `view_resized`                 | Editor resized                                                     |
| `process_without_prepare`      | **Spec violation** — host called `process()` before `prepare()`    |
| `process_sample_rate_drift`    | **Drift** — runtime SR differs from `prepare()` SR                 |
| `process_buffer_overrun`       | **Drift** — runtime buffer larger than `prepare()` max             |
| `process_is_playing_edge`      | First block to see a transport-state edge                          |
| `sidechain_edge`               | Sidechain bus connected/disconnected                               |
| `midi_in`                      | Block carried inbound MIDI events                                  |

The aggregator only reads the event names + the optional `key=value`
fields after them, so adding new always-additive events is safe.

## Filing results

When a per-DAW session is complete:

1. Copy the filled-in `.md` to
   `docs/validation/daw-bench/results/YYYY-MM-DD/<NN-host>.md`
   (the results subdir is checked in but session files live under
   it; create the dated subfolder on first use).
2. Include the matching `.log` file(s) under
   `docs/validation/daw-bench/results/YYYY-MM-DD/logs/` if they're
   small enough; large logs can be summarized in the `.md` and
   linked from a gist.
3. Run the aggregator + include the resulting patch in the PR
   that promotes the tier flips.

Where the catalog row says "Reference evidence" — that's a
*study trail*, not a porting source. The Pulp bench reproduces the
quirk from the host alone; the aggregator's promotion is the
in-tree validation evidence. The license-hygiene rules in
`planning/2026-05-24-daw-host-quirks-inheritance.md` still apply.
