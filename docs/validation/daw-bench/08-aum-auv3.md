# 08 — AUM (iOS, AU v3)

**Target quirks**:

- `au_v3_bypass_dual_tracking` (row #21) — AU v3 expects bypass to be tracked
  twice (plugin param + platform `getShouldBypassEffect`).
- `au_v3_host_id_from_wrapper` (row #22) — AU v3 host ID arrives via the
  wrapper, not the executable path.

> AUM is the iPad audio router/mixer. PulpHostBench needs to be built for iOS
> first (`pulp_add_plugin` AUv3 path; see `examples/ios-auv3-synth/` for a
> working example and `pulp:ios` skill for build steps).

**Prereqs**: iPad with iOS 15+, AUM installed from the App Store,
PulpHostBench iOS .ipa side-loaded or distributed via TestFlight.

## Steps

1. **Connect the iPad to a Mac and use macOS Console.app** to capture
   live log output. Filter on subsystem `com.pulp.host-bench` (the bench
   plugin's bundle id) so only its messages show up.

   (The bench plugin writes to the iOS sandbox file system under
   `<app-group>/Library/Logs/PulpHostBench/`. The Console.app live filter
   gives you the same evidence in real time when file access is awkward.)

2. **Launch AUM**, create a new session, add a channel, insert
   PulpHostBench from the AU effects browser.

3. **Confirm the bench plugin logged session start** in the iOS Console
   filter. Look for `processor_construct` with `format=AU`.

4. **Tap the channel's bypass button** in AUM. Tap it again.
   → catalog row **#21**: AU v3 expects both the user-facing bypass param
     and the platform `getShouldBypassEffect` to flip. The bench plugin
     exposes a Bench Bypass parameter (kBenchBypass) that AUM should drive
     via the AU parameter tree. If AUM's track-level bypass also flips the
     param value (visible in the AU's parameter UI), dual-tracking works.

5. **Switch the AUM session sample rate** (Settings → 44.1 / 48 / 96 kHz).
   Watch the Console output for a `prepare` event with the new sample rate.

6. **Background the AUM app** (return to home screen). Wait 10 seconds.
   Reopen AUM. Watch for either `suspend`/`resume` events or
   `process_*` events depending on AUM's background-audio policy.

7. **Save the AUM session, force-quit AUM, relaunch**.
   → confirms iOS state round-trip via `serialize/deserialize_plugin_state`.
     This is also the row #22 evidence: when AUM relaunches and re-inserts
     the plugin, the wrapper-ID-based host detection should resolve to
     AUM (or `Unknown` until AUM's bundle id graduates into the
     `host_type_from_auv3_wrapper()` switch). Record the `host=` value
     from `session_start`.

8. **Quit AUM**.

## Result

| Quirk flag                            | Row | Observed | Notes |
|---------------------------------------|-----|----------|-------|
| `au_v3_bypass_dual_tracking`          | #21 | <C/R/NT> | <step 4: AUM bypass flipped param?> |
| `au_v3_host_id_from_wrapper`          | #22 | <C/R/NT> | <step 7: host string in session_start = ?> |
| `synthesize_bypass_parameter`         | #23 | <C/R/NT> | <general — AU v3 always-on?> |

**Log file(s)**: `<paste console capture or sandbox export>`
**AUM version**: `<x.y>` — **iOS version**: `<x.y>` — **Date**: `<YYYY-MM-DD>`
