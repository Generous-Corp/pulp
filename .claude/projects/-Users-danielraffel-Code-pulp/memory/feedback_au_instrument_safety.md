---
name: AU instrument vs effect safety
description: AU instruments MUST use MusicDeviceBase, not AUEffectBase — wrong base class crashes DAWs
type: feedback
---

AU instruments (aumu) MUST use `au_v2_instrument.cpp` + `PULP_AU_INSTRUMENT()`, not `au_v2_adapter.cpp` + `PULP_AU_PLUGIN()`.

**Why:** AUEffectBase requires audio input buses. Instruments have none. Using AUEffectBase for an instrument causes a crash in Logic Pro when the host tries to process audio through non-existent input buses. The crash manifests as a PAC exception in AUHostingServiceXPC.

**How to apply:** When creating a new AU plugin, check `PluginDescriptor::category`:
- `PluginCategory::Effect` → `PULP_AU_PLUGIN()` (AUEffectBase)
- `PluginCategory::Instrument` → `PULP_AU_INSTRUMENT()` (MusicDeviceBase)
- `PluginCategory::MidiEffect` → needs its own adapter (not AUEffectBase)

Always look at how PulpTone (instrument) vs PulpGain (effect) are set up as reference. Never copy AU entry points from an effect when making an instrument.

Also: the `fill_cocoa_view_info()` function in `au_v2_cocoa_view.mm` crashes in Logic's sandboxed XPC hosting process — needs defensive null checking.
