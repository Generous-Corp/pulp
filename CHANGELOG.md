# Changelog

All notable changes to Pulp are documented here. Each entry links
to its [GitHub Release](https://github.com/danielraffel/pulp/releases).

<!-- This file is auto-regenerated after every SDK tag by Shipyard's
     `shipyard changelog regenerate` post-tag hook. Edits are picked up on the
     next regen as long as they land in the right release's bullet block. See
     docs/guides/versioning.md § Release pipeline for the full end-to-end flow. -->

<a id="v06680"></a>
## [0.668.0] - 2026-07-13

- feat(gpu-audio): run SuperConvolver's convolution as a WGSL compute shader in the browser ([#6094](https://github.com/danielraffel/pulp/pull/6094))
- fix(super-convolver-ui): tell the truth about a loaded IR, and make the drop zone safe ([#6093](https://github.com/danielraffel/pulp/pull/6093))
- fix(web-player): publish.sh cried wolf on a successful publish ([#6110](https://github.com/danielraffel/pulp/pull/6110))
- chore(web-player): 0.2.1 — publish the corrected README ([#6109](https://github.com/danielraffel/pulp/pull/6109))
- docs(web-player): make the README true ([#6108](https://github.com/danielraffel/pulp/pull/6108))
- chore(web-player): unattended publish — file-backed token, 1Password is backup only ([#6107](https://github.com/danielraffel/pulp/pull/6107))
- chore(web-player): a publish script that cannot ship stale source ([#6106](https://github.com/danielraffel/pulp/pull/6106))
- chore(web-player): 0.2.0 — file upload (dialog + drag-and-drop) ([#6105](https://github.com/danielraffel/pulp/pull/6105))

<a id="v06650"></a>
## [0.665.0] - 2026-07-13

- test: guard the ODR class the Debug lane catches, and stop the Yoga gate crying wolf ([#6098](https://github.com/danielraffel/pulp/pull/6098))
- docs(skill): pulp-web-demo — the player now implements file upload ([#6099](https://github.com/danielraffel/pulp/pull/6099))
- feat(web-player): file upload — dialog button + drag-and-drop drop zone ([#6097](https://github.com/danielraffel/pulp/pull/6097))
- docs(ci-skill): correct the Intel leg and record the release-contention walls ([#6070](https://github.com/danielraffel/pulp/pull/6070))

<a id="v06640"></a>
## [0.664.0] - 2026-07-13

- feat(skill): pulp-web-demo file upload — config + drop-zone contract ([#6091](https://github.com/danielraffel/pulp/pull/6091))
- fix(skill): pulp-web-demo gallery.emit + optional theme; restore clobbered gotchas ([#6085](https://github.com/danielraffel/pulp/pull/6085))
- fix(test): remove ODR violation in the MF-3 denormal null test ([#6081](https://github.com/danielraffel/pulp/pull/6081))

<a id="v06631"></a>
## [0.663.1] - 2026-07-13

- test(view): measure the per-frame Yoga layout rebuild — and conclude not to optimize it ([#6052](https://github.com/danielraffel/pulp/pull/6052))
- test(view): setWidgetShader's contract holds without Skia too ([#6076](https://github.com/danielraffel/pulp/pull/6076))
- build(examples): order ParamInfo designators declaration-first (GCC rejects them) ([#6082](https://github.com/danielraffel/pulp/pull/6082))
- fix(skill): pulp-web-demo import-map resolution + mountDemo feature parity ([#6080](https://github.com/danielraffel/pulp/pull/6080))
- skill(pulp-web-demo): add the generative web-demo skill ([#6078](https://github.com/danielraffel/pulp/pull/6078))
- fix(view): include host_frame_pump outside PULP_HAS_SKIA (mac build break) ([#6079](https://github.com/danielraffel/pulp/pull/6079))
- ci(release): remove run-name — it hid every release run from the VM supervisor ([#6077](https://github.com/danielraffel/pulp/pull/6077))

<a id="v06630"></a>
## [0.663.0] - 2026-07-13

- fix(view): give widgets accessibility roles that are actually true ([#6073](https://github.com/danielraffel/pulp/pull/6073))
- ci(release): don't rebuild a stuck tag while a NEWER tag is still building ([#6075](https://github.com/danielraffel/pulp/pull/6075))
- ci(release): make publication single-owner so slow releases still ship ([#6053](https://github.com/danielraffel/pulp/pull/6053))

<a id="v06620"></a>
## [0.662.0] - 2026-07-12

- fix(view): measure the frame delta instead of assuming 1/60 ([#6072](https://github.com/danielraffel/pulp/pull/6072))

<a id="v06610"></a>
## [0.661.0] - 2026-07-12

- fix(super-convolver): slice the web IR rebuild off the render callback; add IR upload ([#6056](https://github.com/danielraffel/pulp/pull/6056))
- ci(release): fix the auval gate's broken pipeline + re-block it ([#6061](https://github.com/danielraffel/pulp/pull/6061))

<a id="v06600"></a>
## [0.660.0] - 2026-07-12

- feat(signal): add latency-proven pristine oversampling ([#6041](https://github.com/danielraffel/pulp/pull/6041))

<a id="v06581"></a>
## [0.658.1] - 2026-07-12

- ci(release): widen sign-and-release appcast poll for the Intel-slowed build ([#6049](https://github.com/danielraffel/pulp/pull/6049))

<a id="v06580"></a>
## [0.658.0] - 2026-07-12

- fix(view): make the shader/effect surface tell the truth ([#6046](https://github.com/danielraffel/pulp/pull/6046))
- test: deflake SpectralStack phase test + serialize GPU-device tests ([#6043](https://github.com/danielraffel/pulp/pull/6043))

<a id="v06570"></a>
## [0.657.0] - 2026-07-12

- fix(format): keep semantic kind independent of quantization ([#6040](https://github.com/danielraffel/pulp/pull/6040))
- feat(build): PULP_MACOS_ARCH + `pulp build --arch` for universal/thin macOS builds ([#6036](https://github.com/danielraffel/pulp/pull/6036))

<a id="v06550"></a>
## [0.655.0] - 2026-07-12

- docs: fix GPU-audio/render doc inaccuracies found in accuracy audit ([#6015](https://github.com/danielraffel/pulp/pull/6015))

<a id="v06540"></a>
## [0.654.0] - 2026-07-11

- feat(release): ship Intel (x86_64) macOS by cross-compiling on Apple Silicon ([#6024](https://github.com/danielraffel/pulp/pull/6024))
- docs(licensing): credit PreText in Projects That Inspired Pulp ([#6022](https://github.com/danielraffel/pulp/pull/6022))
- ci(tart): thorough stray-VM reaper + scheduled cleanup ([#6021](https://github.com/danielraffel/pulp/pull/6021))
- fix(release): harden the release pipeline so advisory legs can't skip publish ([#6020](https://github.com/danielraffel/pulp/pull/6020))
- feature/voice renderer block kernel ([#6000](https://github.com/danielraffel/pulp/pull/6000))

<a id="v06530"></a>
## [0.653.0] - 2026-07-11

- feature/sf1 adapter boundary core ([#6003](https://github.com/danielraffel/pulp/pull/6003))
- feature/mf7 wclap host extensions ([#6001](https://github.com/danielraffel/pulp/pull/6001))
- fix/appcast wait window ([#6002](https://github.com/danielraffel/pulp/pull/6002))
- fix/bench brace ([#6012](https://github.com/danielraffel/pulp/pull/6012))
- fix/wam wclap planar test ([#6009](https://github.com/danielraffel/pulp/pull/6009))
- test(wam): repair test_wam_adapter for the planar process() signature ([#6004](https://github.com/danielraffel/pulp/pull/6004))
- docs/gpu roofline sequence ([#5999](https://github.com/danielraffel/pulp/pull/5999))
- fix/screenshot sync stale brew path ([#5998](https://github.com/danielraffel/pulp/pull/5998))

<a id="v06520"></a>
## [0.652.0] - 2026-07-11

- chore/reconcile sdk minor bump ([#5997](https://github.com/danielraffel/pulp/pull/5997))
- feature/export osc sdk ([#5935](https://github.com/danielraffel/pulp/pull/5935))

<a id="v06510"></a>
## [0.651.0] - 2026-07-11

- feature/multi convolve roofline ([#5944](https://github.com/danielraffel/pulp/pull/5944))
- fix/coreaudio input only open ([#5810](https://github.com/danielraffel/pulp/pull/5810))

<a id="v06500"></a>
## [0.650.0] - 2026-07-11

- fix/skia canvas stroke fill path ([#5814](https://github.com/danielraffel/pulp/pull/5814))
- feature/config doc gate ([#5953](https://github.com/danielraffel/pulp/pull/5953))
- feature/p0 governance declare ([#5952](https://github.com/danielraffel/pulp/pull/5952))
- refactor(signal): SF-2 unify SIGNAL-side crossfades + fix live_kernel law ([#5913](https://github.com/danielraffel/pulp/pull/5913))
- web(wam): make WAM planar end-to-end and dedup the entry ABI (SF-3) ([#5958](https://github.com/danielraffel/pulp/pull/5958))
- fix(signal): gate ladder fast-tanh behind a flag, default to exact std::tanh ([#5928](https://github.com/danielraffel/pulp/pull/5928))
- format: remove adapter render-thread hard edges (MF-8 + PF-3) ([#5911](https://github.com/danielraffel/pulp/pull/5911))
- fix/release gate merge subject 5939 ([#5971](https://github.com/danielraffel/pulp/pull/5971))
- feature/intel vm tooling ([#5994](https://github.com/danielraffel/pulp/pull/5994))
- perf(gpu-audio): O(n) sliding-window spectral smear on the CPU engine ([#5927](https://github.com/danielraffel/pulp/pull/5927))
- fix(gpu-audio): correct GpuConvolver CPU fallback (MF-4) ([#5930](https://github.com/danielraffel/pulp/pull/5930))
- core/format f64: advertise CLAP 64-bit ports, hoist boundary conversion off the native path ([#5895](https://github.com/danielraffel/pulp/pull/5895))
- test(gpu): cover WaveNet 33<=C<=64 multi-channel-per-lane + B=1; fix roofline serialMAC column ([#5967](https://github.com/danielraffel/pulp/pull/5967))

<a id="v06490"></a>
## [0.649.0] - 2026-07-11

- fix(super-convolver): DSP level-match + Size windowing + no-clip + glitch fixes, editor polish, and customizable installer default ([#5974](https://github.com/danielraffel/pulp/pull/5974))
- core/format LV2: RT hygiene — pre-reserve MIDI, FTZ guard, latency port (MF-2) ([#5890](https://github.com/danielraffel/pulp/pull/5890))
- fix(ci): update build-dir test for merge_group label-exclude condition ([#5982](https://github.com/danielraffel/pulp/pull/5982))
- feat(release): publish native Intel (darwin-x64) macOS CLI + SDK ([#5968](https://github.com/danielraffel/pulp/pull/5968))
- fix(gpu_audio): keep muted stack layers' phase advancing (SF-4 regression) ([#5959](https://github.com/danielraffel/pulp/pull/5959))
- ci: run required checks on merge_group so a merge queue can land PRs ([#5975](https://github.com/danielraffel/pulp/pull/5975))

<a id="v06470"></a>
## [0.647.0] - 2026-07-10

- fix(release): one workflow owns the release; bare title + CHANGELOG-§ footer ([#5960](https://github.com/danielraffel/pulp/pull/5960))
- fix(release-notes): resolve direct-commit entries to PR links, not bare SHAs ([#5956](https://github.com/danielraffel/pulp/pull/5956))
- test(runtime): close consumed==0 harness race in Handoff hammer ([#5949](https://github.com/danielraffel/pulp/pull/5949))
- ci(intel): pin universal-arch-gate to GitHub-hosted macOS, not the Studios ([#5933](https://github.com/danielraffel/pulp/pull/5933))
- feat(examples): retain WebView on the processor and attach via NativeViewHost ([#5932](https://github.com/danielraffel/pulp/pull/5932))
- fix(au): clamp AU v2 instrument output elements + sync view-bridge skill ([#5934](https://github.com/danielraffel/pulp/pull/5934))
- docs+fix(view): document foreign-framework coexistence; keep CPU idle pump alive in modal run-loop modes ([#5931](https://github.com/danielraffel/pulp/pull/5931))

<a id="v06450"></a>
## [0.645.0] - 2026-07-10

- refactor(gpu_audio): SF-4 GPU spectral family consolidation ([#5929](https://github.com/danielraffel/pulp/pull/5929))
- fix(format): clear transient worker-pool reheat latch ([#5926](https://github.com/danielraffel/pulp/pull/5926))
- perf(signal): fast-tanh the ladder filter, cache panner gains ([#5880](https://github.com/danielraffel/pulp/pull/5880))
- perf(gpu-audio): batch GpuConvolver channels into one submit + one readback ([#5883](https://github.com/danielraffel/pulp/pull/5883))

<a id="v06440"></a>
## [0.644.0] - 2026-07-10

- feat(runtime): add Slot<T> and Handoff<T>; port ConvolverIrSwapper onto Handoff ([#5876](https://github.com/danielraffel/pulp/pull/5876))
- fix(audio): close runtime post-merge review gaps ([#5917](https://github.com/danielraffel/pulp/pull/5917))
- docs: fix web-plugin doc inaccuracies (toolchain/target names, CLAP + webview versions) ([#5884](https://github.com/danielraffel/pulp/pull/5884))
- perf(web): remove audio-thread allocations in WAM/WCLAP worklet paths ([#5887](https://github.com/danielraffel/pulp/pull/5887))

<a id="v06430"></a>
## [0.643.0] - 2026-07-10

- core/signal: denormal policy (MF-3) + inner-loop transcendental eviction (PF-2) ([#5889](https://github.com/danielraffel/pulp/pull/5889))

<a id="v06410"></a>
## [0.641.0] - 2026-07-10

- Harden sampler runtime and executor audio paths ([#5888](https://github.com/danielraffel/pulp/pull/5888))

<a id="v06390"></a>
## [0.639.0] - 2026-07-10

- perf(gpu): block-parallel modal_strike — cooperate across the modes sum ([#5881](https://github.com/danielraffel/pulp/pull/5881))
- feat(gpu): roofline/occupancy harness for the gpu_audio compute passes ([#5877](https://github.com/danielraffel/pulp/pull/5877))
- WebCLAP 1:1 demo gallery: all 23 plugins + two gallery indexes ([#5874](https://github.com/danielraffel/pulp/pull/5874))
- perf(gpu): block-parallel WaveNet layer — fill the device at real-time B ([#5878](https://github.com/danielraffel/pulp/pull/5878))
- Redesign in-page synthLoop: seamless, evolving 8-bar loop ([#5872](https://github.com/danielraffel/pulp/pull/5872))
- F2-S1: graph→wasm emitter spike — bit-exact fused compiled tier + measured KILL verdict ([#5875](https://github.com/danielraffel/pulp/pull/5875))
- Rename package to @danielraffel/web-player for npm publish ([#5873](https://github.com/danielraffel/pulp/pull/5873))
- Remove private flag from @pulp/web-player for npm publish ([#5871](https://github.com/danielraffel/pulp/pull/5871))
- Live kernel iter 2: 14 nodes, live signal-flow graph, per-node conformance ([#5867](https://github.com/danielraffel/pulp/pull/5867))
- feat(tools): turnkey Managed SDK Consumer Sweep runner ([#5865](https://github.com/danielraffel/pulp/pull/5865))
- WS-C2: production real-time WebCLAP behind the shared @pulp/web-player ([#5861](https://github.com/danielraffel/pulp/pull/5861))

<a id="v06381"></a>
## [0.638.1] - 2026-07-09

- M1 "Pulp Live" editor: scrub any number, hear it in ~3 ms (no COI) ([#5866](https://github.com/danielraffel/pulp/pull/5866))
- S0 spike: resident-worklet live kernel VM (edit -> crossfade -> hear it) ([#5864](https://github.com/danielraffel/pulp/pull/5864))
- Add headless measurement rig for the own-real-time-compiler spike ([#5860](https://github.com/danielraffel/pulp/pull/5860))

<a id="v06380"></a>
## [0.638.0] - 2026-07-09

- WS-C2 spike: real-time WebCLAP through an AudioWorklet (Architecture A) ([#5858](https://github.com/danielraffel/pulp/pull/5858))
- WS-C1: WebCLAP Cloudflare Pages deploy dir + local isolation/audio proof ([#5853](https://github.com/danielraffel/pulp/pull/5853))
- WS-F(prep): vendor the web-demo player as @pulp/web-player ([#5856](https://github.com/danielraffel/pulp/pull/5856))

<a id="v06370"></a>
## [0.637.0] - 2026-07-09

- feat(tool): register trace-processor as a first-class pulp tool ([#5850](https://github.com/danielraffel/pulp/pull/5850))

<a id="v06360"></a>
## [0.636.0] - 2026-07-09

- feat(wam): WAMv2 web-audio ABI — MIDI/SysEx/state/rack + review-sweep hardening ([#5809](https://github.com/danielraffel/pulp/pull/5809))
- docs/link tracing dev tools ([#5849](https://github.com/danielraffel/pulp/pull/5849))
- feature/trace plugin chain example ([#5848](https://github.com/danielraffel/pulp/pull/5848))
- feature/trace mcp proxies ([#5844](https://github.com/danielraffel/pulp/pull/5844))

<a id="v06350"></a>
## [0.635.0] - 2026-07-09

- feature/trace processor pin ([#5841](https://github.com/danielraffel/pulp/pull/5841))

<a id="v06340"></a>
## [0.634.0] - 2026-07-09

- feature/trace query offline ([#5825](https://github.com/danielraffel/pulp/pull/5825))

<a id="v06260"></a>
## [0.626.0] - 2026-07-09

- fix(test): tolerate unresolved GPU timestamp under load in FFT timing test ([#5800](https://github.com/danielraffel/pulp/pull/5800))

<a id="v06240"></a>
## [0.624.0] - 2026-07-08

- fix(format): standalone derives the playhead change flags like the adapters ([#5789](https://github.com/danielraffel/pulp/pull/5789))

<a id="v06231"></a>
## [0.623.1] - 2026-07-09

- docs/readme getting started install polish ([#5782](https://github.com/danielraffel/pulp/pull/5782))

<a id="v06220"></a>
## [0.622.0] - 2026-07-08

- docs: polish README onboarding flow ([#5774](https://github.com/danielraffel/pulp/pull/5774))

<a id="v06190"></a>
## [0.619.0] - 2026-07-08

- feat(host): click-free crossfade for the SignalGraph live plugin swap ([#5763](https://github.com/danielraffel/pulp/pull/5763))
- docs/readme verification collapse ([#5767](https://github.com/danielraffel/pulp/pull/5767))
- plan/install trust hardening ([#5761](https://github.com/danielraffel/pulp/pull/5761))

<a id="v06170"></a>
## [0.617.0] - 2026-07-08

- feat(host): 2.2c live plugin swap — foundation (graph load + admission) ([#5748](https://github.com/danielraffel/pulp/pull/5748))

<a id="v06150"></a>
## [0.615.0] - 2026-07-08

- codex/sdk hardening 20260708 ([#5725](https://github.com/danielraffel/pulp/pull/5725))

<a id="v06140"></a>
## [0.614.0] - 2026-07-08

- cli: pulp bake / pulp bake verify (#5728) ([#5741](https://github.com/danielraffel/pulp/pull/5741))
- docs/reload lifecycle info main ([#5714](https://github.com/danielraffel/pulp/pull/5714))

<a id="v06130"></a>
## [0.613.0] - 2026-07-08

- feat(state): freeze SequencerStateChannel as a parametric public primitive ([#5740](https://github.com/danielraffel/pulp/pull/5740))
- docs: explain why only hot-reload carries signing/trust + the rack-reload boundary ([#5738](https://github.com/danielraffel/pulp/pull/5738))
- docs: clarify DSP hot-reload vs live graph editing (which swap, and why trust differs) ([#5737](https://github.com/danielraffel/pulp/pull/5737))

<a id="v06120"></a>
## [0.612.0] - 2026-07-08

- feat(format): add CLAP remote controls support ([#5734](https://github.com/danielraffel/pulp/pull/5734))
- docs: surface the bake + signed .pulpbake capability in modules + capabilities ([#5726](https://github.com/danielraffel/pulp/pull/5726))

<a id="v06110"></a>
## [0.611.0] - 2026-07-08

- bake: signed on-disk .pulpbake codec (#5003 slice 3) ([#5701](https://github.com/danielraffel/pulp/pull/5701))

<a id="v06100"></a>
## [0.610.0] - 2026-07-07

- feat/test: RT-guard AU-v2 instrument + standalone render ([#5715](https://github.com/danielraffel/pulp/pull/5715))
- feat(release): human, high-level release notes (strip jargon, fold noise, coalesce) ([#5709](https://github.com/danielraffel/pulp/pull/5709))

<a id="v06091"></a>
## [0.609.1] - 2026-07-07

- fix(view): Windows editor WM_SIZE reconcile + suboptimal-swapchain ([#5708](https://github.com/danielraffel/pulp/pull/5708))

<a id="v06090"></a>
## [0.609.0] - 2026-07-07

- feat(format): sample-accurate parameter output for VST3 + CLAP ([#5706](https://github.com/danielraffel/pulp/pull/5706))
- test(view): mac editor reopen display-link coverage ([#5704](https://github.com/danielraffel/pulp/pull/5704))

<a id="v06080"></a>
## [0.608.0] - 2026-07-07

- feat(view): global per-class ViewPool + prepare_for_reuse ([#5703](https://github.com/danielraffel/pulp/pull/5703))

<a id="v06070"></a>
## [0.607.0] - 2026-07-07

- feat(view): VirtualGrid recycling 2D grid widget ([#5700](https://github.com/danielraffel/pulp/pull/5700))
- feat(release): tier release-note weight by semver bump level ([#5697](https://github.com/danielraffel/pulp/pull/5697))
- docs: acknowledge Truce inspiration ([#5698](https://github.com/danielraffel/pulp/pull/5698))

<a id="v06060"></a>
## [0.606.0] - 2026-07-07

- bake: lower in-process Custom nodes (#5003 slice 2) ([#5694](https://github.com/danielraffel/pulp/pull/5694))
- ci(gates): hotspot guard enforces net-delta vs merge-base (T1.2) ([#5678](https://github.com/danielraffel/pulp/pull/5678))
- fix/virtual list ci followup ([#5685](https://github.com/danielraffel/pulp/pull/5685))

<a id="v06050"></a>
## [0.605.0] - 2026-07-08

- feat(view): off-UI-thread index/search query service (R7) ([#5577](https://github.com/danielraffel/pulp/pull/5577))

<a id="v06040"></a>
## [0.604.0] - 2026-07-07

- bake: extract lowerability_of() gate (#5003 slice 1) ([#5687](https://github.com/danielraffel/pulp/pull/5687))

<a id="v06030"></a>
## [0.603.0] - 2026-07-07

- signal-graph: no-silence live topology swap — begin_swap_edit/prepare_swap (2.2b) ([#5677](https://github.com/danielraffel/pulp/pull/5677))
- feature/release notes humanization ([#5565](https://github.com/danielraffel/pulp/pull/5565))

<a id="v06020"></a>
## [0.602.0] - 2026-07-07

- feat(view): add gesture recognizer arbiter ([#5594](https://github.com/danielraffel/pulp/pull/5594))
- ci(version): version-at-land single-writer bot — dry-run (T1.1 stage 1) ([#5673](https://github.com/danielraffel/pulp/pull/5673))
- docs: document the host-resource build governor (tiers) ([#5679](https://github.com/danielraffel/pulp/pull/5679))

<a id="v06000"></a>
## [0.600.0] - 2026-07-07

- feat(cli): report active build-governance tier in `pulp status` ([#5676](https://github.com/danielraffel/pulp/pull/5676))
- ci(shipyard): govern the local-backend build with a tartci host lease ([#5666](https://github.com/danielraffel/pulp/pull/5666))

<a id="v05990"></a>
## [0.599.0] - 2026-07-07

- feat(view): add plugin-host IME text input ([#5567](https://github.com/danielraffel/pulp/pull/5567))

<a id="v05980"></a>
## [0.598.0] - 2026-07-07

- ci(triage): add pr_check_triage.py — is a red check pre-existing on main or your regression? ([#5665](https://github.com/danielraffel/pulp/pull/5665))
- ci(gates): add planning-gitlink guard + pre-PR closeout/hygiene docs ([#5662](https://github.com/danielraffel/pulp/pull/5662))

<a id="v05970"></a>
## [0.597.0] - 2026-07-07

- feat(midi): add Scala tuning provider ([#5639](https://github.com/danielraffel/pulp/pull/5639))
- signal-graph: cache plugin metadata at prepare() so swap-time compile makes no live PluginSlot call (2.2b H2) ([#5653](https://github.com/danielraffel/pulp/pull/5653))

<a id="v05960"></a>
## [0.596.0] - 2026-07-07

- refactor(cli): one source of truth for the no-lease build job count ([#5646](https://github.com/danielraffel/pulp/pull/5646))

<a id="v05950"></a>
## [0.595.0] - 2026-07-07

- feature/virtual list recycling ([#5641](https://github.com/danielraffel/pulp/pull/5641))
- fix(pre-push): make the diff-coverage build unmistakable (not a "hung push") ([#5644](https://github.com/danielraffel/pulp/pull/5644))

<a id="v05940"></a>
## [0.594.0] - 2026-07-07

- feature/r3 widget bridge codegen ([#5606](https://github.com/danielraffel/pulp/pull/5606))
- feat(lottie): enable + test native skottie rendering on Skia m151 ([#5645](https://github.com/danielraffel/pulp/pull/5645))

<a id="v05930"></a>
## [0.593.0] - 2026-07-07

- chore(deps): bump V8 to 15.2.24 + Skia/Dawn to chrome/m151 (structural version parse + mirror-drift lint) ([#5638](https://github.com/danielraffel/pulp/pull/5638))

<a id="v05920"></a>
## [0.592.0] - 2026-07-07

- feat(reload): opt-in remote UX fetch + enforce require_signed at the watcher ([#5545](https://github.com/danielraffel/pulp/pull/5545))

<a id="v05910"></a>
## [0.591.0] - 2026-07-07

- test(signal-graph): prove compile_() is race-free vs live process() (2.2a no-silence-swap contract) ([#5621](https://github.com/danielraffel/pulp/pull/5621))
- test(cli): raise pulp shell-out timeout to de-flake the macOS gate under -j8 ([#5589](https://github.com/danielraffel/pulp/pull/5589))

<a id="v05900"></a>
## [0.590.0] - 2026-07-07

- Add optional MTS-ESP microtuning and structured state values ([#5625](https://github.com/danielraffel/pulp/pull/5625))

<a id="v05890"></a>
## [0.589.0] - 2026-07-07

- Add double precision audio processing support ([#5578](https://github.com/danielraffel/pulp/pull/5578))

<a id="v05880"></a>
## [0.588.0] - 2026-07-06

- feat(view): first-class NativeViewHost widget for native child embedding ([#5575](https://github.com/danielraffel/pulp/pull/5575))

<a id="v05870"></a>
## [0.587.0] - 2026-07-06

- docs(skills): capture Skia symlink-loop / Shipyard tree-drift gotcha durably ([#5599](https://github.com/danielraffel/pulp/pull/5599))

<a id="v05860"></a>
## [0.586.0] - 2026-07-06

- docs(audio-quality-lab): plain-language experimental framing + define "net" ([#5595](https://github.com/danielraffel/pulp/pull/5595))
- feat(quality-lab): Tier 3 T3.4 — ratio:auto (uniform-ratio estimation) ([#5593](https://github.com/danielraffel/pulp/pull/5593))
- build: stop tracking external/skia-build/build symlink (#238 hygiene) ([#5588](https://github.com/danielraffel/pulp/pull/5588))

<a id="v05840"></a>
## [0.584.0] - 2026-07-06

- feat(quality-lab): Tier 3 T3.3 — pitch:S (duration-preserving pitch shift) ([#5590](https://github.com/danielraffel/pulp/pull/5590))
- phase/tartci lease governor p3b ([#5569](https://github.com/danielraffel/pulp/pull/5569))

<a id="v05830"></a>
## [0.583.0] - 2026-07-06

- phase/tartci lease governor p3a ([#5566](https://github.com/danielraffel/pulp/pull/5566))
- feat(quality-lab): Tier 3 T3.2 — stretch:R (pitch-preserving time-stretch) ([#5546](https://github.com/danielraffel/pulp/pull/5546))

<a id="v05820"></a>
## [0.582.0] - 2026-07-06

- feat(inspect): scripted-UI runtime inspector / debug console (Valdi R8) ([#5570](https://github.com/danielraffel/pulp/pull/5570))

<a id="v05800"></a>
## [0.580.0] - 2026-07-06

- feature/adapter platform hardening audit ([#5564](https://github.com/danielraffel/pulp/pull/5564))
- ci/decouple required gate preamble ([#5576](https://github.com/danielraffel/pulp/pull/5576))

<a id="v05790"></a>
## [0.579.0] - 2026-07-06

- refactor/design choc text helpers ([#5574](https://github.com/danielraffel/pulp/pull/5574))

<a id="v05782"></a>
## [0.578.2] - 2026-07-06

- feat/fig frame selection ([#5553](https://github.com/danielraffel/pulp/pull/5553))
- refactor/cmd design extract subcommands ([#5554](https://github.com/danielraffel/pulp/pull/5554))

<a id="v05780"></a>
## [0.578.0] - 2026-07-06

- refactor/design text util ([#5555](https://github.com/danielraffel/pulp/pull/5555))

<a id="v05770"></a>
## [0.577.0] - 2026-07-06

- feat/design tweaks block ([#5552](https://github.com/danielraffel/pulp/pull/5552))

<a id="v05760"></a>
## [0.576.0] - 2026-07-06

- feat/design variant props ([#5551](https://github.com/danielraffel/pulp/pull/5551))

<a id="v05750"></a>
## [0.575.0] - 2026-07-06

- feat/design claude folder ([#5543](https://github.com/danielraffel/pulp/pull/5543))

<a id="v05740"></a>
## [0.574.0] - 2026-07-06

- feat/design project ledger ([#5526](https://github.com/danielraffel/pulp/pull/5526))

<a id="v05730"></a>
## [0.573.0] - 2026-07-06

- feat(quality-lab): Tier 3 T3.1 — alignment module + varispeed:R warp class ([#5537](https://github.com/danielraffel/pulp/pull/5537))
- feature/split test cmake manifest ([#5520](https://github.com/danielraffel/pulp/pull/5520))

<a id="v05720"></a>
## [0.572.0] - 2026-07-06

- feat/design gallery ([#5535](https://github.com/danielraffel/pulp/pull/5535))
- feat/design verify loop v2 ([#5532](https://github.com/danielraffel/pulp/pull/5532))
- feat(quality-lab): transient-integrity axis (compare -> 6-of-7 detectors) ([#5527](https://github.com/danielraffel/pulp/pull/5527))

<a id="v05710"></a>
## [0.571.0] - 2026-07-06

- chore: raise frozen LOC ceilings for widget_bridge + test manifest (live-swap growth) ([#5505](https://github.com/danielraffel/pulp/pull/5505))
- feat/design fidelity taxonomy ([#5513](https://github.com/danielraffel/pulp/pull/5513))

<a id="v05700"></a>
## [0.570.0] - 2026-07-06

- feat(quality-lab): --align latency (fixed-latency-trim) for compare ([#5515](https://github.com/danielraffel/pulp/pull/5515))

<a id="v05690"></a>
## [0.569.0] - 2026-07-06

- develop/gpu audio runtime ([#5514](https://github.com/danielraffel/pulp/pull/5514))

<a id="v05680"></a>
## [0.568.0] - 2026-07-06

- feat/design adherence lint ([#5512](https://github.com/danielraffel/pulp/pull/5512))

<a id="v05670"></a>
## [0.567.0] - 2026-07-06

- feat(design): compile a design contract (token + component allowlist) via `pulp design compile` ([#5510](https://github.com/danielraffel/pulp/pull/5510))

<a id="v05660"></a>
## [0.566.0] - 2026-07-06

- feat/offline fig import ([#5509](https://github.com/danielraffel/pulp/pull/5509))
- test: make cross-thread Catch2 assertions thread-safe + add lint guard ([#5507](https://github.com/danielraffel/pulp/pull/5507))

<a id="v05630"></a>
## [0.563.0] - 2026-07-04

- feat(design-import): emit param_key + provenance for geometry knobs (producer side) ([#5495](https://github.com/danielraffel/pulp/pull/5495))
- ci: harden the conflict-marker guard per adversarial review ([#5493](https://github.com/danielraffel/pulp/pull/5493))

<a id="v05620"></a>
## [0.562.0] - 2026-07-03

- feat(view): add a FrameClock activity channel + pump_view_frame seam ([#5487](https://github.com/danielraffel/pulp/pull/5487))

<a id="v05610"></a>
## [0.561.0] - 2026-07-03

- feat(import-design): label faithful_svg fidelity honestly in --validate ([#5483](https://github.com/danielraffel/pulp/pull/5483))

<a id="v05600"></a>
## [0.560.0] - 2026-07-03

- feat(view): bind geometry-detected faithful-vector controls to host params ([#5481](https://github.com/danielraffel/pulp/pull/5481))
- ci: guard against committed git conflict markers on every layer ([#5480](https://github.com/danielraffel/pulp/pull/5480))

<a id="v05590"></a>
## [0.559.0] - 2026-07-03

- chore: raise hotspot ceilings for the layer-name binding slice ([#5479](https://github.com/danielraffel/pulp/pull/5479))
- fix(build): resolve committed merge-conflict markers in CMakeLists version ([#5477](https://github.com/danielraffel/pulp/pull/5477))
- fix(sampler): mode-gate the footer chrome + slice bands; fit the toolbar buttons ([#5466](https://github.com/danielraffel/pulp/pull/5466))

<a id="v05580"></a>
## [0.558.0] - 2026-07-03

- feat(view): paint-safe meter/scalar host→view source with FrameClock self-update ([#5472](https://github.com/danielraffel/pulp/pull/5472))

<a id="v05570"></a>
## [0.557.0] - 2026-07-03

- feat(view): make DesignFrameView emit_* a supported protected extension point ([#5471](https://github.com/danielraffel/pulp/pull/5471))
- docs(ci): lead flaky-required-wedge recovery with the shipyard rescue one-liner ([#5469](https://github.com/danielraffel/pulp/pull/5469))
- ci: retry transient flakes on the required test legs ([#5467](https://github.com/danielraffel/pulp/pull/5467))

<a id="v05560"></a>
## [0.556.0] - 2026-07-03

- feat(view): promote needs_continuous_frames to a public predicate ([#5465](https://github.com/danielraffel/pulp/pull/5465))

<a id="v05540"></a>
## [0.554.0] - 2026-07-02

- feat(quality-lab): stereo-width compare axis (compare -> 5-of-7, closes the stereo gap) ([#5457](https://github.com/danielraffel/pulp/pull/5457))
- feat(quality-lab): golden-render regression net (the daily-driver verify loop) ([#5452](https://github.com/danielraffel/pulp/pull/5452))

<a id="v05530"></a>
## [0.553.0] - 2026-07-02

- feat(quality-lab): wire noise-roughness + graininess axes (compare -> 4-of-7) ([#5448](https://github.com/danielraffel/pulp/pull/5448))
- chore(lint): scan source comments and test tags for transient breadcrumbs ([#5430](https://github.com/danielraffel/pulp/pull/5430))

<a id="v05520"></a>
## [0.552.0] - 2026-07-02

- feat(view): complete widget↔parameter automation + ComboBox polish ([#5419](https://github.com/danielraffel/pulp/pull/5419))
- feat(quality-lab): promote corroboration disagreement into the headline ([#5447](https://github.com/danielraffel/pulp/pull/5447))
- fix(quality-lab): make added-hf band-relative (dB), fix the bass-heavy blind spot ([#5443](https://github.com/danielraffel/pulp/pull/5443))
- docs(skills): add code-comments skill for durable comments ([#5421](https://github.com/danielraffel/pulp/pull/5421))
- fix(quality-lab): disclose downmix, guard added-hf band, surface DC offset ([#5439](https://github.com/danielraffel/pulp/pull/5439))
- docs/planning audio measurement brief ([#5432](https://github.com/danielraffel/pulp/pull/5432))
- docs(guides): clarify the template's siblings mode needs both adapter checkouts ([#5426](https://github.com/danielraffel/pulp/pull/5426))
- docs(guides): add "Putting a Pulp UI in a JUCE plugin" guide ([#5424](https://github.com/danielraffel/pulp/pull/5424))

<a id="v05510"></a>
## [0.551.0] - 2026-07-02

- chore: raise hotspot ceilings for host-surface API + new test targets ([#5422](https://github.com/danielraffel/pulp/pull/5422))
- feature/audio compare corroboration ([#5420](https://github.com/danielraffel/pulp/pull/5420))
- docs(skills): add pr-review-sweep skill for material PRs ([#5416](https://github.com/danielraffel/pulp/pull/5416))

<a id="v05500"></a>
## [0.550.0] - 2026-07-02

- feat(quality-lab): shipped `pulp audio compare` CLI + MCP tool + live recipe (S3) ([#5415](https://github.com/danielraffel/pulp/pull/5415))
- feat(quality-lab): profile registry + added-hf axis for compare (Slice 2) ([#5414](https://github.com/danielraffel/pulp/pull/5414))
- docs: document local signing/notarization credential location ([#5411](https://github.com/danielraffel/pulp/pull/5411))
- feat(quality-lab): /audio-compare slash command + diff-integration (Slice 1b) ([#5413](https://github.com/danielraffel/pulp/pull/5413))
- feat(quality-lab): agent-facing before/after compare→judge report (Slice 1) ([#5410](https://github.com/danielraffel/pulp/pull/5410))
- ci(host_vitals): emit sampled_at in the JSON signal ([#5412](https://github.com/danielraffel/pulp/pull/5412))
- fix/hot reload msvc symbol ([#5407](https://github.com/danielraffel/pulp/pull/5407))
- ci/fork guard scheduled workflows ([#5409](https://github.com/danielraffel/pulp/pull/5409))
- feature/host vitals preflight ([#5405](https://github.com/danielraffel/pulp/pull/5405))

<a id="v05493"></a>
## [0.549.3] - 2026-07-01

- fix/ausdk header leak private ([#5403](https://github.com/danielraffel/pulp/pull/5403))
- feature/audio quality lab aubio eval ([#5404](https://github.com/danielraffel/pulp/pull/5404))
- docs/rust cli sync anchor repair ([#5402](https://github.com/danielraffel/pulp/pull/5402))

<a id="v05492"></a>
## [0.549.2] - 2026-07-01

- fix/hosted editor hover ([#5400](https://github.com/danielraffel/pulp/pull/5400))
- feat/moonbase activation parity ([#5397](https://github.com/danielraffel/pulp/pull/5397))
- feature/shipyard pin 0.70.0 ([#5395](https://github.com/danielraffel/pulp/pull/5395))
- feature/ci skill automerge policy ([#5393](https://github.com/danielraffel/pulp/pull/5393))

<a id="v05491"></a>
## [0.549.1] - 2026-06-30

- fix(reload): stage the initial logic image so an in-place rebuild can't corrupt it ([#5391](https://github.com/danielraffel/pulp/pull/5391))

<a id="v05490"></a>
## [0.549.0] - 2026-06-30

- feature/design frame param key ([#5388](https://github.com/danielraffel/pulp/pull/5388))
- feature/moonbase licensing ([#5387](https://github.com/danielraffel/pulp/pull/5387))

<a id="v05480"></a>
## [0.548.0] - 2026-06-30

- refactor(reload): extract gate_logic_image as the single source of truth ([#5380](https://github.com/danielraffel/pulp/pull/5380))

<a id="v05470"></a>
## [0.547.0] - 2026-06-30

- fix/inventory rust motion identity ([#5342](https://github.com/danielraffel/pulp/pull/5342))

<a id="v05460"></a>
## [0.546.0] - 2026-06-30

- fix/rust aax create status parity ([#5337](https://github.com/danielraffel/pulp/pull/5337))
- fix/compat sync require populated ([#5338](https://github.com/danielraffel/pulp/pull/5338))
- fix(scan-worker): set install rpath ([#5341](https://github.com/danielraffel/pulp/pull/5341))
- test(examples): cover audio inspector render proof helper ([#5343](https://github.com/danielraffel/pulp/pull/5343))

<a id="v05450"></a>
## [0.545.0] - 2026-06-30

- feat(reload): live UI+DSP morph — shell create_view forwarding + thin logic ([#5377](https://github.com/danielraffel/pulp/pull/5377))
- feat(reload): pulp_add_reload_logic helper + DSP hot-reload guide ([#5369](https://github.com/danielraffel/pulp/pull/5369))

<a id="v05440"></a>
## [0.544.0] - 2026-06-29

- feat(reload): click-free crossfade on hot-swap + lock-free reclaim ([#5363](https://github.com/danielraffel/pulp/pull/5363))
- feat(reload): DAW-integration hot-reload shell + live REAPER demo (Phase 1b) ([#5358](https://github.com/danielraffel/pulp/pull/5358))

<a id="v05430"></a>
## [0.543.0] - 2026-06-29

- fix(view): snap Toggle thumb on initial seed so it paints the real state ([#5355](https://github.com/danielraffel/pulp/pull/5355))

<a id="v05420"></a>
## [0.542.0] - 2026-06-29

- feat(reload): file-watch ReloadController for the standalone dev loop (Phase 1) ([#5347](https://github.com/danielraffel/pulp/pull/5347))

<a id="v05410"></a>
## [0.541.0] - 2026-06-29

- feat(reload): verify-before-commit hot-reload transaction + ABI (Phase 1) ([#5346](https://github.com/danielraffel/pulp/pull/5346))
- ci: add coverage-upload watchdog so Codecov starvation screams ([#5288](https://github.com/danielraffel/pulp/pull/5288))

<a id="v05400"></a>
## [0.540.0] - 2026-06-29

- feat(reload): scratch-store contract check + dlopen leak-policy handle (Phase 0) ([#5345](https://github.com/danielraffel/pulp/pull/5345))
- docs(quality-lab): restructure guide + fix quote-escaping in the rendered HTML ([#5344](https://github.com/danielraffel/pulp/pull/5344))

<a id="v05390"></a>
## [0.539.0] - 2026-06-29

- test(aax): SDK-gated runtime test for the AAX MIDI bridge ([#5340](https://github.com/danielraffel/pulp/pull/5340))
- feat(quality-lab): autonomous tuning loop, first slice (experimental) ([#5339](https://github.com/danielraffel/pulp/pull/5339))
- feat(quality-lab): advisory LLM/multimodal reviewer (experimental, never a gate) ([#5336](https://github.com/danielraffel/pulp/pull/5336))
- feat(quality-lab): onset_drift detector (experimental) — timing/groove drift ([#5327](https://github.com/danielraffel/pulp/pull/5327))
- fix(web): find Chrome via CHROME_PATH + Linux paths in the WebCLAP host validator ([#5334](https://github.com/danielraffel/pulp/pull/5334))
- ci: add Web Plugins lane — headless-browser WAMv2 + WebCLAP validation (RD11) ([#5333](https://github.com/danielraffel/pulp/pull/5333))
- feat(format): package WebCLAP modules into .wclap bundles ([#5329](https://github.com/danielraffel/pulp/pull/5329))
- feat(format): emit a WAMv2 web-build report (UI strategy + binding targets) ([#5328](https://github.com/danielraffel/pulp/pull/5328))
- feat(format): host a WebCLAP in the browser with a generated UI ([#5323](https://github.com/danielraffel/pulp/pull/5323))
- feat(quality-lab): maturity gate — experimental detectors are advisory-only ([#5320](https://github.com/danielraffel/pulp/pull/5320))
- test: assert FileChooser null callback behavior ([#5242](https://github.com/danielraffel/pulp/pull/5242))

<a id="v05380"></a>
## [0.538.0] - 2026-06-29

- docs(ci-skill): note exact-LOC ceiling bump for test manifest growth ([#5230](https://github.com/danielraffel/pulp/pull/5230))
- docs(cli-maintenance): note pulp validate --json install_ready/summary contract ([#5314](https://github.com/danielraffel/pulp/pull/5314))

<a id="v05370"></a>
## [0.537.0] - 2026-06-29

- feat(cli): add `pulp build --format wam|wclap` for web plugin formats ([#5315](https://github.com/danielraffel/pulp/pull/5315))
- docs: correct web-plugin status — WebCLAP is built + Node-hosted, not scaffolding ([#5312](https://github.com/danielraffel/pulp/pull/5312))

<a id="v05360"></a>
## [0.536.0] - 2026-06-29

- docs(audio-headless-debug): note HeadlessHost::processor_as for typed state ([#5306](https://github.com/danielraffel/pulp/pull/5306))
- feat(format): host a WebCLAP from pure JS — audio + parameter control ([#5310](https://github.com/danielraffel/pulp/pull/5310))

<a id="v05350"></a>
## [0.535.0] - 2026-06-29

- docs(web): mark WebCLAP as a building, probe-validated canary ([#5308](https://github.com/danielraffel/pulp/pull/5308))

<a id="v05343"></a>
## [0.534.3] - 2026-06-29

- fix(view): plugin grabs the DAW keyboard only for a focused TEXT field ([#5304](https://github.com/danielraffel/pulp/pull/5304))
- docs: web-plugin-support reference + claims lint (Phase 0-WAM) ([#5303](https://github.com/danielraffel/pulp/pull/5303))
- fix(mcp): gate audio render by project SDK ([#5135](https://github.com/danielraffel/pulp/pull/5135))
- wam: Phase 2 — validate instrument (PulpPluck) + rich effect (PulpChorus) ([#5299](https://github.com/danielraffel/pulp/pull/5299))
- docs(render): clarify KTX2 parser status ([#5146](https://github.com/danielraffel/pulp/pull/5146))
- feat(quality-lab): add HNR + stereo-width detectors; file roadmap issues ([#5298](https://github.com/danielraffel/pulp/pull/5298))
- docs(view): align font loading status ([#5102](https://github.com/danielraffel/pulp/pull/5102))

<a id="v05342"></a>
## [0.534.2] - 2026-06-29

- fix/macos auv3 objc namespacing ([#5290](https://github.com/danielraffel/pulp/pull/5290))

<a id="v05341"></a>
## [0.534.1] - 2026-06-29

- test/upgrade banner shellout ([#5289](https://github.com/danielraffel/pulp/pull/5289))
- ci: scrub stale Windows runner registration ([#5074](https://github.com/danielraffel/pulp/pull/5074))
- fix/macos render objc namespacing ([#5264](https://github.com/danielraffel/pulp/pull/5264))

<a id="v05340"></a>
## [0.534.0] - 2026-06-29

- feat(sampler): tap the TEMPO number to type an exact BPM (v1.6.0) ([#5279](https://github.com/danielraffel/pulp/pull/5279))
- fix(cli): honor create output defaults ([#5198](https://github.com/danielraffel/pulp/pull/5198))

<a id="v05331"></a>
## [0.533.1] - 2026-06-29

- fix(cli): silence status git probes on Windows ([#5214](https://github.com/danielraffel/pulp/pull/5214))
- fix(format): parse CLAP/.pulpset floats without std::from_chars<float> ([#5232](https://github.com/danielraffel/pulp/pull/5232))

<a id="v05330"></a>
## [0.533.0] - 2026-06-29

- feat: first-class WAMv2 browser canary (PulpGain loads + renders + generated controls) ([#5266](https://github.com/danielraffel/pulp/pull/5266))
- fix(quality-lab): make the real-engine path work for a pulp tool-installed lab ([#5277](https://github.com/danielraffel/pulp/pull/5277))
- fix(import): reject unknown style selector values ([#5250](https://github.com/danielraffel/pulp/pull/5250))

<a id="v05321"></a>
## [0.532.1] - 2026-06-29

- fix(cli): preserve sdk subcommand fallthrough ([#5034](https://github.com/danielraffel/pulp/pull/5034))
- feat(tool): install the Audio Quality Lab via `pulp tool install` ([#5253](https://github.com/danielraffel/pulp/pull/5253))

<a id="v05320"></a>
## [0.532.0] - 2026-06-28

- docs(import): refresh SwiftUI fallback wording ([#5249](https://github.com/danielraffel/pulp/pull/5249))
- feat/upgrade breaking change agent signal ([#5221](https://github.com/danielraffel/pulp/pull/5221))
- docs(cli): refresh config dispatcher comment ([#5248](https://github.com/danielraffel/pulp/pull/5248))
- feat(ci): merge-time intent-trailer version bump (dormant phase 1) ([#5241](https://github.com/danielraffel/pulp/pull/5241))
- fix(state): clamp parameter default to range at registration ([#5257](https://github.com/danielraffel/pulp/pull/5257))

<a id="v05311"></a>
## [0.531.1] - 2026-06-28

- docs(cli): list package commands in top-level help ([#5255](https://github.com/danielraffel/pulp/pull/5255))
- fix(import): avoid fake import-detect version placeholder ([#5256](https://github.com/danielraffel/pulp/pull/5256))
- chore(cli): clarify import emit unresolved guidance ([#5251](https://github.com/danielraffel/pulp/pull/5251))
- fix/macos objc class namespacing ([#5220](https://github.com/danielraffel/pulp/pull/5220))
- docs(view): align conic gradient status ([#5233](https://github.com/danielraffel/pulp/pull/5233))
- feat(ship): support appcast download URL override ([#5105](https://github.com/danielraffel/pulp/pull/5105))

<a id="v05310"></a>
## [0.531.0] - 2026-06-28

- docs(cli): list operator commands in rust help ([#5247](https://github.com/danielraffel/pulp/pull/5247))
- docs(view): refresh CSS compat narrative counts ([#5246](https://github.com/danielraffel/pulp/pull/5246))
- feature/param designation ([#5243](https://github.com/danielraffel/pulp/pull/5243))
- docs(host): remove stale automation smoothing TODO ([#5240](https://github.com/danielraffel/pulp/pull/5240))

<a id="v05300"></a>
## [0.530.0] - 2026-06-28

- docs(import): clarify generated static control values ([#5238](https://github.com/danielraffel/pulp/pull/5238))
- feat(format): AU v2 channel-config negotiation and MIDI output ([#5236](https://github.com/danielraffel/pulp/pull/5236))
- test: remove corrupt Canvas2D bridge stubs ([#5237](https://github.com/danielraffel/pulp/pull/5237))
- test(cli): escape audio assert fixture paths ([#5213](https://github.com/danielraffel/pulp/pull/5213))
- fix(view): skip redundant hot reloads ([#5216](https://github.com/danielraffel/pulp/pull/5216))
- docs(view): clarify CSS background status ([#5231](https://github.com/danielraffel/pulp/pull/5231))
- docs(cli): clarify focus-mode monitor scope ([#5225](https://github.com/danielraffel/pulp/pull/5225))
- docs(view): clarify loadFont bridge comment ([#5212](https://github.com/danielraffel/pulp/pull/5212))
- feat(view): embed Element.animate web-compat shim ([#5153](https://github.com/danielraffel/pulp/pull/5153))
- test(cli): normalize config shellout newlines ([#5215](https://github.com/danielraffel/pulp/pull/5215))

<a id="v05290"></a>
## [0.529.0] - 2026-06-28

- feat(format): MIDI sub-block split iterator for sample-accurate event handling ([#5235](https://github.com/danielraffel/pulp/pull/5235))
- docs(cli): clarify dev watch fallback ([#5227](https://github.com/danielraffel/pulp/pull/5227))
- fix(sampler): bar-exact loop tempo with host-prior octave resolution ([#5143](https://github.com/danielraffel/pulp/pull/5143))

<a id="v05282"></a>
## [0.528.2] - 2026-06-28

- docs(cli): align coverage diff guidance ([#5095](https://github.com/danielraffel/pulp/pull/5095))
- chore(cli): align loop focus marker contract ([#5073](https://github.com/danielraffel/pulp/pull/5073))
- chore(cmake): clarify plugin MIDI option contract ([#5092](https://github.com/danielraffel/pulp/pull/5092))
- build(cmake): warn on reserved Swift option ([#5150](https://github.com/danielraffel/pulp/pull/5150))
- docs(platform): clarify file dialog backend status ([#5147](https://github.com/danielraffel/pulp/pull/5147))
- docs(events): clarify IAP scaffold scope ([#5133](https://github.com/danielraffel/pulp/pull/5133))
- docs(midi): clarify Web MIDI scaffold status ([#5116](https://github.com/danielraffel/pulp/pull/5116))
- docs(cmake): expose app icon helper in status manifest ([#5110](https://github.com/danielraffel/pulp/pull/5110))
- docs(web): clarify WAM and WCLAP helper status ([#5104](https://github.com/danielraffel/pulp/pull/5104))
- docs(cli): clean docs command source comment ([#5076](https://github.com/danielraffel/pulp/pull/5076))
- docs: align token export format guidance ([#5148](https://github.com/danielraffel/pulp/pull/5148))
- test(harness): sync compat aggregate parts ([#5084](https://github.com/danielraffel/pulp/pull/5084))
- docs(examples): align offline stretch defaults ([#5117](https://github.com/danielraffel/pulp/pull/5117))
- docs(events): clarify Windows notification scaffold ([#5130](https://github.com/danielraffel/pulp/pull/5130))
- docs(motion): clarify record out fixture hint ([#5145](https://github.com/danielraffel/pulp/pull/5145))
- ci: keep workflow_dispatch builds example-free ([#5184](https://github.com/danielraffel/pulp/pull/5184))
- test(format): make rolling WAV env test portable ([#5186](https://github.com/danielraffel/pulp/pull/5186))
- fix(cli): align scan inventory docs ([#5192](https://github.com/danielraffel/pulp/pull/5192))
- test(cli): make Rust parity tests hermetic ([#5200](https://github.com/danielraffel/pulp/pull/5200))
- docs(view): align accessibility status evidence ([#5209](https://github.com/danielraffel/pulp/pull/5209))
- test(canvas): add Linux font golden baselines ([#5210](https://github.com/danielraffel/pulp/pull/5210))

<a id="v05281"></a>
## [0.528.1] - 2026-06-28

- fix(cli): find delegated helpers in multi-config builds ([#5211](https://github.com/danielraffel/pulp/pull/5211))
- docs(canvas): align drawImage status with Skia path ([#5096](https://github.com/danielraffel/pulp/pull/5096))
- docs(examples): document ui-preview benchmark mode ([#5118](https://github.com/danielraffel/pulp/pull/5118))
- docs(view): align ImageView decode status ([#5119](https://github.com/danielraffel/pulp/pull/5119))
- docs(ci): align desktop automation examples ([#5120](https://github.com/danielraffel/pulp/pull/5120))
- docs(import): align import-design source status ([#5129](https://github.com/danielraffel/pulp/pull/5129))
- docs(ship): align capabilities packaging status ([#5139](https://github.com/danielraffel/pulp/pull/5139))
- docs(windows): correct WASAPI capture status ([#5140](https://github.com/danielraffel/pulp/pull/5140))
- docs(claude): clarify plugin guide examples ([#5141](https://github.com/danielraffel/pulp/pull/5141))
- docs(format): align ProcessBuffers multi-bus status ([#5194](https://github.com/danielraffel/pulp/pull/5194))
- docs(view): correct table widget status ([#5203](https://github.com/danielraffel/pulp/pull/5203))
- docs(audio): align JACK status ([#5207](https://github.com/danielraffel/pulp/pull/5207))
- docs(aax): clarify create scaffold gating ([#5208](https://github.com/danielraffel/pulp/pull/5208))

<a id="v05280"></a>
## [0.528.0] - 2026-06-28

- feat(format): route secondary output buses to the Processor in CLAP and VST3 ([#5191](https://github.com/danielraffel/pulp/pull/5191))

<a id="v05270"></a>
## [0.527.0] - 2026-06-28

- fix(format): marshal VST3 restartComponent off the audio thread ([#5190](https://github.com/danielraffel/pulp/pull/5190))

<a id="v05260"></a>
## [0.526.0] - 2026-06-28

- fix(format): make CLAP and .pulpset param text conversion locale-independent ([#5181](https://github.com/danielraffel/pulp/pull/5181))
- fix(view): guard GPU hosted-view drag category behind PULP_HAS_SKIA ([#5185](https://github.com/danielraffel/pulp/pull/5185))
- docs(view): align modulation matrix status ([#5187](https://github.com/danielraffel/pulp/pull/5187))
- docs(format): align AU v2 parameter sync guide ([#5069](https://github.com/danielraffel/pulp/pull/5069))

<a id="v05240"></a>
## [0.524.0] - 2026-06-28

- feat(format): VST3 per-note expression (MPE) via INoteExpressionController ([#5170](https://github.com/danielraffel/pulp/pull/5170))

<a id="v05230"></a>
## [0.523.0] - 2026-06-27

- fix(host): serialize control-thread SignalGraph node-field access against prepare ([#5167](https://github.com/danielraffel/pulp/pull/5167))

<a id="v05220"></a>
## [0.522.0] - 2026-06-28

- feat(designmd): track DESIGN.md format spec 0.3.0 in the importer ([#5136](https://github.com/danielraffel/pulp/pull/5136))

<a id="v05211"></a>
## [0.521.1] - 2026-06-27

- fix(rt): coalesce live main-listener notifications and clamp native-core event offsets ([#5161](https://github.com/danielraffel/pulp/pull/5161))

<a id="v05210"></a>
## [0.521.0] - 2026-06-27

- feat(format): deliver MIDI controllers to VST3 instruments via IMidiMapping ([#5155](https://github.com/danielraffel/pulp/pull/5155))

<a id="v05200"></a>
## [0.520.0] - 2026-06-28

- feature/au render rt safety ([#5152](https://github.com/danielraffel/pulp/pull/5152))

<a id="v05190"></a>
## [0.519.0] - 2026-06-27

- fix(midi): correct MPE glide refcount on steal and retrigger expression re-seed ([#5149](https://github.com/danielraffel/pulp/pull/5149))

<a id="v05180"></a>
## [0.518.0] - 2026-06-27

- fix(host): pin live snapshot in SignalGraph control-thread readers ([#5142](https://github.com/danielraffel/pulp/pull/5142))

<a id="v05173"></a>
## [0.517.3] - 2026-06-27

- fix(sampler): unlinked tempo follows the sample + fractional BPM readout ([#5132](https://github.com/danielraffel/pulp/pull/5132))
- fix(mcp): pulp_audio_render temp-dir leak + opaque-error hardening ([#5134](https://github.com/danielraffel/pulp/pull/5134))
- docs(quality-lab): clean public-facing copy, attribute perceptual models ([#5131](https://github.com/danielraffel/pulp/pull/5131))
- feat(mcp): pulp_audio_render MCP tool + refresh /audio-harness ([#5122](https://github.com/danielraffel/pulp/pull/5122))

<a id="v05172"></a>
## [0.517.2] - 2026-06-27

- fix(view,sampler): file drag-drop in hosted plugins + bar-exact loop tempo ([#5113](https://github.com/danielraffel/pulp/pull/5113))

<a id="v05171"></a>
## [0.517.1] - 2026-06-27

- fix(view,sampler): hosted Musical Typing, tempo persistence, DAW-tempo LINK ([#5108](https://github.com/danielraffel/pulp/pull/5108))
- feat(quality-lab): real-engine regression gate ([#5075](https://github.com/danielraffel/pulp/pull/5075))
- fix(ci): gate diff uses three-dot (merge-base) so behind-branch files aren't flagged ([#5106](https://github.com/danielraffel/pulp/pull/5106))

<a id="v05170"></a>
## [0.517.0] - 2026-06-27

- feature/bypass dry latency ([#5094](https://github.com/danielraffel/pulp/pull/5094))
- feat(quality-lab): real-audio input path (reference-free dry-input) ([#5091](https://github.com/danielraffel/pulp/pull/5091))
- fix(view): map ; and ' so Musical Typing reaches all 18 keys ([#5089](https://github.com/danielraffel/pulp/pull/5089))

<a id="v05160"></a>
## [0.516.0] - 2026-06-27

- fix(state): enforce lock-free param contract and fix CachedProperty default/short-circuit ([#5086](https://github.com/danielraffel/pulp/pull/5086))
- feat(quality-lab): versioned license-guarded corpus + add-source ([#5078](https://github.com/danielraffel/pulp/pull/5078))
- fix(format): guard VST3 and CLAP against oversized render blocks ([#5081](https://github.com/danielraffel/pulp/pull/5081))

<a id="v05150"></a>
## [0.515.0] - 2026-06-27

- fix(cli): validate PULP_AUDIO_CAPTURE_ROLLING_FORMAT env value ([#5083](https://github.com/danielraffel/pulp/pull/5083))
- docs(harness): repair stale compat evidence refs ([#5067](https://github.com/danielraffel/pulp/pull/5067))

<a id="v05140"></a>
## [0.514.0] - 2026-06-27

- feat(cli): int24 option for pulp run --audio-capture-rolling ([#5071](https://github.com/danielraffel/pulp/pull/5071))
- feat(quality-lab): validate the REAL Pulp stretch engine ([#5070](https://github.com/danielraffel/pulp/pull/5070))

<a id="v05130"></a>
## [0.513.0] - 2026-06-27

- feat(cli): pulp audio render delivers --param sample-accurately ([#5068](https://github.com/danielraffel/pulp/pull/5068))
- docs(test): clean audit follow-up breadcrumbs ([#5063](https://github.com/danielraffel/pulp/pull/5063))

<a id="v05120"></a>
## [0.512.0] - 2026-06-27

- fix(state): make StateTree listener fan-out re-entrancy-safe and guard against cycles ([#5062](https://github.com/danielraffel/pulp/pull/5062))
- feat(quality-lab): tonal QualityCase family + spectral_flux detector ([#5061](https://github.com/danielraffel/pulp/pull/5061))
- feature/source comment hygiene next selector 20260627 ([#5060](https://github.com/danielraffel/pulp/pull/5060))

<a id="v05111"></a>
## [0.511.1] - 2026-06-27

- perf(format): flush denormals across all real-time adapter callbacks ([#5059](https://github.com/danielraffel/pulp/pull/5059))
- feature/source comment hygiene ui render 20260627 ([#5058](https://github.com/danielraffel/pulp/pull/5058))
- docs(test): clean audio signal selector tags ([#5057](https://github.com/danielraffel/pulp/pull/5057))
- feat(quality-lab): opt-in ViSQOL adapter + self-describing provenance ([#5055](https://github.com/danielraffel/pulp/pull/5055))
- docs(test): clean format host selector tags ([#5054](https://github.com/danielraffel/pulp/pull/5054))
- feat(quality-lab): worst-region clip export + general run CLI ([#5045](https://github.com/danielraffel/pulp/pull/5045))
- docs: codify diff-driven version bumps + full-build-for-struct-layout rules ([#5052](https://github.com/danielraffel/pulp/pull/5052))
- docs(test): clean view coverage selector tags ([#5049](https://github.com/danielraffel/pulp/pull/5049))
- docs(test): clean CLI coverage selector tags ([#5047](https://github.com/danielraffel/pulp/pull/5047))

<a id="v05110"></a>
## [0.511.0] - 2026-06-27

- fix(sampler,signal): clear held notes on panic + scale onset-head to the window ([#5038](https://github.com/danielraffel/pulp/pull/5038))
- docs(test): clean coverage phase tag breadcrumbs ([#5046](https://github.com/danielraffel/pulp/pull/5046))
- docs(tooling): clean CLI and CMake breadcrumb comments ([#5044](https://github.com/danielraffel/pulp/pull/5044))
- feat(quality-lab): spectral-centroid + hf-fizz detectors ([#5036](https://github.com/danielraffel/pulp/pull/5036))
- docs(test): clean phase tag breadcrumbs ([#5041](https://github.com/danielraffel/pulp/pull/5041))

<a id="v05100"></a>
## [0.510.0] - 2026-06-27

- refactor(graph): replace connection bool-soup with a ConnectionKind discriminator ([#5033](https://github.com/danielraffel/pulp/pull/5033))
- feat(quality-lab): P0a audio quality lab — transient detector + real-PV evidence ([#5028](https://github.com/danielraffel/pulp/pull/5028))
- test(cli): cover pulp fmt shellout behavior ([#5029](https://github.com/danielraffel/pulp/pull/5029))

<a id="v05093"></a>
## [0.509.3] - 2026-06-27

- fix(cli): remove inert auv3 xcode target selector ([#5021](https://github.com/danielraffel/pulp/pull/5021))
- docs(test): clean source test comment breadcrumbs ([#5016](https://github.com/danielraffel/pulp/pull/5016))

<a id="v05092"></a>
## [0.509.2] - 2026-06-26

- docs(ship): align share option docs ([#5018](https://github.com/danielraffel/pulp/pull/5018))
- docs(ship): document appcast CLI surface ([#5013](https://github.com/danielraffel/pulp/pull/5013))
- docs(ship): align Windows package docs ([#5012](https://github.com/danielraffel/pulp/pull/5012))
- docs(ship): document Linux package outputs ([#5010](https://github.com/danielraffel/pulp/pull/5010))
- docs(test): clean stale MIDI coverage tags ([#5006](https://github.com/danielraffel/pulp/pull/5006))
- docs(view): fix hot reload API snippet ([#5008](https://github.com/danielraffel/pulp/pull/5008))
- docs: include design-debug contract tests ([#5007](https://github.com/danielraffel/pulp/pull/5007))

<a id="v05091"></a>
## [0.509.1] - 2026-06-26

- fix(cli): mirror targeted tool doctor in rust ([#4999](https://github.com/danielraffel/pulp/pull/4999))

<a id="v05090"></a>
## [0.509.0] - 2026-06-26

- feat(host): opt-in node transport via cached transport-sensitivity bit ([#4992](https://github.com/danielraffel/pulp/pull/4992))

<a id="v05081"></a>
## [0.508.1] - 2026-06-26

- fix(cli): mirror npm tool wrappers in rust ([#4988](https://github.com/danielraffel/pulp/pull/4988))

<a id="v05080"></a>
## [0.508.0] - 2026-06-27

- fix(signal): STN noise-morph correctness (causal mask + WOLA level) — opt-in only ([#4987](https://github.com/danielraffel/pulp/pull/4987))

<a id="v05070"></a>
## [0.507.0] - 2026-06-26

- feat(signal): onset-boundary soft-start (restore the attack at sample 0) ([#4986](https://github.com/danielraffel/pulp/pull/4986))
- feat(sampler): LOOP toggle engages/disengages currently-held notes ([#4975](https://github.com/danielraffel/pulp/pull/4975))
- refactor(host): extract the legacy serial walk into a reference-runner TU ([#4961](https://github.com/danielraffel/pulp/pull/4961))

<a id="v05060"></a>
## [0.506.0] - 2026-06-26

- feat(signal): restore the phase-vocoder energy loss (RMS make-up + soft-clip) ([#4985](https://github.com/danielraffel/pulp/pull/4985))
- docs(test): clean format adapter hygiene breadcrumbs ([#4984](https://github.com/danielraffel/pulp/pull/4984))
- chore(harness): refresh compatibility coverage report ([#4979](https://github.com/danielraffel/pulp/pull/4979))
- fix(sampler): footer layout — green SLICES by SENS, aligned labels ([#4976](https://github.com/danielraffel/pulp/pull/4976))
- fix(sampler): minimum slice length + zero-crossing slice boundaries ([#4974](https://github.com/danielraffel/pulp/pull/4974))
- docs(stretch): add ears-led tuning methodology to the skill ([#4972](https://github.com/danielraffel/pulp/pull/4972))
- docs(test): consolidate source comment hygiene follow-up ([#4971](https://github.com/danielraffel/pulp/pull/4971))
- docs(ship): fix dev-signing notarize recipe ([#4970](https://github.com/danielraffel/pulp/pull/4970))

<a id="v05050"></a>
## [0.505.0] - 2026-06-26

- fix(signal): high-pass the transient graft (kills blown-out deep hits at stretch) ([#4969](https://github.com/danielraffel/pulp/pull/4969))
- docs(test): refresh audio primitive selector tags ([#4962](https://github.com/danielraffel/pulp/pull/4962))
- test: keep Cmajor edge tests Python 3.9 compatible ([#4964](https://github.com/danielraffel/pulp/pull/4964))

<a id="v05040"></a>
## [0.504.0] - 2026-06-26

- feat(cli): pulp run --audio-capture-rolling — last-N float capture-to-WAV ([#4927](https://github.com/danielraffel/pulp/pull/4927))
- docs(cli): align config skill key list ([#4958](https://github.com/danielraffel/pulp/pull/4958))

<a id="v05030"></a>
## [0.503.0] - 2026-06-26

- fix(signal): refractory gate on transient phase reset (kills blown-out deep hits) ([#4959](https://github.com/danielraffel/pulp/pull/4959))
- docs(test): refresh state/runtime support selector tags ([#4957](https://github.com/danielraffel/pulp/pull/4957))

<a id="v05020"></a>
## [0.502.0] - 2026-06-26

- feat(host): thread per-block transport context into SignalGraph::process ([#4946](https://github.com/danielraffel/pulp/pull/4946))

<a id="v05011"></a>
## [0.501.1] - 2026-06-26

- fix(cli): align project pin surface in rust ([#4956](https://github.com/danielraffel/pulp/pull/4956))
- fix(sampler): unique slice per key, master soft-limit, tempo survives re-slice ([#4954](https://github.com/danielraffel/pulp/pull/4954))
- chore(examples): reuse pluck processor in web demo ([#4951](https://github.com/danielraffel/pulp/pull/4951))
- fix(sampler): release typing notes after root changes ([#4947](https://github.com/danielraffel/pulp/pull/4947))
- docs(test): normalize stale Codex regression labels ([#4941](https://github.com/danielraffel/pulp/pull/4941))
- fix(sampler): typing keyboard plays consecutive slices (home row, not white-key gaps) ([#4943](https://github.com/danielraffel/pulp/pull/4943))
- docs(cli): fix troubleshooting plugin install command ([#4944](https://github.com/danielraffel/pulp/pull/4944))
- docs(format): align ARA companion factory status ([#4940](https://github.com/danielraffel/pulp/pull/4940))

<a id="v05010"></a>
## [0.501.0] - 2026-06-26

- fix(sampler+audio): transient relocation + standalone device-follow, test-tone, slice-race (M1 feedback) ([#4924](https://github.com/danielraffel/pulp/pull/4924))
- docs(audio): align audio harness render guidance ([#4937](https://github.com/danielraffel/pulp/pull/4937))
- feat(host): observe silent routed-to-walk fallback for eligible graphs ([#4920](https://github.com/danielraffel/pulp/pull/4920))
- docs(examples): align screenshot helper contract ([#4935](https://github.com/danielraffel/pulp/pull/4935))

<a id="v05000"></a>
## [0.500.0] - 2026-06-26

- feat(cli): pulp audio render — offline scenario render of a plugin bundle ([#4922](https://github.com/danielraffel/pulp/pull/4922))
- docs(format): list compressor capability examples ([#4931](https://github.com/danielraffel/pulp/pull/4931))

<a id="v04991"></a>
## [0.499.1] - 2026-06-26

- fix(cli): port run audio capture flags to rust ([#4925](https://github.com/danielraffel/pulp/pull/4925))
- docs(test): refresh stale review severity labels ([#4923](https://github.com/danielraffel/pulp/pull/4923))
- docs(webview): remove phase-era wording ([#4918](https://github.com/danielraffel/pulp/pull/4918))

<a id="v04990"></a>
## [0.499.0] - 2026-06-26

- feat(cli): pulp run --audio-capture-wav — dump the live output ring to a WAV ([#4921](https://github.com/danielraffel/pulp/pull/4921))
- docs(view): refresh design provenance breadcrumbs ([#4919](https://github.com/danielraffel/pulp/pull/4919))
- test(native-components): force allocation death-test path ([#4912](https://github.com/danielraffel/pulp/pull/4912))
- docs(test): refresh cli coverage tags ([#4916](https://github.com/danielraffel/pulp/pull/4916))
- docs(test): refresh ui widget coverage tags ([#4913](https://github.com/danielraffel/pulp/pull/4913))
- docs(test): refresh audio coverage tags ([#4910](https://github.com/danielraffel/pulp/pull/4910))

<a id="v04980"></a>
## [0.498.0] - 2026-06-26

- docs(hosting): note null-slot-plugin pass-through + universal eligibility ([#4908](https://github.com/danielraffel/pulp/pull/4908))
- docs(test): refresh runtime coverage tags ([#4906](https://github.com/danielraffel/pulp/pull/4906))
- docs(validation): align HostBench manifest version example ([#4905](https://github.com/danielraffel/pulp/pull/4905))
- docs(test): refresh coverage phase tags ([#4904](https://github.com/danielraffel/pulp/pull/4904))
- docs(audio): correct audio harness live inspector status ([#4902](https://github.com/danielraffel/pulp/pull/4902))
- docs(test): refresh runtime state event labels ([#4901](https://github.com/danielraffel/pulp/pull/4901))
- docs(examples): remove orphan animation demo page ([#4900](https://github.com/danielraffel/pulp/pull/4900))
- docs(test): consolidate source comment hygiene batch ([#4898](https://github.com/danielraffel/pulp/pull/4898))
- docs(cli): correct build configure guidance ([#4893](https://github.com/danielraffel/pulp/pull/4893))

<a id="v04970"></a>
## [0.497.0] - 2026-06-26

- feat(host): route Custom nodes through the canonical executor ([#4894](https://github.com/danielraffel/pulp/pull/4894))

<a id="v04961"></a>
## [0.496.1] - 2026-06-26

- fix(run): align Rust live audio flags ([#4883](https://github.com/danielraffel/pulp/pull/4883))
- chore: adversarial-review follow-ups — atomic version writes, test rigor, comment accuracy ([#4891](https://github.com/danielraffel/pulp/pull/4891))

<a id="v04960"></a>
## [0.496.0] - 2026-06-25

- feat(host): default to canonical-executor routing with per-node telemetry parity ([#4884](https://github.com/danielraffel/pulp/pull/4884))
- docs: align doctor slash command ([#4880](https://github.com/danielraffel/pulp/pull/4880))

<a id="v04953"></a>
## [0.495.3] - 2026-06-25

- fix(create): quote standalone build paths ([#4877](https://github.com/danielraffel/pulp/pull/4877))

<a id="v04952"></a>
## [0.495.2] - 2026-06-25

- chore: harden one-backend series guards and parity tests (series review) ([#4867](https://github.com/danielraffel/pulp/pull/4867))

<a id="v04951"></a>
## [0.495.1] - 2026-06-25

- fix(host): reuse AudioBufferList scratch in AU host slot (RT no-alloc) ([#4866](https://github.com/danielraffel/pulp/pull/4866))

<a id="v04950"></a>
## [0.495.0] - 2026-06-25

- feat(host): bake a SignalGraph into a Processor ([#4864](https://github.com/danielraffel/pulp/pull/4864))

<a id="v04940"></a>
## [0.494.0] - 2026-06-25

- feat(host): add offline SignalGraph render host with online/offline parity ([#4861](https://github.com/danielraffel/pulp/pull/4861))
- docs(host): align SignalGraph routing contract comments ([#4856](https://github.com/danielraffel/pulp/pull/4856))
- refactor(host): harden the anticipation splice + prove determinism for harder graphs ([#4855](https://github.com/danielraffel/pulp/pull/4855))

<a id="v04931"></a>
## [0.493.1] - 2026-06-25

- ci: enforce SignalGraph single-backend governance in gates + CI ([#4848](https://github.com/danielraffel/pulp/pull/4848))

<a id="v04930"></a>
## [0.493.0] - 2026-06-25

- fix(host): isolate parallel plugin routing scratch ([#4850](https://github.com/danielraffel/pulp/pull/4850))
- feat(tempo-sampler): Musical Typing keyboard + adaptive-FFT stretch + m4a (M1-feedback) ([#4774](https://github.com/danielraffel/pulp/pull/4774))

<a id="v04920"></a>
## [0.492.0] - 2026-06-25

- feat(host): wire anticipative rendering into SignalGraph::process (Phase 6 capstone) ([#4841](https://github.com/danielraffel/pulp/pull/4841))
- docs(host): note anticipation skip-mask splice contract ([#4842](https://github.com/danielraffel/pulp/pull/4842))
- docs(host): clarify anticipation subgraph sink shape ([#4834](https://github.com/danielraffel/pulp/pull/4834))

<a id="v04910"></a>
## [0.491.0] - 2026-06-25

- feat(format): optional skip-mask in process_routed for pre-filled nodes ([#4836](https://github.com/danielraffel/pulp/pull/4836))

<a id="v04900"></a>
## [0.490.0] - 2026-06-25

- feat(host): anticipation lane — render an eligible sub-graph ahead of the deadline ([#4833](https://github.com/danielraffel/pulp/pull/4833))
- test(host): render-path proof for the anticipation sub-graph ([#4827](https://github.com/danielraffel/pulp/pull/4827))

<a id="v04890"></a>
## [0.489.0] - 2026-06-25

- feat(host): anticipation sub-graph extraction (renderable interior + sink) ([#4824](https://github.com/danielraffel/pulp/pull/4824))

<a id="v04880"></a>
## [0.488.0] - 2026-06-25

- fix(host): expose SignalGraph parallel cost gate ([#4822](https://github.com/danielraffel/pulp/pull/4822))

<a id="v04870"></a>
## [0.487.0] - 2026-06-25

- feat(host): anticipation partition (eligible interior + boundary edges) ([#4821](https://github.com/danielraffel/pulp/pull/4821))

<a id="v04860"></a>
## [0.486.0] - 2026-06-25

- feat(host): static anticipative-rendering eligibility analysis ([#4820](https://github.com/danielraffel/pulp/pull/4820))

<a id="v04850"></a>
## [0.485.0] - 2026-06-25

- feat(graph): coarse static cost threshold for the parallel graph executor ([#4818](https://github.com/danielraffel/pulp/pull/4818))
- docs: align FAUST guide build type ([#4816](https://github.com/danielraffel/pulp/pull/4816))
- test(host): run-to-run determinism golden for the parallel graph executor ([#4815](https://github.com/danielraffel/pulp/pull/4815))
- refactor(view): remove widget gallery dead height state ([#4813](https://github.com/danielraffel/pulp/pull/4813))

<a id="v04840"></a>
## [0.484.0] - 2026-06-25

- feat(host): opt-in levelized parallel graph execution in SignalGraph::process ([#4812](https://github.com/danielraffel/pulp/pull/4812))

<a id="v04830"></a>
## [0.483.0] - 2026-06-25

- feat(format): levelized parallel graph execution (process_parallel) ([#4806](https://github.com/danielraffel/pulp/pull/4806))
- docs(examples): align PulpSampler parameter name ([#4804](https://github.com/danielraffel/pulp/pull/4804))
- docs(examples): align PulpSampler sample scope ([#4802](https://github.com/danielraffel/pulp/pull/4802))

<a id="v04820"></a>
## [0.482.0] - 2026-06-25

- feat(format): persistent fork-join worker pool for the levelized parallel executor ([#4801](https://github.com/danielraffel/pulp/pull/4801))
- docs(examples): correct PulpSynth screenshot controls ([#4799](https://github.com/danielraffel/pulp/pull/4799))

<a id="v04810"></a>
## [0.481.0] - 2026-06-25

- feat(graph): off-RT plan levelization for static multicore scheduling ([#4798](https://github.com/danielraffel/pulp/pull/4798))

<a id="v04800"></a>
## [0.480.0] - 2026-06-25

- feat(host): route dense audio-rate parameter modulation on the canonical executor ([#4796](https://github.com/danielraffel/pulp/pull/4796))
- test(examples): cover PulpDrums MIDI pass-through ([#4790](https://github.com/danielraffel/pulp/pull/4790))

<a id="v04790"></a>
## [0.479.0] - 2026-06-25

- feat(host): route sparse parameter automation on the canonical executor path ([#4791](https://github.com/danielraffel/pulp/pull/4791))
- test(format): generated DSP reaches a graph through ProcessorNode (one path) ([#4783](https://github.com/danielraffel/pulp/pull/4783))

<a id="v04780"></a>
## [0.478.0] - 2026-06-25

- feat(graph): carry parameter-automation metadata on graph connections ([#4789](https://github.com/danielraffel/pulp/pull/4789))

<a id="v04770"></a>
## [0.477.0] - 2026-06-25

- feat(host): route MIDI edges on the canonical executor path ([#4786](https://github.com/danielraffel/pulp/pull/4786))
- docs(examples): clarify screenshot format labels ([#4782](https://github.com/danielraffel/pulp/pull/4782))
- docs(examples): clarify AU v2 example formats ([#4777](https://github.com/danielraffel/pulp/pull/4777))
- feat(format): run a Processor as an in-process graph node (ProcessorNode) ([#4767](https://github.com/danielraffel/pulp/pull/4767))

<a id="v04760"></a>
## [0.476.0] - 2026-06-25

- feat(host): route sidechain edges on the canonical executor path ([#4778](https://github.com/danielraffel/pulp/pull/4778))

<a id="v04750"></a>
## [0.475.0] - 2026-06-25

- feat(host): per-connection PDC in the canonical executor routing path ([#4776](https://github.com/danielraffel/pulp/pull/4776))
- test(cli): widen create-shellout timeout for cold ephemeral CI VMs ([#4772](https://github.com/danielraffel/pulp/pull/4772))
- docs(examples): align PulpTone AU wording ([#4773](https://github.com/danielraffel/pulp/pull/4773))
- docs: align PulpGain format docs ([#4771](https://github.com/danielraffel/pulp/pull/4771))
- docs: align ui-preview example page ([#4770](https://github.com/danielraffel/pulp/pull/4770))

<a id="v04740"></a>
## [0.474.0] - 2026-06-25

- feat(host): execute Plugin nodes on the canonical executor routing path ([#4769](https://github.com/danielraffel/pulp/pull/4769))
- tools: add clean_build_cov.sh to reclaim orphaned coverage build dirs ([#4768](https://github.com/danielraffel/pulp/pull/4768))
- docs: align ViewBridge remote-view docs ([#4763](https://github.com/danielraffel/pulp/pull/4763))
- docs(examples): clarify plugin host demo format wording ([#4762](https://github.com/danielraffel/pulp/pull/4762))
- docs(ci): clarify PR example test coverage ([#4760](https://github.com/danielraffel/pulp/pull/4760))
- fix(examples): align TempoSampler package version metadata ([#4758](https://github.com/danielraffel/pulp/pull/4758))
- fix(examples): make stream demo complete cleanly ([#4755](https://github.com/danielraffel/pulp/pull/4755))

<a id="v04730"></a>
## [0.473.0] - 2026-06-25

- feat(graph): persistent-output node slots in the routing buffer assignment ([#4753](https://github.com/danielraffel/pulp/pull/4753))
- docs(sdf): correct example links ([#4754](https://github.com/danielraffel/pulp/pull/4754))
- docs(examples): clarify curated gallery scope ([#4752](https://github.com/danielraffel/pulp/pull/4752))

<a id="v04722"></a>
## [0.472.2] - 2026-06-24

- fix(cli): quote docs build-site args ([#4750](https://github.com/danielraffel/pulp/pull/4750))

<a id="v04721"></a>
## [0.472.1] - 2026-06-24

- test(cli): cover docs build-api command ([#4749](https://github.com/danielraffel/pulp/pull/4749))
- test(cli): cover docs check command ([#4745](https://github.com/danielraffel/pulp/pull/4745))
- docs: gate processing-model terminology in docs check ([#4744](https://github.com/danielraffel/pulp/pull/4744))

<a id="v04720"></a>
## [0.472.0] - 2026-06-25

- feat(host): opt-in SignalGraph::process routing through the canonical executor ([#4738](https://github.com/danielraffel/pulp/pull/4738))
- docs(processing-model): Processor vs SignalGraph guidance + terminology lint ([#4728](https://github.com/danielraffel/pulp/pull/4728))

<a id="v04710"></a>
## [0.471.0] - 2026-06-25

- feat(host): route eligible SignalGraphs through the canonical executor ([#4702](https://github.com/danielraffel/pulp/pull/4702))
- docs(audio): clarify WAV chunk-size clamp covers RF64/over-advertised sizes ([#4734](https://github.com/danielraffel/pulp/pull/4734))

<a id="v04702"></a>
## [0.470.2] - 2026-06-24

- fix(cli): detect write failures when rewriting version files ([#4733](https://github.com/danielraffel/pulp/pull/4733))
- test(view): guard Label font-gen reshape assertion to real-shaping builds ([#4731](https://github.com/danielraffel/pulp/pull/4731))
- fix(events): active_kind() honors untagged backends ([#4721](https://github.com/danielraffel/pulp/pull/4721))

<a id="v04701"></a>
## [0.470.1] - 2026-06-24

- fix: route ship signing checks through platform helpers ([#4723](https://github.com/danielraffel/pulp/pull/4723))

<a id="v04700"></a>
## [0.470.0] - 2026-06-24

- perf(view): cache Label soft-wrap shaped layout across paints ([#4693](https://github.com/danielraffel/pulp/pull/4693))
- test(graph): differential routing parity fuzzer + shared harness ([#4714](https://github.com/danielraffel/pulp/pull/4714))

<a id="v04691"></a>
## [0.469.1] - 2026-06-24

- fix(design-import): P8 package custom-controls were a no-op (wrong registry path) ([#4717](https://github.com/danielraffel/pulp/pull/4717))
- docs(ship): align notarization examples with packaged artifacts ([#4716](https://github.com/danielraffel/pulp/pull/4716))

<a id="v04690"></a>
## [0.469.0] - 2026-06-24

- perf(view): cache parsed SVG DOM across repaints (faithful-frame lane) ([#4701](https://github.com/danielraffel/pulp/pull/4701))
- fix(tooling): force fix/feat bump in version_bump_check --mode=apply on same-version base ([#4700](https://github.com/danielraffel/pulp/pull/4700))

<a id="v04680"></a>
## [0.468.0] - 2026-06-24

- feat(design-import): wire installed-package custom controls end-to-end (P8) ([#4707](https://github.com/danielraffel/pulp/pull/4707))
- test: cover MCP docs check execution ([#4703](https://github.com/danielraffel/pulp/pull/4703))

<a id="v04670"></a>
## [0.467.0] - 2026-06-24

- feat(design-import): extensible recognition — user component-key→kind manifest + merge module ([#4705](https://github.com/danielraffel/pulp/pull/4705))
- refactor(design-import): extract import-report + placement-verify passes out of design_ir_json.cpp ([#4691](https://github.com/danielraffel/pulp/pull/4691))

<a id="v04663"></a>
## [0.466.3] - 2026-06-24

- fix(inspect): keep unavailable MCP wrappers honest ([#4698](https://github.com/danielraffel/pulp/pull/4698))

<a id="v04662"></a>
## [0.466.2] - 2026-06-24

- fix(osc): fail closed on truncated/impossible typed arguments ([#4684](https://github.com/danielraffel/pulp/pull/4684))

<a id="v04661"></a>
## [0.466.1] - 2026-06-24

- perf(graph): reuse routing scratch slots via liveness allocation (Phase 4e) ([#4683](https://github.com/danielraffel/pulp/pull/4683))

<a id="v04660"></a>
## [0.466.0] - 2026-06-24

- feat(graph): one-block feedback routing in the canonical executor (Phase 4d) ([#4682](https://github.com/danielraffel/pulp/pull/4682))

<a id="v04650"></a>
## [0.465.0] - 2026-06-24

- refactor(graph): extract routing helpers + mode flag (code-health pass) ([#4681](https://github.com/danielraffel/pulp/pull/4681))

<a id="v04640"></a>
## [0.464.0] - 2026-06-24

- feat(graph): inter-node audio routing through the canonical executor (Phase 4a/4b) ([#4680](https://github.com/danielraffel/pulp/pull/4680))
- test(host): SignalGraph<->GraphRuntimeExecutor gain output-parity (Phase 4) ([#4675](https://github.com/danielraffel/pulp/pull/4675))
- feat(audio): extend RT-safety contract registry to the core runtime ([#4672](https://github.com/danielraffel/pulp/pull/4672))
- docs(cli): document doctor au cache flag ([#4671](https://github.com/danielraffel/pulp/pull/4671))
- tools(cli): classify shellout commands as slashless ([#4669](https://github.com/danielraffel/pulp/pull/4669))
- tools(cli): classify macos overflow as slashless ([#4668](https://github.com/danielraffel/pulp/pull/4668))
- docs(cli): remove stale pulp new references ([#4666](https://github.com/danielraffel/pulp/pull/4666))
- feat(host): per-node CPU-load telemetry via SignalGraph::node_loads() ([#4662](https://github.com/danielraffel/pulp/pull/4662))

<a id="v04639"></a>
## [0.463.9] - 2026-06-23

- fix: reject extra rust cache args ([#4658](https://github.com/danielraffel/pulp/pull/4658))
- fix(host): pre-reserve CLAP slot scratch so process() doesn't allocate ([#4657](https://github.com/danielraffel/pulp/pull/4657))

<a id="v04638"></a>
## [0.463.8] - 2026-06-23

- fix: correct scan and host CLI docs ([#4653](https://github.com/danielraffel/pulp/pull/4653))
- ci(coverage): build PulpGain fixtures for hosted-slot diff coverage ([#4652](https://github.com/danielraffel/pulp/pull/4652))
- docs(test): refresh source regression comments ([#4650](https://github.com/danielraffel/pulp/pull/4650))

<a id="v04637"></a>
## [0.463.7] - 2026-06-23

- fix(cli): include fmt in Rust help ([#4649](https://github.com/danielraffel/pulp/pull/4649))
- docs(tools): refresh source comment provenance ([#4648](https://github.com/danielraffel/pulp/pull/4648))

<a id="v04636"></a>
## [0.463.6] - 2026-06-23

- fix(cli): install pulp-mcp during upgrade ([#4645](https://github.com/danielraffel/pulp/pull/4645))
- docs(test): refresh source regression comment labels ([#4643](https://github.com/danielraffel/pulp/pull/4643))

<a id="v04635"></a>
## [0.463.5] - 2026-06-23

- fix(cli): honor rust pr workflow selection ([#4642](https://github.com/danielraffel/pulp/pull/4642))
- docs(test): refresh platform regression comments ([#4641](https://github.com/danielraffel/pulp/pull/4641))
- docs(test): refresh regression comment provenance ([#4640](https://github.com/danielraffel/pulp/pull/4640))

<a id="v04634"></a>
## [0.463.4] - 2026-06-23

- fix(cli): expose tool info in rust frontend ([#4638](https://github.com/danielraffel/pulp/pull/4638))
- docs(ci): refresh workflow comment provenance ([#4639](https://github.com/danielraffel/pulp/pull/4639))
- docs(tools): refresh regression comment provenance ([#4636](https://github.com/danielraffel/pulp/pull/4636))
- test(state): flood-test ParameterEventQueue sort() worst case + document it ([#4619](https://github.com/danielraffel/pulp/pull/4619))
- docs(inspect): refresh inspector regression comment anchors ([#4635](https://github.com/danielraffel/pulp/pull/4635))
- docs(cli): list docs build subcommands ([#4634](https://github.com/danielraffel/pulp/pull/4634))
- docs(audio): refresh regression test comment anchors ([#4633](https://github.com/danielraffel/pulp/pull/4633))
- test(cli): cover clean command success path ([#4629](https://github.com/danielraffel/pulp/pull/4629))
- docs(view): refresh view test comment wording ([#4632](https://github.com/danielraffel/pulp/pull/4632))
- docs(webcompat): refresh svg bridge test comments ([#4628](https://github.com/danielraffel/pulp/pull/4628))
- feat(signal): add ScopedFlushDenormals and flush denormals in standalone host ([#4604](https://github.com/danielraffel/pulp/pull/4604))
- docs(font): refresh text rendering test comments ([#4626](https://github.com/danielraffel/pulp/pull/4626))
- docs(cli): align version command docs ([#4620](https://github.com/danielraffel/pulp/pull/4620))
- docs(view): refresh canvas test comment wording ([#4625](https://github.com/danielraffel/pulp/pull/4625))
- fix(design-import): adversarial-review follow-ups (P5 install + nm-guard, P8 schema↔TS, P2 vocab) ([#4624](https://github.com/danielraffel/pulp/pull/4624))

<a id="v04633"></a>
## [0.463.3] - 2026-06-23

- fix(state): write finite param values in presets (NaN → param default) ([#4622](https://github.com/danielraffel/pulp/pull/4622))
- docs(import): trim design codegen comment breadcrumbs ([#4618](https://github.com/danielraffel/pulp/pull/4618))
- docs(audio): clarify model activation requires install ([#4616](https://github.com/danielraffel/pulp/pull/4616))
- perf(view): extend the z-order fast path to hit_test (no per-event alloc) ([#4613](https://github.com/danielraffel/pulp/pull/4613))
- docs(audio): clarify validate manifest scope ([#4615](https://github.com/danielraffel/pulp/pull/4615))
- docs(import): trim test comment provenance breadcrumbs ([#4612](https://github.com/danielraffel/pulp/pull/4612))
- docs(audio): clarify excerpt-find backend status ([#4614](https://github.com/danielraffel/pulp/pull/4614))

<a id="v04632"></a>
## [0.463.2] - 2026-06-23

- fix(cli): restore rust status workflow parity ([#4607](https://github.com/danielraffel/pulp/pull/4607))
- docs(cli): trim CLI test provenance comments ([#4611](https://github.com/danielraffel/pulp/pull/4611))
- feature/source comment hygiene batch 20260623 cd ([#4610](https://github.com/danielraffel/pulp/pull/4610))
- feature/source comment hygiene batch 20260623 cc ([#4609](https://github.com/danielraffel/pulp/pull/4609))
- docs(cli): align validate command surface ([#4599](https://github.com/danielraffel/pulp/pull/4599))
- fix/ios auv3 example bundle id ([#4603](https://github.com/danielraffel/pulp/pull/4603))

<a id="v04631"></a>
## [0.463.1] - 2026-06-23

- fix(midi): flush/close before reporting write_midi_file success ([#4602](https://github.com/danielraffel/pulp/pull/4602))

<a id="v04630"></a>
## [0.463.0] - 2026-06-23

- fix(audio): make AudioFormatReaderSource compile + allocation-free, with tests ([#4598](https://github.com/danielraffel/pulp/pull/4598))
- docs(cli): route test slash command through pulp ([#4597](https://github.com/danielraffel/pulp/pull/4597))
- docs(status): align accessibility feature evidence ([#4595](https://github.com/danielraffel/pulp/pull/4595))
- feature/source comment hygiene batch 20260623 cb ([#4594](https://github.com/danielraffel/pulp/pull/4594))
- feat(ios): enforce AUv3 .appex bundle-id containment at configure time ([#4592](https://github.com/danielraffel/pulp/pull/4592))
- docs(examples): align overview guide feature bullets ([#4591](https://github.com/danielraffel/pulp/pull/4591))
- docs(examples): clarify pending platform validation ([#4590](https://github.com/danielraffel/pulp/pull/4590))
- feature/source comment hygiene batch 20260623 ca ([#4589](https://github.com/danielraffel/pulp/pull/4589))
- docs(plugin): align Claude install path with public repo ([#4586](https://github.com/danielraffel/pulp/pull/4586))
- feature/source comment hygiene batch 20260623 bz ([#4584](https://github.com/danielraffel/pulp/pull/4584))

<a id="v04621"></a>
## [0.462.1] - 2026-06-23

- fix(cli): route build install through Rust fallthrough ([#4579](https://github.com/danielraffel/pulp/pull/4579))
- test(rt-safety): cover SignalGraph::process() under the no-alloc trap ([#4583](https://github.com/danielraffel/pulp/pull/4583))
- docs(ci): trim workflow comment breadcrumbs ([#4582](https://github.com/danielraffel/pulp/pull/4582))
- chore: drop private project-importer consumers from public downstream manifests ([#4095](https://github.com/danielraffel/pulp/pull/4095))

<a id="v04620"></a>
## [0.462.0] - 2026-06-23

- feat(view): standalone outbound file drag — Windows (OLE) + SDL native handles ([#4092](https://github.com/danielraffel/pulp/pull/4092))
- test(midi): pin realtime SysEx pool recycling (no silent drop across blocks) ([#4581](https://github.com/danielraffel/pulp/pull/4581))
- docs(ci): trim workflow provenance comments ([#4580](https://github.com/danielraffel/pulp/pull/4580))
- docs(tools,test): trim stale provenance comments ([#4578](https://github.com/danielraffel/pulp/pull/4578))
- docs(run): align slash screenshot example with CLI ([#4577](https://github.com/danielraffel/pulp/pull/4577))
- docs(cli): trim extraction provenance comments ([#4576](https://github.com/danielraffel/pulp/pull/4576))
- docs(create): align scaffold wording with CLI ([#4573](https://github.com/danielraffel/pulp/pull/4573))
- docs(local-ci): trim extraction provenance comments ([#4571](https://github.com/danielraffel/pulp/pull/4571))
- docs(render): align GPU validation checklist ([#4572](https://github.com/danielraffel/pulp/pull/4572))
- docs(ship): align command inventory ([#4570](https://github.com/danielraffel/pulp/pull/4570))
- docs(render): align surface status with implementation ([#4569](https://github.com/danielraffel/pulp/pull/4569))
- docs(view): trim source roadmap comments ([#4566](https://github.com/danielraffel/pulp/pull/4566))

<a id="v04610"></a>
## [0.461.0] - 2026-06-23

- perf(format): reuse reserved MIDI buffers in VST3 process() (no per-block alloc) ([#4564](https://github.com/danielraffel/pulp/pull/4564))
- docs(signal): align convolver module example ([#4567](https://github.com/danielraffel/pulp/pull/4567))
- docs(events): align module reference with current APIs ([#4565](https://github.com/danielraffel/pulp/pull/4565))
- docs(runtime): align module examples with API ([#4563](https://github.com/danielraffel/pulp/pull/4563))
- docs(runtime): trim source provenance comments ([#4561](https://github.com/danielraffel/pulp/pull/4561))
- docs(reference): align state examples with public API ([#4562](https://github.com/danielraffel/pulp/pull/4562))
- docs(canvas): align examples with public API ([#4560](https://github.com/danielraffel/pulp/pull/4560))
- docs(osc): align examples with public API ([#4559](https://github.com/danielraffel/pulp/pull/4559))
- docs(view): correct plugin host capability matrix ([#4556](https://github.com/danielraffel/pulp/pull/4556))
- docs(view): correct style props theme bridge signatures ([#4555](https://github.com/danielraffel/pulp/pull/4555))
- docs(view): clarify JS bridge reference scope ([#4554](https://github.com/danielraffel/pulp/pull/4554))
- docs: update hosting loader status ([#4551](https://github.com/danielraffel/pulp/pull/4551))
- docs: fix stale sync strategy references ([#4549](https://github.com/danielraffel/pulp/pull/4549))
- docs: fix versioning guide CI link ([#4547](https://github.com/danielraffel/pulp/pull/4547))
- docs: fix widget support matrix evidence ([#4545](https://github.com/danielraffel/pulp/pull/4545))
- docs(view): clarify CodeEditor native scope ([#4543](https://github.com/danielraffel/pulp/pull/4543))

<a id="v04600"></a>
## [0.460.0] - 2026-06-23

- perf(view): skip per-frame z-sort allocation when children already in z-order ([#4541](https://github.com/danielraffel/pulp/pull/4541))
- docs(view): trim workflow provenance comments ([#4538](https://github.com/danielraffel/pulp/pull/4538))
- docs(format): clarify CLAP GUI limitation ([#4540](https://github.com/danielraffel/pulp/pull/4540))
- docs: clarify scripted ui hot reload scope ([#4539](https://github.com/danielraffel/pulp/pull/4539))
- tools(mcp): clarify inspector audio description ([#4537](https://github.com/danielraffel/pulp/pull/4537))
- docs: trim source provenance comments ([#4536](https://github.com/danielraffel/pulp/pull/4536))

<a id="v04597"></a>
## [0.459.7] - 2026-06-23

- docs(import): clarify design import surfaces ([#4535](https://github.com/danielraffel/pulp/pull/4535))
- docs(test): trim regression provenance comments ([#4534](https://github.com/danielraffel/pulp/pull/4534))
- docs(packages): clarify remove cleanup ownership ([#4533](https://github.com/danielraffel/pulp/pull/4533))
- docs(test): trim WidgetBridge provenance comments ([#4532](https://github.com/danielraffel/pulp/pull/4532))
- docs(packages): align search and suggest options ([#4531](https://github.com/danielraffel/pulp/pull/4531))
- feature/source comment hygiene batch 20260623 bj ([#4529](https://github.com/danielraffel/pulp/pull/4529))
- docs(format): align AUv3 sidechain status ([#4530](https://github.com/danielraffel/pulp/pull/4530))
- docs(readme): align MIDI backend limitations ([#4528](https://github.com/danielraffel/pulp/pull/4528))
- docs(examples): trim demo provenance comments ([#4526](https://github.com/danielraffel/pulp/pull/4526))
- docs: align ALSA MIDI status limitation ([#4527](https://github.com/danielraffel/pulp/pull/4527))
- docs: align format limitation status ([#4525](https://github.com/danielraffel/pulp/pull/4525))
- docs(examples): align PulpSynth format docs ([#4523](https://github.com/danielraffel/pulp/pull/4523))
- docs(readme): clarify audio file codec support ([#4522](https://github.com/danielraffel/pulp/pull/4522))
- docs(view): trim web-compat provenance comments ([#4521](https://github.com/danielraffel/pulp/pull/4521))
- docs(formats): align adapter capability guide ([#4520](https://github.com/danielraffel/pulp/pull/4520))
- docs(packages): clarify commercial license override ([#4518](https://github.com/danielraffel/pulp/pull/4518))
- feature/source comment hygiene batch 20260623 bg ([#4516](https://github.com/danielraffel/pulp/pull/4516))
- test(examples): add direct PulpPluck coverage ([#4494](https://github.com/danielraffel/pulp/pull/4494))
- docs(packages): refresh package manager status ([#4504](https://github.com/danielraffel/pulp/pull/4504))
- test(state): keep preset escape test portable ([#4508](https://github.com/danielraffel/pulp/pull/4508))
- fix(view): decode screenshot comparisons with Skia ([#4514](https://github.com/danielraffel/pulp/pull/4514))

<a id="v04596"></a>
## [0.459.6] - 2026-06-22

- docs(view): trim lock-to-source provenance comments ([#4515](https://github.com/danielraffel/pulp/pull/4515))
- docs(ci): trim stale runner provenance comments ([#4512](https://github.com/danielraffel/pulp/pull/4512))
- test(view): trim stale provenance comments ([#4511](https://github.com/danielraffel/pulp/pull/4511))
- chore(harness): trim stale provenance labels ([#4510](https://github.com/danielraffel/pulp/pull/4510))
- docs(ci): trim workflow provenance comments ([#4509](https://github.com/danielraffel/pulp/pull/4509))
- docs(tools): trim stale provenance comment labels ([#4507](https://github.com/danielraffel/pulp/pull/4507))
- docs(view): trim source provenance comments ([#4506](https://github.com/danielraffel/pulp/pull/4506))
- docs(tokens): fix export-token guide commands ([#4502](https://github.com/danielraffel/pulp/pull/4502))
- fix(state): write content index atomically (temp + rename) ([#4498](https://github.com/danielraffel/pulp/pull/4498))
- docs(templates): trim create scaffold provenance comments ([#4499](https://github.com/danielraffel/pulp/pull/4499))
- docs(tooling): refresh guard script comments ([#4495](https://github.com/danielraffel/pulp/pull/4495))

<a id="v04595"></a>
## [0.459.5] - 2026-06-22

- fix(state): make PresetManager save atomic and JSON-safe ([#4493](https://github.com/danielraffel/pulp/pull/4493))
- docs(web): correct experimental plugin target docs ([#4492](https://github.com/danielraffel/pulp/pull/4492))

<a id="v04594"></a>
## [0.459.4] - 2026-06-22

- fix(state): make PropertiesFile save atomic (temp + rename) ([#4489](https://github.com/danielraffel/pulp/pull/4489))
- docs(test): refresh view harness comments ([#4490](https://github.com/danielraffel/pulp/pull/4490))
- docs(cmake): document LV2 and AUv3 plugin targets ([#4491](https://github.com/danielraffel/pulp/pull/4491))
- docs: use Release in source build snippets ([#4488](https://github.com/danielraffel/pulp/pull/4488))
- docs(test): refresh misc regression comments ([#4486](https://github.com/danielraffel/pulp/pull/4486))
- docs(cli): document coverage command ([#4485](https://github.com/danielraffel/pulp/pull/4485))
- docs(cli): remove duplicate config manifest entry ([#4483](https://github.com/danielraffel/pulp/pull/4483))
- fix(ci): coordinate the two release workflows to publish a complete release ([#4481](https://github.com/danielraffel/pulp/pull/4481))
- ci: add release-health escalation watchdog (Layer 4) ([#4431](https://github.com/danielraffel/pulp/pull/4431))
- docs(format,cli): refresh host quirk and CLI comments ([#4484](https://github.com/danielraffel/pulp/pull/4484))
- docs(test): refresh motion inspector comments ([#4482](https://github.com/danielraffel/pulp/pull/4482))
- docs(audio): refresh observability comments ([#4480](https://github.com/danielraffel/pulp/pull/4480))
- docs(test): refresh legacy workstream comments ([#4479](https://github.com/danielraffel/pulp/pull/4479))
- docs(font): refresh rendering test comments ([#4478](https://github.com/danielraffel/pulp/pull/4478))
- docs(test): refresh CMake test comments ([#4477](https://github.com/danielraffel/pulp/pull/4477))
- docs(font): refresh text test comments ([#4476](https://github.com/danielraffel/pulp/pull/4476))
- fix(harness): read split Canvas2D bridge surface ([#4475](https://github.com/danielraffel/pulp/pull/4475))
- docs(canvas): refresh shim comment status ([#4474](https://github.com/danielraffel/pulp/pull/4474))
- docs(view): clean bridge provenance comments ([#4473](https://github.com/danielraffel/pulp/pull/4473))
- docs(source): clean remaining provenance comment labels ([#4472](https://github.com/danielraffel/pulp/pull/4472))
- docs(native): clean Rust fixture provenance comments ([#4471](https://github.com/danielraffel/pulp/pull/4471))
- docs(signal,rust): clean offline stretch comment breadcrumbs ([#4470](https://github.com/danielraffel/pulp/pull/4470))
- docs(widget-bridge): clean web-compat provenance comments ([#4469](https://github.com/danielraffel/pulp/pull/4469))
- docs(inspect): clean inspector provenance comments ([#4468](https://github.com/danielraffel/pulp/pull/4468))
- docs(import): clean design import provenance comments ([#4467](https://github.com/danielraffel/pulp/pull/4467))
- docs(view): clean observability comment breadcrumbs ([#4466](https://github.com/danielraffel/pulp/pull/4466))
- docs(tools): clean version gate provenance comments ([#4465](https://github.com/danielraffel/pulp/pull/4465))
- docs(test): clean extracted test comment breadcrumbs ([#4464](https://github.com/danielraffel/pulp/pull/4464))

<a id="v04593"></a>
## [0.459.3] - 2026-06-22

- docs(source): clean format cli and figma comment breadcrumbs ([#4463](https://github.com/danielraffel/pulp/pull/4463))
- docs(source): clean runtime and view comment breadcrumbs ([#4462](https://github.com/danielraffel/pulp/pull/4462))
- docs(source): clean model and event comment breadcrumbs ([#4461](https://github.com/danielraffel/pulp/pull/4461))
- docs(source): clean Priority B source comment breadcrumbs ([#4460](https://github.com/danielraffel/pulp/pull/4460))
- docs(source): clean iOS AUv3 example comment breadcrumbs ([#4459](https://github.com/danielraffel/pulp/pull/4459))
- docs(source): clean view host comment breadcrumbs ([#4458](https://github.com/danielraffel/pulp/pull/4458))
- docs(source): clean test harness comment breadcrumbs ([#4457](https://github.com/danielraffel/pulp/pull/4457))
- docs(source): clean tooling comment breadcrumbs ([#4456](https://github.com/danielraffel/pulp/pull/4456))
- docs(source): clean core provenance comments ([#4455](https://github.com/danielraffel/pulp/pull/4455))
- docs(source): clean view comment breadcrumbs ([#4454](https://github.com/danielraffel/pulp/pull/4454))
- docs(cli): clean Rust CLI comment breadcrumbs ([#4453](https://github.com/danielraffel/pulp/pull/4453))
- docs(source): clean view and cli comment breadcrumbs ([#4452](https://github.com/danielraffel/pulp/pull/4452))
- docs(source): clean view and midi comment breadcrumbs ([#4451](https://github.com/danielraffel/pulp/pull/4451))
- docs(source): clean core and tooling comment breadcrumbs ([#4450](https://github.com/danielraffel/pulp/pull/4450))
- feature/source comment hygiene batch 20260622 n ([#4449](https://github.com/danielraffel/pulp/pull/4449))
- feature/source comment hygiene batch 20260622 m ([#4448](https://github.com/danielraffel/pulp/pull/4448))
- docs(source): clean figma plugin comment breadcrumbs ([#4447](https://github.com/danielraffel/pulp/pull/4447))
- docs(source): clean web-compat JS comment breadcrumbs ([#4446](https://github.com/danielraffel/pulp/pull/4446))
- docs(source): clean layout geometry comment breadcrumbs ([#4445](https://github.com/danielraffel/pulp/pull/4445))
- docs(source): clean widget bridge comment breadcrumbs ([#4444](https://github.com/danielraffel/pulp/pull/4444))
- docs(source): clean core view comment breadcrumbs ([#4443](https://github.com/danielraffel/pulp/pull/4443))
- docs(source): clean inspect comment breadcrumbs ([#4442](https://github.com/danielraffel/pulp/pull/4442))
- chore(source): batch comment hygiene and benchmark cleanup ([#4441](https://github.com/danielraffel/pulp/pull/4441))
- docs(canvas): clean text font comment breadcrumbs ([#4440](https://github.com/danielraffel/pulp/pull/4440))
- docs(import-ir): clean stale comments and fix HostBench test path ([#4439](https://github.com/danielraffel/pulp/pull/4439))
- docs(react): clean stale comment breadcrumbs ([#4438](https://github.com/danielraffel/pulp/pull/4438))
- docs(view): clean web-compat JS comment breadcrumbs ([#4437](https://github.com/danielraffel/pulp/pull/4437))
- docs(format): clean audio and MIDI comment breadcrumbs ([#4436](https://github.com/danielraffel/pulp/pull/4436))
- ci: fix codecov coverage drift and gate it so it screams on regress ([#4435](https://github.com/danielraffel/pulp/pull/4435))
- docs(render): remove stale headless surface plan breadcrumb ([#4434](https://github.com/danielraffel/pulp/pull/4434))
- docs(render): clean canvas/render comment breadcrumbs ([#4433](https://github.com/danielraffel/pulp/pull/4433))
- docs(view): clean widget/codegen comment breadcrumbs ([#4430](https://github.com/danielraffel/pulp/pull/4430))

<a id="v04592"></a>
## [0.459.2] - 2026-06-21

- docs(skill): clean import-design comment breadcrumbs ([#4429](https://github.com/danielraffel/pulp/pull/4429))
- fix(ci): route release signing to macos-15 ([#4427](https://github.com/danielraffel/pulp/pull/4427))
- fix/release unblock 20260620 ([#4426](https://github.com/danielraffel/pulp/pull/4426))
- feature/comment hygiene batch open 20260620 ([#4425](https://github.com/danielraffel/pulp/pull/4425))
- docs(ci): prefer Shipyard for GitHub work; gh 'invalid token' is usually a rate limit ([#4097](https://github.com/danielraffel/pulp/pull/4097))

<a id="v04591"></a>
## [0.459.1] - 2026-06-20

- fix(view): repaint secondary GPU windows + fill MusicalTypingKeyboard pad highlight ([#4375](https://github.com/danielraffel/pulp/pull/4375))
- docs: clean canvas comment archaeology ([#4242](https://github.com/danielraffel/pulp/pull/4242))
- docs: clean android shell comment archaeology ([#4249](https://github.com/danielraffel/pulp/pull/4249))
- docs: clean android motion comment archaeology ([#4250](https://github.com/danielraffel/pulp/pull/4250))
- docs: clean android audio comment archaeology ([#4248](https://github.com/danielraffel/pulp/pull/4248))
- docs: clean apple audio comment archaeology ([#4247](https://github.com/danielraffel/pulp/pull/4247))
- build(view): PULP_ENABLE_JS — native-only build hygiene (no JS engine when off) ([#4236](https://github.com/danielraffel/pulp/pull/4236))
- feat(packages): P8 — custom-control package category for design import ([#4240](https://github.com/danielraffel/pulp/pull/4240))
- refactor(import-design): P2 — decompose walk() into node_tree_to_ir (no globals) ([#4237](https://github.com/danielraffel/pulp/pull/4237))
- docs: clean compat reference archaeology ([#4238](https://github.com/danielraffel/pulp/pull/4238))

<a id="v04590"></a>
## [0.459.0] - 2026-06-19

- feat(design-import): faithful-import kinds end-to-end (P0–P7) — fader/toggle/swap/action/xy_pad/value_label/custom + import-report + placement-verify ([#4235](https://github.com/danielraffel/pulp/pull/4235))
- docs(ara): remove workflow archaeology ([#4231](https://github.com/danielraffel/pulp/pull/4231))

<a id="v04580"></a>
## [0.458.0] - 2026-06-19

- docs(stretch): agent skill for the stretch engine, modes, presets, eval ([#4219](https://github.com/danielraffel/pulp/pull/4219))
- docs(auv3): remove workflow archaeology ([#4229](https://github.com/danielraffel/pulp/pull/4229))
- docs(auv2): remove workflow archaeology ([#4228](https://github.com/danielraffel/pulp/pull/4228))
- ci: guard inspect hotspots ([#4227](https://github.com/danielraffel/pulp/pull/4227))

<a id="v04570"></a>
## [0.457.0] - 2026-06-19

- feat(ship): non-interactive signing+notarization via `pulp ship doctor` ([#4223](https://github.com/danielraffel/pulp/pull/4223))
- docs(inspect): map module boundaries ([#4226](https://github.com/danielraffel/pulp/pull/4226))
- docs(import-design): map tool boundaries ([#4222](https://github.com/danielraffel/pulp/pull/4222))
- docs(cli): map content command boundaries ([#4221](https://github.com/danielraffel/pulp/pull/4221))
- docs(cli): map command surface boundaries ([#4220](https://github.com/danielraffel/pulp/pull/4220))

<a id="v04562"></a>
## [0.456.2] - 2026-06-19

- refactor(render): extract renderer3d internal types ([#4215](https://github.com/danielraffel/pulp/pull/4215))
- chore(examples): make Ink & Signal showcase window shrinkable, both-axis scroll ([#4217](https://github.com/danielraffel/pulp/pull/4217))
- docs(render): map renderer3d split boundaries ([#4214](https://github.com/danielraffel/pulp/pull/4214))

<a id="v04561"></a>
## [0.456.1] - 2026-06-19

- docs(cli): map kit command split boundaries ([#4207](https://github.com/danielraffel/pulp/pull/4207))

<a id="v04560"></a>
## [0.456.0] - 2026-06-19

- fix(tempo-sampler): defer command-line loop load to avoid null-StateStore crash ([#4191](https://github.com/danielraffel/pulp/pull/4191))

<a id="v04550"></a>
## [0.455.0] - 2026-06-19

- feat(offline-stretch): material-adaptive window + STN-off default ([#4178](https://github.com/danielraffel/pulp/pull/4178))
- ci: enforce hotspot shrink ratchet ([#4206](https://github.com/danielraffel/pulp/pull/4206))

<a id="v04540"></a>
## [0.454.0] - 2026-06-18

- feat(view): add opt-in Ink & Signal navigation variants (#69) ([#4205](https://github.com/danielraffel/pulp/pull/4205))
- docs: add current maintainability roadmap plan ([#4204](https://github.com/danielraffel/pulp/pull/4204))
- feat(inspect): Wiring tab annotate loop — click → note → Reveal/Export ([#4200](https://github.com/danielraffel/pulp/pull/4200))
- chore: raise hotspot ceilings for the Wiring-tab change ([#4196](https://github.com/danielraffel/pulp/pull/4196))
- docs: clarify opencv-python license (OpenCV Apache-2.0; packaging MIT) ([#4198](https://github.com/danielraffel/pulp/pull/4198))
- docs(ci): codify slow-PR diagnosis (it is almost never runner saturation) ([#4194](https://github.com/danielraffel/pulp/pull/4194))

<a id="v04530"></a>
## [0.453.0] - 2026-06-18

- feat(ship): portability guard + render-before-show for shareable standalone apps ([#4193](https://github.com/danielraffel/pulp/pull/4193))

<a id="v04520"></a>
## [0.452.0] - 2026-06-18

- feat(view): press-flash command-button mode for toggles ([#4190](https://github.com/danielraffel/pulp/pull/4190))

<a id="v04510"></a>
## [0.451.0] - 2026-06-18

- docs(ci skill): note source-hotspot ceiling-bump workflow ([#4181](https://github.com/danielraffel/pulp/pull/4181))

<a id="v04500"></a>
## [0.450.0] - 2026-06-18

- feat(view): sliding switch dot + xy_pad 2D puck for DesignFrameView ([#4180](https://github.com/danielraffel/pulp/pull/4180))
- build: auto-provision Skia into a shared machine cache ([#4179](https://github.com/danielraffel/pulp/pull/4179))

<a id="v04490"></a>
## [0.449.0] - 2026-06-18

- feat(view): horizontal fader orientation in DesignFrameView ([#4177](https://github.com/danielraffel/pulp/pull/4177))
- feat(examples): add Bendr — pitch/time/freeze effect from pulp::signal primitives ([#4174](https://github.com/danielraffel/pulp/pull/4174))
- fix(view): extend MTK typing octave reach to +5 (highlight nears G8) ([#4172](https://github.com/danielraffel/pulp/pull/4172))

<a id="v04480"></a>
## [0.448.0] - 2026-06-17

- feat(view): faithful-vector fader + toggle kinds; rotate rect needles ([#4167](https://github.com/danielraffel/pulp/pull/4167))
- docs: drop external blog-post citations from source comments ([#4173](https://github.com/danielraffel/pulp/pull/4173))

<a id="v04470"></a>
## [0.447.0] - 2026-06-18

- feat(view): MTK piano windowed range + control-highlight & readout polish ([#4166](https://github.com/danielraffel/pulp/pull/4166))
- fix(import-design): retry Figma REST calls on 429 / 5xx with backoff ([#4159](https://github.com/danielraffel/pulp/pull/4159))

<a id="v04461"></a>
## [0.446.1] - 2026-06-17

- fix(import-design): bound keyboard-shortcut regex (catastrophic backtracking) ([#4164](https://github.com/danielraffel/pulp/pull/4164))

<a id="v04460"></a>
## [0.446.0] - 2026-06-18

- fix(view): MTK number-row keys + tap-flash on octave/velocity/arrow buttons ([#4158](https://github.com/danielraffel/pulp/pull/4158))

<a id="v04450"></a>
## [0.445.0] - 2026-06-18

- feat(view): live MTK readouts, c/v velocity keys, focused demo ([#4149](https://github.com/danielraffel/pulp/pull/4149))
- docs(view): remove framework-name mentions from hosted-view comments ([#4157](https://github.com/danielraffel/pulp/pull/4157))
- docs: inventory optional developer-only tooling + web-platform standards ([#4156](https://github.com/danielraffel/pulp/pull/4156))

<a id="v04442"></a>
## [0.444.2] - 2026-06-17

- fix(view): harden hosted-editor key forward (resign fallback, scoped blur, event-keyed guard) ([#4153](https://github.com/danielraffel/pulp/pull/4153))

<a id="v04441"></a>
## [0.444.1] - 2026-06-17

- fix(view): forward unconsumed keys from the AU editor to the DAW host ([#4136](https://github.com/danielraffel/pulp/pull/4136))

<a id="v04440"></a>
## [0.444.0] - 2026-06-17

- feat(view): wire the MusicalTypingKeyboard on-screen controls ([#4146](https://github.com/danielraffel/pulp/pull/4146))
- fix(examples): component-shots creates its --out directory ([#4142](https://github.com/danielraffel/pulp/pull/4142))

<a id="v04430"></a>
## [0.443.0] - 2026-06-17

- fix(audio,canvas): post-review hardening + test coverage ([#4133](https://github.com/danielraffel/pulp/pull/4133))

<a id="v04420"></a>
## [0.442.0] - 2026-06-17

- fix(view): resolve adversarial-review findings on the MTK toggle ([#4135](https://github.com/danielraffel/pulp/pull/4135))
- ci(sanitizer): set TARTCI_GH_CLI=ghapp on the parked sanitizer lane template ([#4131](https://github.com/danielraffel/pulp/pull/4131))
- ci(coverage): make the best-effort budget kill unconditional (was &&-gated) ([#4130](https://github.com/danielraffel/pulp/pull/4130))

<a id="v04411"></a>
## [0.441.1] - 2026-06-17

- fix(view): hosted editor focus affordance + keyboard hand-back ([#4127](https://github.com/danielraffel/pulp/pull/4127))
- ci(coverage): make os-windows coverage best-effort (deterministic) — unblocks #3947 ([#4088](https://github.com/danielraffel/pulp/pull/4088))

<a id="v04410"></a>
## [0.441.0] - 2026-06-17

- feat(view): piano⇄typing toggle via DesignFrameView multi-frame swap ([#4125](https://github.com/danielraffel/pulp/pull/4125))

<a id="v04401"></a>
## [0.440.1] - 2026-06-17

- fix(view): hosted (AU/DAW) text fields route keys like the standalone ([#4122](https://github.com/danielraffel/pulp/pull/4122))
- feat(local-ci): review-first video-proof cards in desktop publish report ([#4102](https://github.com/danielraffel/pulp/pull/4102))
- feat(view): Ink & Signal showcase + faithful-playable Musical Typing Keyboard ([#4076](https://github.com/danielraffel/pulp/pull/4076))

<a id="v04400"></a>
## [0.440.0] - 2026-06-17

- feat(view,canvas,build): shadow cache, opt-in Lottie, authoring-strip + design-import gating ([#4079](https://github.com/danielraffel/pulp/pull/4079))

<a id="v04390"></a>
## [0.439.0] - 2026-06-16

- feat(au): emit parameter automation to host from plugin editor edits ([#4110](https://github.com/danielraffel/pulp/pull/4110))
- feat(audio): true ranged decode in MemoryMappedAudioReader ([#4080](https://github.com/danielraffel/pulp/pull/4080))
- feat(view): Linux standalone outbound file drag (XDND source) ([#4106](https://github.com/danielraffel/pulp/pull/4106))
- ci(sanitizers): route TSan pilot to a gate-safe local VM lane (#4101) ([#4108](https://github.com/danielraffel/pulp/pull/4108))

<a id="v04380"></a>
## [0.438.0] - 2026-06-17

- feat(view): iOS native drag-and-drop (in + out) ([#4085](https://github.com/danielraffel/pulp/pull/4085))

<a id="v04371"></a>
## [0.437.1] - 2026-06-17

- fix(view): release XdndSelection when an XDND outbound drag ends ([#4083](https://github.com/danielraffel/pulp/pull/4083))
- feat(audio): streaming sample source + multi-mic zone selection ([#4077](https://github.com/danielraffel/pulp/pull/4077))

<a id="v04370"></a>
## [0.437.0] - 2026-06-16

- feat(view): Android native file drop (inbound) ([#4094](https://github.com/danielraffel/pulp/pull/4094))
- fix(local-ci): emit review.md + review-package.json from desktop publish ([#4089](https://github.com/danielraffel/pulp/pull/4089))

<a id="v04360"></a>
## [0.436.0] - 2026-06-16

- fix/model download connect timeout ([#4090](https://github.com/danielraffel/pulp/pull/4090))
- ci(coverage): fix launchd template TCC shape + invert label gate ([#4087](https://github.com/danielraffel/pulp/pull/4087))
- feat(signal): phase-vocoder + click-free soft-bypass DSP primitives ([#4078](https://github.com/danielraffel/pulp/pull/4078))

<a id="v04350"></a>
## [0.435.0] - 2026-06-16

- feat/video proof reland ([#4084](https://github.com/danielraffel/pulp/pull/4084))

<a id="v04340"></a>
## [0.434.0] - 2026-06-16

- feat(view): native file drag-out source on Windows (OLE) + Linux (XDND) ([#4073](https://github.com/danielraffel/pulp/pull/4073))
- Sampler: LOOP toggle, GPU-host editor, sensitivity-mapping fix ([#4074](https://github.com/danielraffel/pulp/pull/4074))
- ci(coverage): drop -object entries that vanish after the pre-flight probe ([#4075](https://github.com/danielraffel/pulp/pull/4075))
- ci(coverage): pass llvm-cov objects via response file (fixes Windows ARG_MAX) ([#4070](https://github.com/danielraffel/pulp/pull/4070))
- Sampler: interactive ⌘K keyboard, Open button, AU MIDI + slice-mapping fixes ([#4067](https://github.com/danielraffel/pulp/pull/4067))

<a id="v04330"></a>
## [0.433.0] - 2026-06-15

- feature/view outbound file drag ([#4068](https://github.com/danielraffel/pulp/pull/4068))
- ci(coverage): harden coverage VM lane + screenshot helper (post-merge review) ([#4066](https://github.com/danielraffel/pulp/pull/4066))
- Sampler: waveform zoom + pan (scroll/pinch) ([#4059](https://github.com/danielraffel/pulp/pull/4059))
- ci: add dedicated macOS coverage VM lane ([#4036](https://github.com/danielraffel/pulp/pull/4036))
- docs(validation): autonomous REAPER HostBench re-validation on current main ([#4063](https://github.com/danielraffel/pulp/pull/4063))

<a id="v04321"></a>
## [0.432.1] - 2026-06-14

- fix(cli): make kit-verify --execute-screenshots pass on Windows ([#4061](https://github.com/danielraffel/pulp/pull/4061))
- refactor(local-ci): extract ssh bundle bindings ([#4027](https://github.com/danielraffel/pulp/pull/4027))
- test(validation): fix two Windows-only host-lab test fixtures ([#4060](https://github.com/danielraffel/pulp/pull/4060))
- Offline stretch engine + PulpTempoSampler (Phases 0-4) ([#4054](https://github.com/danielraffel/pulp/pull/4054))
- test(validation): check AU gatekeeper preflight ([#4058](https://github.com/danielraffel/pulp/pull/4058))
- Add package kit and content workflows ([#4044](https://github.com/danielraffel/pulp/pull/4044))
- feature/dsp hostlab coverage summary ([#4052](https://github.com/danielraffel/pulp/pull/4052))

<a id="v04320"></a>
## [0.432.0] - 2026-06-13

- feat(view): close text input behavior gaps ([#4055](https://github.com/danielraffel/pulp/pull/4055))
- docs(audio): explain inspector signal and scope modes ([#4056](https://github.com/danielraffel/pulp/pull/4056))

<a id="v04312"></a>
## [0.431.2] - 2026-06-12

- feature/round textbutton border ([#4047](https://github.com/danielraffel/pulp/pull/4047))

<a id="v04311"></a>
## [0.431.1] - 2026-06-12

- feature/standalone oversize transport advance ([#4051](https://github.com/danielraffel/pulp/pull/4051))

<a id="v04310"></a>
## [0.431.0] - 2026-06-12

- feature/standalone callback max block ([#4049](https://github.com/danielraffel/pulp/pull/4049))
- Fix Audio Scope follow-up CI failures ([#4046](https://github.com/danielraffel/pulp/pull/4046))

<a id="v04300"></a>
## [0.430.0] - 2026-06-12

- feature/cpu host continuous frames ([#4040](https://github.com/danielraffel/pulp/pull/4040))
- Add Audio Scope analysis surface ([#4038](https://github.com/danielraffel/pulp/pull/4038))

<a id="v04290"></a>
## [0.429.0] - 2026-06-12

- feature/standalone chrome editor fill ([#4041](https://github.com/danielraffel/pulp/pull/4041))
- docs: link public shipping guide from migration notes ([#4033](https://github.com/danielraffel/pulp/pull/4033))

<a id="v04280"></a>
## [0.428.0] - 2026-06-11

- test: prove DSP runtime RT contracts ([#4028](https://github.com/danielraffel/pulp/pull/4028))

<a id="v04270"></a>
## [0.427.0] - 2026-06-11

- Expose Audio Inspector probe JSON to agents ([#3989](https://github.com/danielraffel/pulp/pull/3989))

<a id="v04261"></a>
## [0.426.1] - 2026-06-11

- fix(view): CPU plugin-view host drives continuous frames + idle pump ([#4029](https://github.com/danielraffel/pulp/pull/4029))
- refactor(local-ci): extract target preflight bindings ([#4026](https://github.com/danielraffel/pulp/pull/4026))
- refactor(local-ci): expand execution helper bindings ([#4025](https://github.com/danielraffel/pulp/pull/4025))
- refactor(local-ci): expand queue display bindings ([#4024](https://github.com/danielraffel/pulp/pull/4024))
- refactor(local-ci): expand queue runner bindings ([#4023](https://github.com/danielraffel/pulp/pull/4023))
- refactor(local-ci): expand source prep bindings ([#4022](https://github.com/danielraffel/pulp/pull/4022))
- refactor(local-ci): extract desktop reporting bindings ([#4021](https://github.com/danielraffel/pulp/pull/4021))

<a id="v04260"></a>
## [0.426.0] - 2026-06-11

- feat(view): per-view continuous-repaint opt-in for live custom views ([#4015](https://github.com/danielraffel/pulp/pull/4015))
- refactor(local-ci): extract desktop probe bindings ([#4020](https://github.com/danielraffel/pulp/pull/4020))
- refactor(local-ci): extract desktop command bindings ([#4019](https://github.com/danielraffel/pulp/pull/4019))
- refactor(local-ci): extract utility command bindings ([#4018](https://github.com/danielraffel/pulp/pull/4018))
- refactor(local-ci): extract command bindings ([#4017](https://github.com/danielraffel/pulp/pull/4017))
- refactor(local-ci): extract dispatch bindings ([#4016](https://github.com/danielraffel/pulp/pull/4016))
- refactor(local-ci): extract execution processing bindings ([#4014](https://github.com/danielraffel/pulp/pull/4014))
- refactor(local-ci): extract queue lifecycle bindings ([#4013](https://github.com/danielraffel/pulp/pull/4013))
- refactor(local-ci): extract validation execution bindings ([#4012](https://github.com/danielraffel/pulp/pull/4012))
- refactor(local-ci): extract windows desktop facade bindings ([#4011](https://github.com/danielraffel/pulp/pull/4011))
- refactor(local-ci): extract linux desktop facade bindings ([#4009](https://github.com/danielraffel/pulp/pull/4009))
- refactor(local-ci): extract source prep facade bindings ([#4007](https://github.com/danielraffel/pulp/pull/4007))

<a id="v04250"></a>
## [0.425.0] - 2026-06-11

- feat(signal): FreezeLoopSampler — capture-and-loop time-domain freeze ([#4006](https://github.com/danielraffel/pulp/pull/4006))
- refactor(local-ci): extract macos desktop facade bindings ([#4005](https://github.com/danielraffel/pulp/pull/4005))

<a id="v04240"></a>
## [0.424.0] - 2026-06-11

- feat(signal): optional sinc resampling in RealtimePitchTimeProcessor ([#4004](https://github.com/danielraffel/pulp/pull/4004))
- refactor(local-ci): move artifact wait helper to io utils ([#4002](https://github.com/danielraffel/pulp/pull/4002))

<a id="v04230"></a>
## [0.423.0] - 2026-06-11

- feat(signal): Kaiser-windowed sinc resampler primitive ([#4000](https://github.com/danielraffel/pulp/pull/4000))

<a id="v04220"></a>
## [0.422.0] - 2026-06-11

- fix(host): wait for SignalGraph readers before release ([#3999](https://github.com/danielraffel/pulp/pull/3999))

<a id="v04210"></a>
## [0.421.0] - 2026-06-11

- feat(signal): noise-morphing path in RealtimePitchTimeProcessor (STN-routed) ([#3997](https://github.com/danielraffel/pulp/pull/3997))

<a id="v04200"></a>
## [0.420.0] - 2026-06-10

- feat(signal): NoiseMorpher — transparent noise time-stretch by morphing ([#3995](https://github.com/danielraffel/pulp/pull/3995))

<a id="v04190"></a>
## [0.419.0] - 2026-06-11

- feat(signal): STN (sines/transients/noise) decomposition by median filtering ([#3994](https://github.com/danielraffel/pulp/pull/3994))

<a id="v04180"></a>
## [0.418.0] - 2026-06-10

- test(midi): cover all bind_midi overloads + map remove-keep path ([#3992](https://github.com/danielraffel/pulp/pull/3992))
- test(signal): align delay max block with debug test size ([#3988](https://github.com/danielraffel/pulp/pull/3988))
- feature/parameter automation binding ([#3984](https://github.com/danielraffel/pulp/pull/3984))

<a id="v04170"></a>
## [0.417.0] - 2026-06-10

- fix/spectral frame edge norm ([#3978](https://github.com/danielraffel/pulp/pull/3978))

<a id="v04160"></a>
## [0.416.0] - 2026-06-10

- feature/dsp public dense param contract ([#3980](https://github.com/danielraffel/pulp/pull/3980))
- feature/signal rt helper contract ([#3976](https://github.com/danielraffel/pulp/pull/3976))
- feature/refactor local ci desktop management cli ([#3974](https://github.com/danielraffel/pulp/pull/3974))

<a id="v04150"></a>
## [0.415.0] - 2026-06-10

- feature/signalgraph midi mailbox ([#3973](https://github.com/danielraffel/pulp/pull/3973))
- feature/signal spectral primitives ([#3970](https://github.com/danielraffel/pulp/pull/3970))

<a id="v04140"></a>
## [0.414.0] - 2026-06-10

- feature/signalgraph thread ownership ([#3967](https://github.com/danielraffel/pulp/pull/3967))
- fix(release): make CLI backfills safe by default ([#3963](https://github.com/danielraffel/pulp/pull/3963))

<a id="v04130"></a>
## [0.413.0] - 2026-06-10

- feature/graph midi block sidecars ([#3959](https://github.com/danielraffel/pulp/pull/3959))

<a id="v04122"></a>
## [0.412.2] - 2026-06-10

- fix/win ble scan winrt init ([#3958](https://github.com/danielraffel/pulp/pull/3958))

<a id="v04120"></a>
## [0.412.0] - 2026-06-10

- feat/audio inspector host wiring ([#3954](https://github.com/danielraffel/pulp/pull/3954))

<a id="v04110"></a>
## [0.411.0] - 2026-06-10

- fix/audio harness sweep followups ([#3955](https://github.com/danielraffel/pulp/pull/3955))
- refactor(local-ci): extract desktop status output helpers ([#3953](https://github.com/danielraffel/pulp/pull/3953))
- fix/audio probe followups ([#3951](https://github.com/danielraffel/pulp/pull/3951))
- refactor(local-ci): extract desktop summary output helpers ([#3952](https://github.com/danielraffel/pulp/pull/3952))

<a id="v04100"></a>
## [0.410.0] - 2026-06-10

- fix/clap inbound sysex rt ([#3945](https://github.com/danielraffel/pulp/pull/3945))
- refactor(local-ci): extract desktop recent proof output ([#3950](https://github.com/danielraffel/pulp/pull/3950))
- feature/refactor local ci desktop cli lines ([#3946](https://github.com/danielraffel/pulp/pull/3946))
- fix(ci): clean up release review sweep findings ([#3940](https://github.com/danielraffel/pulp/pull/3940))
- feature/refactor local ci desktop action policy ([#3944](https://github.com/danielraffel/pulp/pull/3944))
- feature/refactor local ci next slice ([#3943](https://github.com/danielraffel/pulp/pull/3943))

<a id="v04090"></a>
## [0.409.0] - 2026-06-10

- fix/audio probe wiring ([#3941](https://github.com/danielraffel/pulp/pull/3941))
- feature/refactor local ci desktop doctor probes ([#3939](https://github.com/danielraffel/pulp/pull/3939))

<a id="v04080"></a>
## [0.408.0] - 2026-06-10

- feature/param event overflow contract ([#3905](https://github.com/danielraffel/pulp/pull/3905))
- fix/local ci stale reconcile current queue ([#3942](https://github.com/danielraffel/pulp/pull/3942))

<a id="v04070"></a>
## [0.407.0] - 2026-06-10

- feature/audio harness 7 cli ([#3934](https://github.com/danielraffel/pulp/pull/3934))
- ci/codesign fatal ([#3938](https://github.com/danielraffel/pulp/pull/3938))
- ci/review followups ([#3937](https://github.com/danielraffel/pulp/pull/3937))
- feature/refactor local ci process job runner ([#3935](https://github.com/danielraffel/pulp/pull/3935))
- ci/strip fail loud ([#3936](https://github.com/danielraffel/pulp/pull/3936))
- feature/refactor local ci windows validation runner ([#3932](https://github.com/danielraffel/pulp/pull/3932))
- ci/coverage scheduled refresh ([#3933](https://github.com/danielraffel/pulp/pull/3933))
- feature/refactor local ci posix validation runner ([#3931](https://github.com/danielraffel/pulp/pull/3931))
- ci/release make latest ([#3930](https://github.com/danielraffel/pulp/pull/3930))
- feature/refactor local ci local validation runner ([#3929](https://github.com/danielraffel/pulp/pull/3929))
- feature/refactor local ci target task planning ([#3928](https://github.com/danielraffel/pulp/pull/3928))
- feature/refactor local ci execution planning ([#3927](https://github.com/danielraffel/pulp/pull/3927))
- feature/refactor local ci target task runner ([#3926](https://github.com/danielraffel/pulp/pull/3926))
- feature/refactor local ci target result ordering ([#3925](https://github.com/danielraffel/pulp/pull/3925))
- feature/refactor local ci target state snapshot ([#3924](https://github.com/danielraffel/pulp/pull/3924))
- feature/refactor local ci target progress state ([#3923](https://github.com/danielraffel/pulp/pull/3923))
- feature/refactor local ci initial target state ([#3922](https://github.com/danielraffel/pulp/pull/3922))
- feature/refactor local ci completed target state ([#3921](https://github.com/danielraffel/pulp/pull/3921))
- feature/refactor local ci job result helpers ([#3920](https://github.com/danielraffel/pulp/pull/3920))
- feature/refactor local ci unreachable result helper ([#3919](https://github.com/danielraffel/pulp/pull/3919))
- feature/refactor local ci windows validation script ([#3918](https://github.com/danielraffel/pulp/pull/3918))
- feature/refactor local ci posix validation command ([#3917](https://github.com/danielraffel/pulp/pull/3917))
- feature/refactor local ci local validation command ([#3916](https://github.com/danielraffel/pulp/pull/3916))
- feature/refactor local ci validation error result ([#3915](https://github.com/danielraffel/pulp/pull/3915))
- feature/refactor local ci validation result helper ([#3914](https://github.com/danielraffel/pulp/pull/3914))
- feature/refactor local ci validation helper policy ([#3913](https://github.com/danielraffel/pulp/pull/3913))
- feature/refactor local ci execution runner ([#3912](https://github.com/danielraffel/pulp/pull/3912))
- feature/refactor local ci drain active line ([#3911](https://github.com/danielraffel/pulp/pull/3911))
- feature/refactor local ci enqueue result line ([#3910](https://github.com/danielraffel/pulp/pull/3910))
- feature/refactor local ci pr list lines ([#3909](https://github.com/danielraffel/pulp/pull/3909))
- feature/refactor local ci evidence command lines ([#3908](https://github.com/danielraffel/pulp/pull/3908))
- feature/refactor local ci log command lines ([#3907](https://github.com/danielraffel/pulp/pull/3907))
- feature/refactor local ci queue command lines ([#3906](https://github.com/danielraffel/pulp/pull/3906))
- fix/release cli package overlay ([#3901](https://github.com/danielraffel/pulp/pull/3901))
- feature/param event ramp contract ([#3900](https://github.com/danielraffel/pulp/pull/3900))

<a id="v04060"></a>
## [0.406.0] - 2026-06-10

- chore/cut v0.406.0 ([#3902](https://github.com/danielraffel/pulp/pull/3902))
- feature/refactor local ci cleanup lines ([#3898](https://github.com/danielraffel/pulp/pull/3898))
- feature/signalgraph rt noalloc ([#3889](https://github.com/danielraffel/pulp/pull/3889))
- feature/refactor local ci footprint lines ([#3896](https://github.com/danielraffel/pulp/pull/3896))

<a id="v04050"></a>
## [0.405.0] - 2026-06-10

- feature/audio harness 6 inspector window ([#3895](https://github.com/danielraffel/pulp/pull/3895))
- ci/package cli resign ([#3894](https://github.com/danielraffel/pulp/pull/3894))
- feature/refactor local ci status runner lines ([#3893](https://github.com/danielraffel/pulp/pull/3893))
- feature/audio harness plugin surface ([#3891](https://github.com/danielraffel/pulp/pull/3891))
- ci/release cli rust selfheal ([#3865](https://github.com/danielraffel/pulp/pull/3865))
- feature/refactor local ci result formatting ([#3892](https://github.com/danielraffel/pulp/pull/3892))
- ci(release): keep backfill CLI CMake source-compatible ([#3890](https://github.com/danielraffel/pulp/pull/3890))
- feature/refactor local ci status formatting ([#3869](https://github.com/danielraffel/pulp/pull/3869))

<a id="v04040"></a>
## [0.404.0] - 2026-06-10

- feature/audio harness 5 rt probe ([#3864](https://github.com/danielraffel/pulp/pull/3864))
- feature/refactor local ci status target states ([#3863](https://github.com/danielraffel/pulp/pull/3863))
- feat: add sampler looper primitives ([#3808](https://github.com/danielraffel/pulp/pull/3808))
- ci(release): tolerate pre-swap CLI backfills ([#3866](https://github.com/danielraffel/pulp/pull/3866))

<a id="v04030"></a>
## [0.403.0] - 2026-06-09

- fix(signal): make oversampler processing allocation-free ([#3804](https://github.com/danielraffel/pulp/pull/3804))
- test(audio): offline Audio Doctor analyzers — magnitude response + THD ([#3862](https://github.com/danielraffel/pulp/pull/3862))
- ci/release cli rust path ([#3861](https://github.com/danielraffel/pulp/pull/3861))
- feature/refactor local ci status recent jobs ([#3860](https://github.com/danielraffel/pulp/pull/3860))
- feature/refactor local ci status active targets ([#3859](https://github.com/danielraffel/pulp/pull/3859))
- test(audio): named contract coverage for PulpGain/PulpEffect/PulpTone (harness PR 3) ([#3854](https://github.com/danielraffel/pulp/pull/3854))
- feature/refactor local ci status groups ([#3855](https://github.com/danielraffel/pulp/pull/3855))
- feature/refactor local ci target state formatting ([#3853](https://github.com/danielraffel/pulp/pull/3853))

<a id="v04020"></a>
## [0.402.0] - 2026-06-09

- feature/audio harness 2 render scenario ([#3852](https://github.com/danielraffel/pulp/pull/3852))
- feature/refactor local ci log selection ([#3851](https://github.com/danielraffel/pulp/pull/3851))
- feature/refactor local ci completion actions ([#3849](https://github.com/danielraffel/pulp/pull/3849))

<a id="v04010"></a>
## [0.401.0] - 2026-06-09

- feature/devtools command routing ([#3847](https://github.com/danielraffel/pulp/pull/3847))
- feature/refactor local ci command actions ([#3844](https://github.com/danielraffel/pulp/pull/3844))
- ci/release cli macos strip ([#3846](https://github.com/danielraffel/pulp/pull/3846))
- ci/release cli no lfs ([#3843](https://github.com/danielraffel/pulp/pull/3843))
- ci(release): include manifest in backfill overlay ([#3837](https://github.com/danielraffel/pulp/pull/3837))
- refactor(local-ci): extract runner active target update helper ([#3842](https://github.com/danielraffel/pulp/pull/3842))
- test(audio): convert golden/matrix suites to shared helpers + tone regression scenario (harness PR 1B) ([#3841](https://github.com/danielraffel/pulp/pull/3841))
- refactor(local-ci): extract runner stale selection helper ([#3838](https://github.com/danielraffel/pulp/pull/3838))

<a id="v04000"></a>
## [0.400.0] - 2026-06-09

- chore/cut v0.400.0 ([#3840](https://github.com/danielraffel/pulp/pull/3840))
- test(audio): add offline audio metrics/assertions/summary helpers (harness PR 1A) ([#3834](https://github.com/danielraffel/pulp/pull/3834))
- ci/release cli macos self hosted ([#3835](https://github.com/danielraffel/pulp/pull/3835))
- refactor(local-ci): extract running reconciliation helper ([#3836](https://github.com/danielraffel/pulp/pull/3836))
- ci(release): restore linux-arm64 Skia asset + guard the require-GPU/asset invariant ([#3814](https://github.com/danielraffel/pulp/pull/3814))
- refactor(local-ci): extract drain lifecycle helper ([#3825](https://github.com/danielraffel/pulp/pull/3825))
- docs(licensing): recognize system/OS-provided dependencies (BlueZ, D-Bus, ALSA, JACK, WinRT, …) ([#3828](https://github.com/danielraffel/pulp/pull/3828))

<a id="v03990"></a>
## [0.399.0] - 2026-06-09

- feat(midi): Windows WinRT GATT BLE MIDI connect/notify + MidiSystem port merge (W13 — completes BLE epic) ([#3830](https://github.com/danielraffel/pulp/pull/3830))

<a id="v03980"></a>
## [0.398.0] - 2026-06-09

- feat(midi): Linux BlueZ BLE MIDI backend + virtual MIDI-port merge (L13) ([#3824](https://github.com/danielraffel/pulp/pull/3824))
- refactor(local-ci): extract stale validator lifecycle helper ([#3823](https://github.com/danielraffel/pulp/pull/3823))
- feat(midi): WinRT BLE MIDI scan-probe backend + advisory compile-gate (W13 part 1) ([#3811](https://github.com/danielraffel/pulp/pull/3811))

<a id="v03970"></a>
## [0.397.0] - 2026-06-09

- feat(platform): D-Bus system-bus + signal subscription + GetManagedObjects (BLE prereq) ([#3812](https://github.com/danielraffel/pulp/pull/3812))
- refactor(local-ci): extract target state lifecycle helpers ([#3822](https://github.com/danielraffel/pulp/pull/3822))
- refactor(local-ci): extract enqueue lifecycle helper ([#3821](https://github.com/danielraffel/pulp/pull/3821))
- refactor(local-ci): extract wait loop helper ([#3820](https://github.com/danielraffel/pulp/pull/3820))

<a id="v03960"></a>
## [0.396.0] - 2026-06-09

- chore/cut v0.396.0 ([#3819](https://github.com/danielraffel/pulp/pull/3819))
- fix/skia linux arm64 release asset ([#3817](https://github.com/danielraffel/pulp/pull/3817))
- refactor(local-ci): extract finalize lifecycle helper ([#3818](https://github.com/danielraffel/pulp/pull/3818))
- refactor(local-ci): extract locked job lifecycle helpers ([#3813](https://github.com/danielraffel/pulp/pull/3813))
- feature/svgpath jsx parity contract ([#3810](https://github.com/danielraffel/pulp/pull/3810))
- refactor(local-ci): extract runner state helpers ([#3809](https://github.com/danielraffel/pulp/pull/3809))
- feature/svgpath wiring parity ([#3802](https://github.com/danielraffel/pulp/pull/3802))
- refactor(local-ci): extract queue command mutation helpers ([#3805](https://github.com/danielraffel/pulp/pull/3805))
- refactor(local-ci): extract stale-running action helper ([#3803](https://github.com/danielraffel/pulp/pull/3803))
- refactor(local-ci): extract enqueue supersedence helper ([#3800](https://github.com/danielraffel/pulp/pull/3800))
- refactor(local-ci): extract enqueue state helpers ([#3796](https://github.com/danielraffel/pulp/pull/3796))
- feature/svgpath fill rule ([#3794](https://github.com/danielraffel/pulp/pull/3794))
- ci/release artifact smoke ([#3795](https://github.com/danielraffel/pulp/pull/3795))

<a id="v03950"></a>
## [0.395.0] - 2026-06-09

- fix(midi,view): address Codex P2 review on UMP system messages + Linux XDND HiDPI ([#3793](https://github.com/danielraffel/pulp/pull/3793))
- refactor(local-ci): extract stale-running selection helper ([#3789](https://github.com/danielraffel/pulp/pull/3789))
- ci(release): stop re-running the unit suite in sign-and-release (not a test gate) ([#3783](https://github.com/danielraffel/pulp/pull/3783))
- feat(midi): implement WinRT MIDI 2.0 (UMP) backend with device hotplug ([#3781](https://github.com/danielraffel/pulp/pull/3781))
- fix(view): @pulp/react forwards <img src> to setImageSource (parity-found) ([#3790](https://github.com/danielraffel/pulp/pull/3790))
- refactor(local-ci): extract job completion helper ([#3788](https://github.com/danielraffel/pulp/pull/3788))
- refactor(local-ci): extract stale-running queue helpers ([#3787](https://github.com/danielraffel/pulp/pull/3787))
- refactor(local-ci): extract runner active-target helper ([#3786](https://github.com/danielraffel/pulp/pull/3786))

<a id="v03940"></a>
## [0.394.0] - 2026-06-09

- feat(view): HiDPI for the Win/Linux plugin-editor host (W8/L9) ([#3774](https://github.com/danielraffel/pulp/pull/3774))
- refactor(local-ci): extract terminal job completion ([#3784](https://github.com/danielraffel/pulp/pull/3784))
- refactor(local-ci): extract job claim helper ([#3782](https://github.com/danielraffel/pulp/pull/3782))
- refactor(local-ci): extract target state helper ([#3779](https://github.com/danielraffel/pulp/pull/3779))
- refactor(local-ci): extract queue state helpers ([#3777](https://github.com/danielraffel/pulp/pull/3777))
- refactor(local-ci): extract queue policy helpers ([#3776](https://github.com/danielraffel/pulp/pull/3776))
- refactor(local-ci): extract Windows remote probes ([#3773](https://github.com/danielraffel/pulp/pull/3773))
- refactor(local-ci): extract Windows SSH probe helpers ([#3771](https://github.com/danielraffel/pulp/pull/3771))

<a id="v03930"></a>
## [0.393.0] - 2026-06-09

- feat(view): Linux XDND drag-drop on the plugin-host X11 window (#3645) ([#3770](https://github.com/danielraffel/pulp/pull/3770))
- fix/ci release toolchain pin ([#3749](https://github.com/danielraffel/pulp/pull/3749))
- refactor(local-ci): extract desktop reporting helpers ([#3759](https://github.com/danielraffel/pulp/pull/3759))
- refactor(local-ci): extract desktop artifact layout helpers ([#3755](https://github.com/danielraffel/pulp/pull/3755))
- refactor(local-ci): move stale validator cleanup helpers ([#3757](https://github.com/danielraffel/pulp/pull/3757))

<a id="v03922"></a>
## [0.392.2] - 2026-06-09

- refactor(canvas): move Skia font shim to private TU ([#3756](https://github.com/danielraffel/pulp/pull/3756))
- refactor(local-ci): extract cleanup planning helpers ([#3752](https://github.com/danielraffel/pulp/pull/3752))
- refactor(local-ci): extract SSH bundle transport helpers ([#3748](https://github.com/danielraffel/pulp/pull/3748))
- refactor(local-ci): extract evidence index module ([#3747](https://github.com/danielraffel/pulp/pull/3747))

<a id="v03921"></a>
## [0.392.1] - 2026-06-09

- fix/release build cxx17 aggregate init ([#3745](https://github.com/danielraffel/pulp/pull/3745))
- refactor: migrate Canvas2D bridge API to registry helper ([#3746](https://github.com/danielraffel/pulp/pull/3746))

<a id="v03920"></a>
## [0.392.0] - 2026-06-09

- feat(view): Linux AT-SPI Value interface + focus/value/name event signals (L7c) ([#3744](https://github.com/danielraffel/pulp/pull/3744))
- refactor: migrate GPU bridge API to registry helper ([#3741](https://github.com/danielraffel/pulp/pull/3741))
- refactor: migrate typography bridge API to registry helper ([#3740](https://github.com/danielraffel/pulp/pull/3740))

<a id="v03910"></a>
## [0.391.0] - 2026-06-09

- feat(view): per-widget AT-SPI Accessible + Component tree (L7b) ([#3737](https://github.com/danielraffel/pulp/pull/3737))
- refactor: migrate animation bridge API to registry helper ([#3736](https://github.com/danielraffel/pulp/pull/3736))
- refactor: migrate layout bridge API to registry helper ([#3735](https://github.com/danielraffel/pulp/pull/3735))
- refactor: migrate widget factory bridge API to registry helper ([#3734](https://github.com/danielraffel/pulp/pull/3734))
- refactor: migrate event bridge API to registry helper ([#3733](https://github.com/danielraffel/pulp/pull/3733))

<a id="v03900"></a>
## [0.390.0] - 2026-06-09

- feat(view): register Linux AT-SPI root over direct D-Bus (L7a-2) ([#3732](https://github.com/danielraffel/pulp/pull/3732))
- refactor: migrate runtime bridge API to registry helper ([#3731](https://github.com/danielraffel/pulp/pull/3731))
- refactor: migrate SVG bridge API to registry helper ([#3726](https://github.com/danielraffel/pulp/pull/3726))
- refactor: migrate platform services bridge API to registry helper ([#3725](https://github.com/danielraffel/pulp/pull/3725))
- refactor: migrate widget asset bridge API to registry helper ([#3723](https://github.com/danielraffel/pulp/pull/3723))
- refactor: migrate runtime import bridge API to registry helper ([#3720](https://github.com/danielraffel/pulp/pull/3720))
- refactor: migrate widget schema bridge API to registry helper ([#3721](https://github.com/danielraffel/pulp/pull/3721))
- feat(platform): generic D-Bus object-server layer (L7a-1) ([#3698](https://github.com/danielraffel/pulp/pull/3698))

<a id="v03890"></a>
## [0.389.0] - 2026-06-09

- feat(audio): WASAPI shared low-latency (IAudioClient3) + device-invalidation recovery (W4b) ([#3717](https://github.com/danielraffel/pulp/pull/3717))
- refactor: migrate RN compatibility bridge API to registry helper ([#3705](https://github.com/danielraffel/pulp/pull/3705))
- refactor: migrate widget value content bridge API to registry helper ([#3716](https://github.com/danielraffel/pulp/pull/3716))

<a id="v03880"></a>
## [0.388.0] - 2026-06-09

- feat(audio): WASAPI exclusive-mode output via DeviceConfig::share_mode (W4a) ([#3676](https://github.com/danielraffel/pulp/pull/3676))
- refactor: migrate style effects bridge API to registry helper ([#3711](https://github.com/danielraffel/pulp/pull/3711))

<a id="v03874"></a>
## [0.387.4] - 2026-06-09

- fix(runtime): address post-merge review feedback (#3488 sha256/collision + #3492 caret test) ([#3703](https://github.com/danielraffel/pulp/pull/3703))
- ci(coverage): include importer CLI test targets in the diff-cover lane ([#3707](https://github.com/danielraffel/pulp/pull/3707))
- refactor: migrate storage asset bridge API to registry helper ([#3706](https://github.com/danielraffel/pulp/pull/3706))

<a id="v03873"></a>
## [0.387.3] - 2026-06-08

- refactor: migrate WidgetBridge DOM registrar to API registry ([#3687](https://github.com/danielraffel/pulp/pull/3687))
- refactor: migrate storage style bridge API to registry helper ([#3699](https://github.com/danielraffel/pulp/pull/3699))
- refactor: migrate value controls bridge API to registry helper ([#3697](https://github.com/danielraffel/pulp/pull/3697))
- refactor: migrate visual style bridge API to registry helper ([#3696](https://github.com/danielraffel/pulp/pull/3696))
- feat(import): pulp tool install/uninstall for framework importers ([#3693](https://github.com/danielraffel/pulp/pull/3693))
- refactor: migrate visibility bridge API to registry helper ([#3695](https://github.com/danielraffel/pulp/pull/3695))
- refactor: migrate border-box bridge API to registry helper ([#3692](https://github.com/danielraffel/pulp/pull/3692))
- refactor: migrate basic value bridge API to registry helper ([#3694](https://github.com/danielraffel/pulp/pull/3694))

<a id="v03861"></a>
## [0.386.1] - 2026-06-08

- refactor: migrate theme bridge API to registry helper ([#3689](https://github.com/danielraffel/pulp/pull/3689))
- refactor: migrate tokens bridge API to registry helper ([#3688](https://github.com/danielraffel/pulp/pull/3688))
- refactor: migrate border-side bridge API to registry helper ([#3686](https://github.com/danielraffel/pulp/pull/3686))
- refactor: migrate border-radius bridge API to registry helper ([#3685](https://github.com/danielraffel/pulp/pull/3685))
- refactor: migrate outline bridge API to registry helper ([#3684](https://github.com/danielraffel/pulp/pull/3684))
- refactor: migrate metadata bridge API to registry helper ([#3683](https://github.com/danielraffel/pulp/pull/3683))
- refactor: migrate shader bridge API to registry helper ([#3682](https://github.com/danielraffel/pulp/pull/3682))
- refactor: migrate list-style bridge API to registry helper ([#3677](https://github.com/danielraffel/pulp/pull/3677))
- feat(design-import): auto-label faithful interactive elements from layer names (§2.1) ([#3691](https://github.com/danielraffel/pulp/pull/3691))
- feat(view): Windows UIA per-widget fragments (a11y Phase 3, W6) ([#3680](https://github.com/danielraffel/pulp/pull/3680))

<a id="v03860"></a>
## [0.386.0] - 2026-06-09

- feat(import): IMPORTER_TERMS accept-gate + provenance PR-check ([#3681](https://github.com/danielraffel/pulp/pull/3681))
- refactor: migrate WidgetBridge style-state registrar to API registry ([#3678](https://github.com/danielraffel/pulp/pull/3678))
- feat(design-import): IRInteractiveElement.label — caption -> generated-param name ([#3671](https://github.com/danielraffel/pulp/pull/3671))
- refactor: migrate WidgetBridge value-list registrar to API registry ([#3675](https://github.com/danielraffel/pulp/pull/3675))

<a id="v03850"></a>
## [0.385.0] - 2026-06-08

- refactor(cli): split pulp import SPI-verb orchestration into import_run ([#3673](https://github.com/danielraffel/pulp/pull/3673))

<a id="v03840"></a>
## [0.384.0] - 2026-06-08

- feat(view): Windows IDropTarget drag-drop on the plugin-host child HWND (#3645) ([#3670](https://github.com/danielraffel/pulp/pull/3670))
- refactor: migrate WidgetBridge leaf registrars to API registry ([#3672](https://github.com/danielraffel/pulp/pull/3672))

<a id="v03830"></a>
## [0.383.0] - 2026-06-09

- feat(view): deliver macOS native drops into the dispatch core (#3645) ([#3669](https://github.com/danielraffel/pulp/pull/3669))
- refactor: split WidgetBridge visual style registrar ([#3661](https://github.com/danielraffel/pulp/pull/3661))

<a id="v03821"></a>
## [0.382.1] - 2026-06-08

- ci(audio): compile the JACK backend in Linux CI + open/start/stop smoke (L5b) ([#3657](https://github.com/danielraffel/pulp/pull/3657))

<a id="v03820"></a>
## [0.382.0] - 2026-06-08

- feat(import): materialize pulp import emit into a buildable scaffold + clean-room output scan ([#3634](https://github.com/danielraffel/pulp/pull/3634))
- refactor: split WidgetBridge storage style registrar ([#3653](https://github.com/danielraffel/pulp/pull/3653))
- refactor: split WidgetBridge visibility style registrar ([#3652](https://github.com/danielraffel/pulp/pull/3652))
- refactor: split WidgetBridge event registrar ([#3649](https://github.com/danielraffel/pulp/pull/3649))

<a id="v03810"></a>
## [0.381.0] - 2026-06-08

- refactor(view): unify drag-drop on DropReceiver + fix header self-containment ([#3647](https://github.com/danielraffel/pulp/pull/3647))
- refactor: split WidgetBridge typography style registrars ([#3646](https://github.com/danielraffel/pulp/pull/3646))

<a id="v03800"></a>
## [0.380.0] - 2026-06-08

- feat(view): native drag-drop dispatch core + SDL standalone producer ([#3638](https://github.com/danielraffel/pulp/pull/3638))
- refactor: split WidgetBridge animation registrar ([#3644](https://github.com/danielraffel/pulp/pull/3644))
- refactor: split WidgetBridge canvas2d registrar ([#3642](https://github.com/danielraffel/pulp/pull/3642))
- fix(ci): give Tart ephemeral runners a static name per (host, slot) ([#3632](https://github.com/danielraffel/pulp/pull/3632))
- feature/refactor widgetbridge runtime registrar ([#3643](https://github.com/danielraffel/pulp/pull/3643))
- feature/refactor widgetbridge border side registrar ([#3641](https://github.com/danielraffel/pulp/pull/3641))
- refactor: split WidgetBridge border radius registrar ([#3640](https://github.com/danielraffel/pulp/pull/3640))
- refactor: split WidgetBridge runtime import registrar ([#3636](https://github.com/danielraffel/pulp/pull/3636))
- refactor: split WidgetBridge outline registrar ([#3637](https://github.com/danielraffel/pulp/pull/3637))
- docs(ci-skill): patch bumps auto-apply (#3626) + document --adopt-head SHA-drift recovery ([#3631](https://github.com/danielraffel/pulp/pull/3631))
- refactor: split WidgetBridge border box registrar ([#3630](https://github.com/danielraffel/pulp/pull/3630))

<a id="v03790"></a>
## [0.379.0] - 2026-06-08

- feat(view): Pulp-drawn ContextMenu (view-layer GPU popup menu) ([#3628](https://github.com/danielraffel/pulp/pull/3628))
- refactor: split WidgetBridge DOM registrar ([#3624](https://github.com/danielraffel/pulp/pull/3624))
- refactor: split WidgetBridge text runs registrar ([#3629](https://github.com/danielraffel/pulp/pull/3629))
- refactor: split WidgetBridge typography registrar ([#3625](https://github.com/danielraffel/pulp/pull/3625))

<a id="v03780"></a>
## [0.378.0] - 2026-06-08

- chore(deps): pin choc to fork tag for WebView file drag-and-drop (interim) ([#3620](https://github.com/danielraffel/pulp/pull/3620))
- ci: fix SIGPIPE (exit 141) in auto-release tag lookup ([#3627](https://github.com/danielraffel/pulp/pull/3627))
- feat(import): pulp import command + tool-registry importer fields + SPI runner ([#3608](https://github.com/danielraffel/pulp/pull/3608))
- fix(scripts): apply patch bumps in version_bump apply_bumps (Shipyard#358) ([#3626](https://github.com/danielraffel/pulp/pull/3626))
- fix(canvas): upload mask/pattern shader images to GPU texture (Graphite) ([#3607](https://github.com/danielraffel/pulp/pull/3607))
- fix(view): route knob/fader/toggle DOM tags to native widgets in live-JSX path ([#3617](https://github.com/danielraffel/pulp/pull/3617))
- refactor: split WidgetBridge storage/assets registrar ([#3623](https://github.com/danielraffel/pulp/pull/3623))
- refactor: split WidgetBridge layout registrar ([#3622](https://github.com/danielraffel/pulp/pull/3622))
- refactor: split WidgetBridge basic value registrar ([#3621](https://github.com/danielraffel/pulp/pull/3621))
- feat(view): Windows V8 consumer via clang-cl + Chromium libc++ (__Cr) ([#3603](https://github.com/danielraffel/pulp/pull/3603))
- feat(ship): Windows installer Start-menu shortcuts + signtool failure contract (W7) ([#3618](https://github.com/danielraffel/pulp/pull/3618))
- fix(cmake): Linux SDK export — re-find ICU + expose fontconfig link helper ([#3616](https://github.com/danielraffel/pulp/pull/3616))
- refactor: split WidgetBridge content value registrar ([#3604](https://github.com/danielraffel/pulp/pull/3604))
- feat(runtime): Windows high-resolution OS timer-queue backend (W9) ([#3605](https://github.com/danielraffel/pulp/pull/3605))
- deps(skia): migrate Skia/Dawn prebuilt toolchain m149 -> m150 ([#3606](https://github.com/danielraffel/pulp/pull/3606))
- docs: codify no-Namespace, local-Macs-first macOS CI policy ([#3602](https://github.com/danielraffel/pulp/pull/3602))
- refactor: split WidgetBridge list value registrar ([#3601](https://github.com/danielraffel/pulp/pull/3601))
- refactor: split WidgetBridge value control registrar ([#3600](https://github.com/danielraffel/pulp/pull/3600))
- refactor: split WidgetBridge platform services registrar ([#3599](https://github.com/danielraffel/pulp/pull/3599))
- feature/refactor widgetbridge metadata registrar ([#3598](https://github.com/danielraffel/pulp/pull/3598))
- feature/refactor widgetbridge widget assets registrar ([#3597](https://github.com/danielraffel/pulp/pull/3597))
- feature/refactor widgetbridge shader registrar ([#3596](https://github.com/danielraffel/pulp/pull/3596))
- feature/refactor widgetbridge factory api clean ([#3595](https://github.com/danielraffel/pulp/pull/3595))
- feature/refactor widgetbridge theme registrar ([#3594](https://github.com/danielraffel/pulp/pull/3594))
- feature/refactor widgetbridge widget schema registrar ([#3593](https://github.com/danielraffel/pulp/pull/3593))
- feature/refactor widgetbridge state binding registrar ([#3592](https://github.com/danielraffel/pulp/pull/3592))
- feature/refactor widgetbridge tokens registrar ([#3591](https://github.com/danielraffel/pulp/pull/3591))
- feature/refactor widgetbridge svg registrar ([#3590](https://github.com/danielraffel/pulp/pull/3590))
- feature/refactor widgetbridge list style registrar ([#3588](https://github.com/danielraffel/pulp/pull/3588))
- feature/refactor widgetbridge registrar slice ([#3587](https://github.com/danielraffel/pulp/pull/3587))
- feature/refactor debt inventory ([#3586](https://github.com/danielraffel/pulp/pull/3586))
- feature/refactor rt safety harness ([#3585](https://github.com/danielraffel/pulp/pull/3585))
- feature/refactor renderer3d cpu seams ([#3583](https://github.com/danielraffel/pulp/pull/3583))
- feature/refactor test outliers ([#3582](https://github.com/danielraffel/pulp/pull/3582))
- feature/refactor local ci contracts ([#3581](https://github.com/danielraffel/pulp/pull/3581))
- feature/refactor downstream validation ([#3579](https://github.com/danielraffel/pulp/pull/3579))
- ci/scene coverage tier ([#3577](https://github.com/danielraffel/pulp/pull/3577))
- test: add WidgetBridge API manifest contract ([#3575](https://github.com/danielraffel/pulp/pull/3575))
- ci: guard refactor hotspot sizes ([#3574](https://github.com/danielraffel/pulp/pull/3574))
- test(view): cover ScriptEngine identity pass-throughs ([#3573](https://github.com/danielraffel/pulp/pull/3573))
- feat(platform): Windows native file dialogs via IFileDialog (W5) ([#3571](https://github.com/danielraffel/pulp/pull/3571))
- fix(platform): real Win32 clipboard (was fake in-process shadow) ([#3566](https://github.com/danielraffel/pulp/pull/3566))
- chore(design-import): return the standalone demo knobs to inert (pulp #3562) ([#3565](https://github.com/danielraffel/pulp/pull/3565))
- fix(design-import): tab-strip glow + digit doubling + dropdown label fit ([#3572](https://github.com/danielraffel/pulp/pull/3572))
- feat(import): project-import SPI contract + plan/manifest/compat schemas ([#3568](https://github.com/danielraffel/pulp/pull/3568))
- fix(design-import): skip overlay controls occluded by a later opaque node ([#3567](https://github.com/danielraffel/pulp/pull/3567))
- feat(ship): Linux AppImage packaging for standalone apps (L8b) ([#3563](https://github.com/danielraffel/pulp/pull/3563))
- feat(plugin): add claude.send_user_file config + SessionStart injection ([#3554](https://github.com/danielraffel/pulp/pull/3554))

<a id="v03711"></a>
## [0.371.1] - 2026-06-08

- fix(cli): `pulp upgrade` self-heals PATH after a source/SDK-prefix install ([#3556](https://github.com/danielraffel/pulp/pull/3556))
- fix(design-import): a single-option stepper paints nothing (no doubled header text) ([#3558](https://github.com/danielraffel/pulp/pull/3558))

<a id="v03710"></a>
## [0.371.0] - 2026-06-07

- feat(platform): Linux binary clipboard via wl-copy/xclip custom types (L9a) ([#3557](https://github.com/danielraffel/pulp/pull/3557))

<a id="v03701"></a>
## [0.370.1] - 2026-06-07

- fix(view): embedded plugin-view host routes keyDown to the focused input ([#3552](https://github.com/danielraffel/pulp/pull/3552))
- feat(import): default design import to the interactive faithful-vector lane ([#3551](https://github.com/danielraffel/pulp/pull/3551))
- test(import): codify design-import tab group as an interactive overlay ([#3550](https://github.com/danielraffel/pulp/pull/3550))

<a id="v03700"></a>
## [0.370.0] - 2026-06-07

- fix(view): keep selected glyphs in place when selecting across a space ([#3549](https://github.com/danielraffel/pulp/pull/3549))

<a id="v03690"></a>
## [0.369.0] - 2026-06-07

- fix(design-import): suppress the baked selected-tab highlight (no double-pill) ([#3544](https://github.com/danielraffel/pulp/pull/3544))
- chore: de-brand the design-import example; credit ELYSIUM as a test reference ([#3527](https://github.com/danielraffel/pulp/pull/3527))

<a id="v03680"></a>
## [0.368.0] - 2026-06-07

- fix(design-import): preserve a search field's leading icon (inset + bg-color match) ([#3526](https://github.com/danielraffel/pulp/pull/3526))
- feat(platform): Linux file dialogs via xdg-desktop-portal + reusable DBus client ([#3539](https://github.com/danielraffel/pulp/pull/3539))

<a id="v03670"></a>
## [0.367.0] - 2026-06-07

- feat(view): ScriptedUiSession explicit reload() / reload_from() (on-demand, last-good) ([#3541](https://github.com/danielraffel/pulp/pull/3541))

<a id="v03660"></a>
## [0.366.0] - 2026-06-07

- feat(view): DesignFrameView event-driven binding API (on_element_changed + gestures + uniform value) ([#3537](https://github.com/danielraffel/pulp/pull/3537))

<a id="v03650"></a>
## [0.365.0] - 2026-06-07

- feat(midi): Linux ALSA MIDI device hotplug; promote udev monitor to runtime (#3327 / L4) ([#3535](https://github.com/danielraffel/pulp/pull/3535))

<a id="v03640"></a>
## [0.364.0] - 2026-06-07

- feat(audio): Linux ALSA device hotplug via libudev (#3327 / L4) ([#3533](https://github.com/danielraffel/pulp/pull/3533))
- docs(status): reflect shipped Win/Linux catch-up slices in support matrix ([#3528](https://github.com/danielraffel/pulp/pull/3528))
- test(view): harden render_to_rgba raster test against transient load (retry-on-blank) ([#3532](https://github.com/danielraffel/pulp/pull/3532))

<a id="v03630"></a>
## [0.363.0] - 2026-06-07

- feat(view): cross-platform headless Skia screenshot + Win/Linux PluginViewHost ([#3500](https://github.com/danielraffel/pulp/pull/3500))
- feat(figma-plugin): detect overlay controls in the TS lane (lockstep with REST) ([#3521](https://github.com/danielraffel/pulp/pull/3521))

<a id="v03620"></a>
## [0.362.0] - 2026-06-07

- fix(canvas): render registered fonts in SkParagraph + instance variable-font weight ([#3499](https://github.com/danielraffel/pulp/pull/3499))

<a id="v03610"></a>
## [0.361.0] - 2026-06-07

- feat(view): GPU plugin-view embedding hardening ([#3496](https://github.com/danielraffel/pulp/pull/3496))
- test(view): harden capture_view raster tests against transient CI starvation ([#3520](https://github.com/danielraffel/pulp/pull/3520))

<a id="v03602"></a>
## [0.360.2] - 2026-06-07

- fix(install): ship Skia static libs in the installed SDK ([#3497](https://github.com/danielraffel/pulp/pull/3497))
- test(import-design): revive cli-import-design suite + guard the baked figma-plugin lane ([#3498](https://github.com/danielraffel/pulp/pull/3498))
- fix(ci): force safe ccache config on macOS lane to stop false-hit corruption ([#3512](https://github.com/danielraffel/pulp/pull/3512))

<a id="v03601"></a>
## [0.360.1] - 2026-06-07

- fix(signal): wire FFTW3/MKL dynamic loading on Windows (perf parity) ([#3511](https://github.com/danielraffel/pulp/pull/3511))

<a id="v03600"></a>
## [0.360.0] - 2026-06-07

- fix(linux): JACK build break, real ALSA MIDI timestamps, .deb packaging routing ([#3495](https://github.com/danielraffel/pulp/pull/3495))
- test(view): size capture_view sparse-UI chip via flex so it actually paints ([#3517](https://github.com/danielraffel/pulp/pull/3517))

<a id="v03591"></a>
## [0.359.1] - 2026-06-07

- fix(design-import): emit only real option values; drop fabricated dropdown/stepper stubs ([#3514](https://github.com/danielraffel/pulp/pull/3514))

<a id="v03590"></a>
## [0.359.0] - 2026-06-07

- feat(design-import): header < > steppers (detect + slide value in place) ([#3513](https://github.com/danielraffel/pulp/pull/3513))

<a id="v03581"></a>
## [0.358.1] - 2026-06-07

- fix(model-registry): allow benign percent-encoding in hf:// paths; fix VST3 round-trip param pick ([#3510](https://github.com/danielraffel/pulp/pull/3510))
- build: one cpp-httplib interface target so all TUs share the mbedTLS macro ([#3507](https://github.com/danielraffel/pulp/pull/3507))

<a id="v03580"></a>
## [0.358.0] - 2026-06-07

- fix(model-manager): address code-review findings from PRs #3463/#3464 ([#3486](https://github.com/danielraffel/pulp/pull/3486))
- fix(ci): guard self-hosted macOS build dir against interrupted-build ODR ([#3504](https://github.com/danielraffel/pulp/pull/3504))

<a id="v03570"></a>
## [0.357.0] - 2026-06-07

- feat(view): headless GPU view capture + smart capture_view (P1) ([#3459](https://github.com/danielraffel/pulp/pull/3459))
- fix(ci): make jsc-bundler + design-import tests Windows-safe (path/CRLF) ([#3485](https://github.com/danielraffel/pulp/pull/3485))

<a id="v03560"></a>
## [0.356.0] - 2026-06-06

- fix(view): caret/selection X from shaped offsets, not summed glyph widths ([#3492](https://github.com/danielraffel/pulp/pull/3492))

<a id="v03551"></a>
## [0.355.1] - 2026-06-06

- fix(design-import): tab overlay no longer occludes content (drop opaque strip) ([#3493](https://github.com/danielraffel/pulp/pull/3493))
- feat(view): pin sealed libv8 as the V8 provider (macOS P0 slice) ([#3481](https://github.com/danielraffel/pulp/pull/3481))
- fix(design-import): only down-chevron dropdowns; exclude < > steppers + placeholder ([#3490](https://github.com/danielraffel/pulp/pull/3490))

<a id="v03550"></a>
## [0.355.0] - 2026-06-06

- feat(runtime): install_model fetches every asset of a multi-asset bundle ([#3488](https://github.com/danielraffel/pulp/pull/3488))

<a id="v03540"></a>
## [0.354.0] - 2026-06-06

- feat: ModelManager primitive + plugin-contributed Settings + standalone persistence ([#3464](https://github.com/danielraffel/pulp/pull/3464))

<a id="v03530"></a>
## [0.353.0] - 2026-06-06

- feat(design-import): native-overlay tabs on faithful frames (Plan B / full-A slice 4) ([#3482](https://github.com/danielraffel/pulp/pull/3482))

<a id="v03520"></a>
## [0.352.0] - 2026-06-06

- feat(design-import): native-overlay dropdowns on faithful frames (Plan B / full-A slice 3) ([#3480](https://github.com/danielraffel/pulp/pull/3480))

<a id="v03510"></a>
## [0.351.0] - 2026-06-06

- feat(design-import): native-overlay search field on faithful frames (Plan B / full-A slice 2) ([#3477](https://github.com/danielraffel/pulp/pull/3477))

<a id="v03500"></a>
## [0.350.0] - 2026-06-06

- feat(design-import): typed IR for dropdown/text_field/tab_group overlays (Plan B / full-A slice 1) ([#3475](https://github.com/danielraffel/pulp/pull/3475))

<a id="v03490"></a>
## [0.349.0] - 2026-06-06

- fix(design-import): faithful frame is interactive + fits at any window aspect ([#3474](https://github.com/danielraffel/pulp/pull/3474))
- test(design-import): end-to-end faithful_svg envelope → DesignFrameView (Plan B / B5) ([#3473](https://github.com/danielraffel/pulp/pull/3473))
- feat(design-import): Figma plugin emits faithful-vector frames (Plan B / B4b) ([#3470](https://github.com/danielraffel/pulp/pull/3470))
- feat(design-import): REST exporter emits faithful-vector frames (Plan B / B4a) ([#3469](https://github.com/danielraffel/pulp/pull/3469))

<a id="v03480"></a>
## [0.348.0] - 2026-06-06

- feat(design-import): materialize faithful_svg nodes as DesignFrameView (Plan B / B3) ([#3468](https://github.com/danielraffel/pulp/pull/3468))

<a id="v03470"></a>
## [0.347.0] - 2026-06-06

- feat(design-import): typed IR for faithful-vector render mode (Plan B / B2 slice 1) ([#3466](https://github.com/danielraffel/pulp/pull/3466))

<a id="v03460"></a>
## [0.346.0] - 2026-06-05

- feat(view): DesignFrameView — faithful-vector design-import view (Plan B / B1) ([#3465](https://github.com/danielraffel/pulp/pull/3465))

<a id="v03450"></a>
## [0.345.0] - 2026-06-05

- feat(canvas): Canvas::draw_svg — faithful SVG rendering via Skia SkSVGDOM ([#3462](https://github.com/danielraffel/pulp/pull/3462))

<a id="v03440"></a>
## [0.344.0] - 2026-06-05

- fix(design-import): non-destructive knob antenna clean — remove baked indicator without notching the disc ([#3460](https://github.com/danielraffel/pulp/pull/3460))
- ci: retry flaky tests on sanitizer + coverage lanes (--repeat until-pass:2) ([#3458](https://github.com/danielraffel/pulp/pull/3458))

<a id="v03430"></a>
## [0.343.0] - 2026-06-05

- feat(format): .pulpset render/replay harness (G4) ([#3457](https://github.com/danielraffel/pulp/pull/3457))

<a id="v03420"></a>
## [0.342.0] - 2026-06-05

- feat(design-import): per-shape gradient fill reproduces each shape's own colors ([#3455](https://github.com/danielraffel/pulp/pull/3455))

<a id="v03410"></a>
## [0.341.0] - 2026-06-05

- fix(design-import): recognize demoted knob pointer + erase baked disc indicator ([#3453](https://github.com/danielraffel/pulp/pull/3453))
- feat(inspect): pulp_inspect_set_param MCP tool for live param mutation ([#3454](https://github.com/danielraffel/pulp/pull/3454))

<a id="v03400"></a>
## [0.340.0] - 2026-06-04

- feat(design-import): search-box field, dropdown precision, knob/dropdown polish ([#3451](https://github.com/danielraffel/pulp/pull/3451))
- fix(ci): make the macOS runner VM teardown fast enough for launchd's kill-grace ([#3450](https://github.com/danielraffel/pulp/pull/3450))

<a id="v03390"></a>
## [0.339.0] - 2026-06-04

- feat(design-import): recognize Dropdown frames + Search fields by layer name ([#3447](https://github.com/danielraffel/pulp/pull/3447))

<a id="v03380"></a>
## [0.338.0] - 2026-06-04

- feat(design-import): shape-fill capability + text-centering + EQ grid ([#3446](https://github.com/danielraffel/pulp/pull/3446))

<a id="v03370"></a>
## [0.337.0] - 2026-06-04

- fix(design-import): native knob + EQ image fidelity polish ([#3444](https://github.com/danielraffel/pulp/pull/3444))
- feat(examples): launchable ELYSIUM standalone — native import with sprite knobs ([#3442](https://github.com/danielraffel/pulp/pull/3442))
- test(harness): repoint planning submodule to rasterized ELYSIUM fixture (defect-2) ([#3441](https://github.com/danielraffel/pulp/pull/3441))
- fix(ci): tear down the in-flight VM when the macOS runner is stopped ([#3438](https://github.com/danielraffel/pulp/pull/3438))
- test(import): Phase D — imported fader/button/text-input interaction probes ([#3440](https://github.com/danielraffel/pulp/pull/3440))
- ci(macos): re-enable 2 audio tests via PROCESSORS reservation (RT-thread teardown fix) ([#3437](https://github.com/danielraffel/pulp/pull/3437))

<a id="v03360"></a>
## [0.336.0] - 2026-06-04

- fix(ci): clean up the QEMU VM when the JIT-config upload fails ([#3435](https://github.com/danielraffel/pulp/pull/3435))
- chore(test): make Windows tests portable ([#3432](https://github.com/danielraffel/pulp/pull/3432))
- ci(runners): accurate failure message on qemu early-exit vs no-SSH (#3433 review) ([#3434](https://github.com/danielraffel/pulp/pull/3434))
- ci(runners): address #3430 supervisor review findings (ssh-guard, TOCTOU, integrity) ([#3433](https://github.com/danielraffel/pulp/pull/3433))
- feat(ci): Linux + Windows ephemeral pool runners + opt-in self-hosted routing ([#3430](https://github.com/danielraffel/pulp/pull/3430))
- docs: dedupe Build from source, fold MCP note into the canonical block ([#3431](https://github.com/danielraffel/pulp/pull/3431))

<a id="v03350"></a>
## [0.335.0] - 2026-06-03

- Add native Scene3D glTF bake slice ([#3429](https://github.com/danielraffel/pulp/pull/3429))

<a id="v03340"></a>
## [0.334.0] - 2026-06-04

- feat(cli): add `pulp ci-host` wrapping setup-ci-host.sh (+ /ci-host) ([#3423](https://github.com/danielraffel/pulp/pull/3423))

<a id="v03330"></a>
## [0.333.0] - 2026-06-03

- Harden native Three.js WebGPU runtime ([#3391](https://github.com/danielraffel/pulp/pull/3391))
- docs: add Build from source section so source-tree MCP connects ([#3422](https://github.com/danielraffel/pulp/pull/3422))
- docs(ci): correct the issue auto-close gotcha — the colon is not the cause ([#3421](https://github.com/danielraffel/pulp/pull/3421))
- ci(macos-cap): VM-slot-aware reclaim watcher + tart-runner --loop capacity gate ([#3420](https://github.com/danielraffel/pulp/pull/3420))
- feat(native-components): Phase 2c — PulpRustGain example (Rust DSP core packaged as a CLAP plugin) ([#3387](https://github.com/danielraffel/pulp/pull/3387))
- chore(deps): bump Shipyard pin v0.67.0 -> v0.68.0 ([#3414](https://github.com/danielraffel/pulp/pull/3414))
- ci(macos-gate): capacity-aware routing (#3299), reachable gpu-perf sentinel, recovery recipe ([#3413](https://github.com/danielraffel/pulp/pull/3413))
- fix(view): resolve version-suffixed libnode as V8 provider ([#3409](https://github.com/danielraffel/pulp/pull/3409))

<a id="v03321"></a>
## [0.332.1] - 2026-06-03

- fix(import-design): harden SwiftUI codegen against non-finite floats + hostile grid tracks ([#3402](https://github.com/danielraffel/pulp/pull/3402))
- docs(native-components): reflect the shipped seam (de-defer node ABI + packs) ([#3403](https://github.com/danielraffel/pulp/pull/3403))

<a id="v03320"></a>
## [0.332.0] - 2026-06-03

- feat(import-design): SwiftUI grid + image assets + host scaffold (Workstream B5) ([#3401](https://github.com/danielraffel/pulp/pull/3401))
- feat(native-components): Phase 3 — AUv2 + LV2 param-events sidecar + RT-safety guard ([#3389](https://github.com/danielraffel/pulp/pull/3389))

<a id="v03310"></a>
## [0.331.0] - 2026-06-03

- feat(import-design): SwiftUI binding-manifest parity + adapter round-trip (Workstream B4) ([#3396](https://github.com/danielraffel/pulp/pull/3396))

<a id="v03300"></a>
## [0.330.0] - 2026-06-03

- feat(import-design): SwiftUI widget coverage — meter/xy_pad/waveform/spectrum + buttons (Workstream B3) ([#3395](https://github.com/danielraffel/pulp/pull/3395))

<a id="v03290"></a>
## [0.329.0] - 2026-06-03

- feat(import-design): full SwiftUI style + text-runs + flex-fidelity (Workstream B2) ([#3393](https://github.com/danielraffel/pulp/pull/3393))
- test(import-design): sync CLI assertions to --emit swiftui; doc local runner-log diagnosis ([#3390](https://github.com/danielraffel/pulp/pull/3390))

<a id="v03280"></a>
## [0.328.0] - 2026-06-03

- feat(import-design): baked SwiftUI emitter — `--emit swiftui` (Workstream B1) ([#3386](https://github.com/danielraffel/pulp/pull/3386))
- feat(import-design): faithful Skia validate render + --screenshot-backend; screenshot skill ([#3373](https://github.com/danielraffel/pulp/pull/3373))

<a id="v03270"></a>
## [0.327.0] - 2026-06-03

- feat(native-components): Phase 7 — signed node-pack dynamic loading ([#3374](https://github.com/danielraffel/pulp/pull/3374))

<a id="v03260"></a>
## [0.326.0] - 2026-06-02

- feat(native-components): Phase 6 — public pulp_node_v1 C ABI ([#3372](https://github.com/danielraffel/pulp/pull/3372))

<a id="v03250"></a>
## [0.325.0] - 2026-06-02

- feat(native-components): Phase 5 — stateful SignalGraph custom nodes + state serialization ([#3370](https://github.com/danielraffel/pulp/pull/3370))

<a id="v03240"></a>
## [0.324.0] - 2026-06-02

- feat(native-components): Phase 4 — Rust non-RT domain logic via editor_command ([#3368](https://github.com/danielraffel/pulp/pull/3368))
- feat(native-components): Phase 2b — real Rust gain DSP through the adapter ([#3365](https://github.com/danielraffel/pulp/pull/3365))

<a id="v03230"></a>
## [0.323.0] - 2026-06-02

- feat(native-components): Phase 2a — NativeCoreProcessor C-ABI↔Processor adapter ([#3364](https://github.com/danielraffel/pulp/pull/3364))
- feat(native-components): Phase 1b — opt-in Rust staticlib lane + RT-safety hook ([#3362](https://github.com/danielraffel/pulp/pull/3362))

<a id="v03220"></a>
## [0.322.0] - 2026-06-03

- feat(native-components): Phase 1a — canonical C ABI header + module + contract tests ([#3360](https://github.com/danielraffel/pulp/pull/3360))
- docs(native-components): Phase 0 — policy, boundaries, platform matrix ([#3359](https://github.com/danielraffel/pulp/pull/3359))

<a id="v03210"></a>
## [0.321.0] - 2026-06-02

- feat(import-design): recover tokens from frontmatter-less DESIGN.md bodies ([#3357](https://github.com/danielraffel/pulp/pull/3357))
- feat(figma-plugin): capture all variable modes (light/dark) as tokens ([#3354](https://github.com/danielraffel/pulp/pull/3354))
- chore: gitignore fetched Skia build artifacts + clarify diff-cover bypass knobs ([#3353](https://github.com/danielraffel/pulp/pull/3353))

<a id="v03200"></a>
## [0.320.0] - 2026-06-02

- fix(view): honor text-align in per-range styled labels + clear stale runs ([#3346](https://github.com/danielraffel/pulp/pull/3346))
- ci(release): build the release without examples so design-tool Skia gate can't block it ([#3345](https://github.com/danielraffel/pulp/pull/3345))

<a id="v03190"></a>
## [0.319.0] - 2026-06-02

- feat(ship): one-off sign+notarize via `share`, `--path` primitives, release notarizes its artifact ([#3318](https://github.com/danielraffel/pulp/pull/3318))
- docs(tart-ci): document shipping from a VM-only runner host (cloud→pool) ([#3333](https://github.com/danielraffel/pulp/pull/3333))
- fix(import-design): UTF-16 code-unit offsets for emoji in text runs ([#3337](https://github.com/danielraffel/pulp/pull/3337))
- fix(ci): unblock macOS lane (rustc gate) + isolate test git from inherited GIT_DIR ([#3335](https://github.com/danielraffel/pulp/pull/3335))

<a id="v03180"></a>
## [0.318.0] - 2026-06-02

- feat(view+import-design): render per-range text styles on the native arm ([#3336](https://github.com/danielraffel/pulp/pull/3336))

<a id="v03174"></a>
## [0.317.4] - 2026-06-01

- fix(view+import-design): map CSS radial gradient extent keywords to a radius ([#3334](https://github.com/danielraffel/pulp/pull/3334))

<a id="v03173"></a>
## [0.317.3] - 2026-06-01

- fix(view+import-design): design-import codegen/exporter correctness follow-ups ([#3332](https://github.com/danielraffel/pulp/pull/3332))
- fix(tart-ci): pre-create ccache mount dir so fresh hosts don't fake a "no IP" ([#3308](https://github.com/danielraffel/pulp/pull/3308))

<a id="v03172"></a>
## [0.317.2] - 2026-06-01

- fix(view): use a portable pi constant in design-import polygon synthesis ([#3319](https://github.com/danielraffel/pulp/pull/3319))

<a id="v03171"></a>
## [0.317.1] - 2026-06-01

- fix(view+import-design): address design-import review findings ([#3315](https://github.com/danielraffel/pulp/pull/3315))

<a id="v03170"></a>
## [0.317.0] - 2026-06-01

- feat(view+import-design): emit per-range text styles as nested spans ([#3314](https://github.com/danielraffel/pulp/pull/3314))

<a id="v03160"></a>
## [0.316.0] - 2026-06-01

- feat(view+import-design): render radial and conic background gradients ([#3313](https://github.com/danielraffel/pulp/pull/3313))

<a id="v03150"></a>
## [0.315.0] - 2026-06-01

- feat(view+import-design): lower grid containers to the native grid bridge ([#3311](https://github.com/danielraffel/pulp/pull/3311))

<a id="v03140"></a>
## [0.314.0] - 2026-06-01

- feat(view+import-design): map Figma resize constraints to flex/position ([#3310](https://github.com/danielraffel/pulp/pull/3310))

<a id="v03130"></a>
## [0.313.0] - 2026-06-01

- feat(view+import-design): synthesize SVG paths for vector shape primitives ([#3309](https://github.com/danielraffel/pulp/pull/3309))
- chore: bump planning submodule pointer ([#3307](https://github.com/danielraffel/pulp/pull/3307))

<a id="v03121"></a>
## [0.312.1] - 2026-06-01

- fix(import-design): detect_audio_widget matches whole words not substrings ([#3306](https://github.com/danielraffel/pulp/pull/3306))
- docs: codify local-dev audio etiquette in CLAUDE.md + ios/auv3 skills ([#3190](https://github.com/danielraffel/pulp/pull/3190))
- docs+tooling: CI-host onboarding page (nav) + one-command setup-ci-host.sh ([#3304](https://github.com/danielraffel/pulp/pull/3304))
- docs: opinionated runbook to set up a Mac as a Pulp Tart VM CI host ([#3303](https://github.com/danielraffel/pulp/pull/3303))

<a id="v03120"></a>
## [0.312.0] - 2026-06-01

- feat(view+import-design): preserve + emit CSS clip-path / mask ([#3288](https://github.com/danielraffel/pulp/pull/3288))
- ci(tart): persistent launchd agent for the ephemeral VM runner ([#3302](https://github.com/danielraffel/pulp/pull/3302))
- tart-ci skill + Skia/Dawn baked-cache optimization ([#3301](https://github.com/danielraffel/pulp/pull/3301))
- ci(tart): Tart golden-VM macOS lane — layered goldens, ephemeral per-job runners, vm-image manifest ([#3300](https://github.com/danielraffel/pulp/pull/3300))

<a id="v03110"></a>
## [0.311.0] - 2026-06-01

- feat(view+ios): full-width responsive iOS AUv3 editor + working host slider ([#3298](https://github.com/danielraffel/pulp/pull/3298))
- fix(import-design): coerce CSS string dimensions to floats (no degenerate dims) ([#3285](https://github.com/danielraffel/pulp/pull/3285))

<a id="v03100"></a>
## [0.310.0] - 2026-06-01

- feat(view+ios): iOS AUv3 Three.js cube demo polish — fill, scale, OrbitControls touch ([#3286](https://github.com/danielraffel/pulp/pull/3286))

<a id="v03090"></a>
## [0.309.0] - 2026-06-01

- feat(view+import-design): preserve + emit CSS mix-blend-mode ([#3284](https://github.com/danielraffel/pulp/pull/3284))

<a id="v03080"></a>
## [0.308.0] - 2026-06-01

- feat(view+import-design): lower SVG path-data vectors to a native SvgPath ([#3283](https://github.com/danielraffel/pulp/pull/3283))

<a id="v03070"></a>
## [0.307.0] - 2026-06-01

- feat(view+import-design): interactive (turnable) sprite knobs ([#3280](https://github.com/danielraffel/pulp/pull/3280))

<a id="v03060"></a>
## [0.306.0] - 2026-06-01

- fix(view): render Three.js cube on iOS AUv3 GPU path (#3217) ([#3228](https://github.com/danielraffel/pulp/pull/3228))

<a id="v03050"></a>
## [0.305.0] - 2026-06-01

- fix(view+import-design): close 5 Codex P2s on the fidelity strict-gate ([#3279](https://github.com/danielraffel/pulp/pull/3279))
- test(import): compat.json object-coverage matrix + drift guard ([#3278](https://github.com/danielraffel/pulp/pull/3278))

<a id="v03040"></a>
## [0.304.0] - 2026-06-01

- feat(view): vector-renderability fidelity invariant (dropped-vector) ([#3275](https://github.com/danielraffel/pulp/pull/3275))
- fix(ci): pin macOS toolchain to Xcode 26.5 and regenerate drifted font goldens ([#3276](https://github.com/danielraffel/pulp/pull/3276))

<a id="v03030"></a>
## [0.303.0] - 2026-05-31

- feat(view): widget-intrinsic-size + text-vertical-centering fidelity invariants ([#3274](https://github.com/danielraffel/pulp/pull/3274))
- iOS-D.3c: WebGPU writeBuffer element-vs-byte fix + inter-app-audio host entitlement (#3217) ([#3273](https://github.com/danielraffel/pulp/pull/3273))

<a id="v03020"></a>
## [0.302.0] - 2026-05-31

- refactor(view): extract design_fidelity module + golden re-import regression tool ([#3272](https://github.com/danielraffel/pulp/pull/3272))

<a id="v03010"></a>
## [0.301.0] - 2026-05-31

- feat(import-design): gross-size divergence fidelity self-check (any node, any source) ([#3268](https://github.com/danielraffel/pulp/pull/3268))

<a id="v03000"></a>
## [0.300.0] - 2026-05-31

- feat(import-design): reference-free no-skew fidelity self-check ([#3267](https://github.com/danielraffel/pulp/pull/3267))
- fix(view): center asset-bleed sprites after contain sizing ([#3264](https://github.com/danielraffel/pulp/pull/3264))
- fix(view): include asset_bleed in the bleed sprite sizing gate ([#3262](https://github.com/danielraffel/pulp/pull/3262))
- fix(view): limit aspect-preserving image sizing to bleed sprites only ([#3260](https://github.com/danielraffel/pulp/pull/3260))

<a id="v02990"></a>
## [0.299.0] - 2026-05-31

- fix(view+import-design): faithful sprite sizing, widget recognition, and layout fidelity for design imports ([#3259](https://github.com/danielraffel/pulp/pull/3259))
- fix(import-design): address Codex P2 review comments (container-guard, url-decode, image-fills, montage, doc) ([#3257](https://github.com/danielraffel/pulp/pull/3257))
- feat(import-design): REST port emits font_family_assets + sha256 content_hash ([#3256](https://github.com/danielraffel/pulp/pull/3256))

<a id="v02980"></a>
## [0.298.0] - 2026-05-31

- fix(format): address Codex review findings on the host-quirks enforcement PRs ([#3253](https://github.com/danielraffel/pulp/pull/3253))
- feat(format): default host-quirks policy to validated_only ([#3248](https://github.com/danielraffel/pulp/pull/3248))

<a id="v02970"></a>
## [0.297.0] - 2026-05-30

- feat(format): synthesize + honor a Bypass parameter in CLAP and AU v2 (P3d) ([#3246](https://github.com/danielraffel/pulp/pull/3246))
- docs(import-design): codify pulp-screenshot (decodes images) vs --validate (layout-only) ([#3230](https://github.com/danielraffel/pulp/pull/3230))
- feat(import-design): labeled comparison-montage helper (montage.py) ([#3237](https://github.com/danielraffel/pulp/pull/3237))
- feat(format): host-quirks staleness check + affected_versions catalog field (P4) ([#3245](https://github.com/danielraffel/pulp/pull/3245))
- feat(import-design): register bundled fonts from font_family_assets (#43b) ([#3241](https://github.com/danielraffel/pulp/pull/3241))

<a id="v02960"></a>
## [0.296.0] - 2026-05-30

- fix(format): harden host-quirks enforcement from the self-review sweep ([#3240](https://github.com/danielraffel/pulp/pull/3240))

<a id="v02951"></a>
## [0.295.1] - 2026-05-30

- fix(figma-plugin): #43c hardening — reject invalid font drops + diagnose orphan user fonts ([#3239](https://github.com/danielraffel/pulp/pull/3239))

<a id="v02950"></a>
## [0.295.0] - 2026-05-30

- feat(format): synthesize a Bypass parameter (VST3 + AU v3) when the plugin declares none (P3b) ([#3238](https://github.com/danielraffel/pulp/pull/3238))

<a id="v02940"></a>
## [0.294.0] - 2026-05-30

- feat(format): enforce silence_unsupported_bus_arrangements in VST3 (P3c) ([#3235](https://github.com/danielraffel/pulp/pull/3235))
- docs(figma-plugin): new headless-extraction guide ([#3236](https://github.com/danielraffel/pulp/pull/3236))
- fix(import-design): figma REST export recognizes audio-widgets + captures polygon shapes ([#3234](https://github.com/danielraffel/pulp/pull/3234))

<a id="v02931"></a>
## [0.293.1] - 2026-05-30

- refactor(figma-plugin): P2 — split pure helpers out of extract.ts → extract-pure.ts ([#3229](https://github.com/danielraffel/pulp/pull/3229))

<a id="v02930"></a>
## [0.293.0] - 2026-05-30

- feat(figma-plugin): P1 — headless extractor bundle for MCP-driven exports ([#3227](https://github.com/danielraffel/pulp/pull/3227))

<a id="v02920"></a>
## [0.292.0] - 2026-05-30

- feat(format): enforce clamp_latency_to_nonneg via resolved_quirks across all adapters (P3a) ([#3226](https://github.com/danielraffel/pulp/pull/3226))

<a id="v02910"></a>
## [0.291.0] - 2026-05-30

- feat(import-design): headless Figma REST export + --from figma auto-route guardrail (#41 follow-up) ([#3225](https://github.com/danielraffel/pulp/pull/3225))

<a id="v02900"></a>
## [0.290.0] - 2026-05-30

- refactor(design-import): structured box-shadow layers + extracted IR post-passes (#41) ([#3220](https://github.com/danielraffel/pulp/pull/3220))

<a id="v02890"></a>
## [0.289.0] - 2026-05-30

- feat(view+ios): iOS-D.3c bring-up wins — esbuild bundler, QuickJS unhandled-rejection tracker, self= global alias, AUv3 host discovery retry, IndirectFirstInstance opt-in ([#3219](https://github.com/danielraffel/pulp/pull/3219))
- feat(import-design): visual-fidelity diff harness for design imports ([#3218](https://github.com/danielraffel/pulp/pull/3218))

<a id="v02880"></a>
## [0.288.0] - 2026-05-30

- feat(design-import): native figma-plugin import fidelity to ~1:1 (#3191, #3192) ([#3208](https://github.com/danielraffel/pulp/pull/3208))

<a id="v02870"></a>
## [0.287.0] - 2026-05-30

- feat(figma-plugin): #43c — drag-drop UI for user-supplied font bundling ([#3215](https://github.com/danielraffel/pulp/pull/3215))

<a id="v02861"></a>
## [0.286.1] - 2026-05-30

- feat(figma-import): #43a-rev — emit font_family_assets metadata on envelope ([#3209](https://github.com/danielraffel/pulp/pull/3209))
- test(figma-import): kitchen-sink fixture exercising all 6 Pulp Library widgets ([#3212](https://github.com/danielraffel/pulp/pull/3212))
- docs(figma-plugin): expand widget list to all six Pulp Library v0.3.0 widgets ([#3213](https://github.com/danielraffel/pulp/pull/3213))

<a id="v02860"></a>
## [0.286.0] - 2026-05-30

- feat(format): runtime host-quirks policy gate + pulp doctor reporting (P2) ([#3207](https://github.com/danielraffel/pulp/pull/3207))

<a id="v02851"></a>
## [0.285.1] - 2026-05-30

- feat(figma-import): Phase 5 part 2 — XYPad + Waveform + Spectrum library widgets ([#3205](https://github.com/danielraffel/pulp/pull/3205))
- chore(format): add host-quirks catalog JSON + parity test (P1) ([#3198](https://github.com/danielraffel/pulp/pull/3198))

<a id="v02850"></a>
## [0.285.0] - 2026-05-30

- fix(auv3): top-align editor design viewport so it reads like CLAP/VST3 in taller host panes ([#3200](https://github.com/danielraffel/pulp/pull/3200))
- chore(figma-plugin): pin published Figma Community plugin ID ([#3192](https://github.com/danielraffel/pulp/pull/3192))

<a id="v02842"></a>
## [0.284.2] - 2026-05-30

- fix(import-design): tighten .pulp.zip zip-slip + zip-bomb guards ([#3199](https://github.com/danielraffel/pulp/pull/3199))
- docs(design-import): list figma-plugin as a --from source + adapter details ([#3197](https://github.com/danielraffel/pulp/pull/3197))
- docs(figma-plugin): direct .pulp.zip import, plugin/library URLs, fidelity note ([#3196](https://github.com/danielraffel/pulp/pull/3196))

<a id="v02841"></a>
## [0.284.1] - 2026-05-29

- fix(import-design): auto-unpack .pulp.zip exports from the Figma plugin ([#3189](https://github.com/danielraffel/pulp/pull/3189))
- test(figma-plugin): cover real emitter + align export schema with emitted shape ([#3185](https://github.com/danielraffel/pulp/pull/3185))

<a id="v02840"></a>
## [0.284.0] - 2026-05-30

- feat(design-import): wire figma-plugin parameter bindings into native widgets ([#3184](https://github.com/danielraffel/pulp/pull/3184))
- docs: add Figma → Pulp design-import model + plugin guide; refresh stale plugin README ([#3186](https://github.com/danielraffel/pulp/pull/3186))

<a id="v02831"></a>
## [0.283.1] - 2026-05-30

- feat(figma-import): Phase 5 part 1 — Pulp / Fader + Pulp / Meter library widgets ([#3180](https://github.com/danielraffel/pulp/pull/3180))
- fix(auv3): defer editor GPU-host creation until host-settled size (Logic first paint) ([#3165](https://github.com/danielraffel/pulp/pull/3165))

<a id="v02830"></a>
## [0.283.0] - 2026-05-29

- feat(examples): iOS-D.3c — Three.js rotating cube AUv3 demo (iPad-ready) ([#3171](https://github.com/danielraffel/pulp/pull/3171))

<a id="v02822"></a>
## [0.282.2] - 2026-05-30

- feat(figma-import): Phase 3 — Pulp library component recognition + Knob library key wired ([#3177](https://github.com/danielraffel/pulp/pull/3177))
- feat(import-validation): shared source-contract-v0 schema + conformance fixture (#3116) ([#3174](https://github.com/danielraffel/pulp/pull/3174))
- refactor(frontend-ir): model one-to-many native bindings explicitly (#3125) ([#3172](https://github.com/danielraffel/pulp/pull/3172))
- refactor(frontend-ir): modularize evidence assembly out of frontend_ir_report (#3119) ([#3170](https://github.com/danielraffel/pulp/pull/3170))

<a id="v02821"></a>
## [0.282.1] - 2026-05-29

- refactor(view): extract layout/snapshot diagnostic helpers into import_validation_bridge (#3151) ([#3164](https://github.com/danielraffel/pulp/pull/3164))

<a id="v02820"></a>
## [0.282.0] - 2026-05-29

- feat(ios): iOS-D.3b Slice 6 — program closeout + iOS bundle path fix ([#3163](https://github.com/danielraffel/pulp/pull/3163))

<a id="v02810"></a>
## [0.281.0] - 2026-05-29

- feat(view): iOS-D.3b Slice 5 — queue.submit log marker + engine-agnostic input contract pin ([#3159](https://github.com/danielraffel/pulp/pull/3159))
- fix(ci): provision prebuilt Skia in CI macOS setup (unblock shipyard mac lane) ([#3160](https://github.com/danielraffel/pulp/pull/3160))

<a id="v02800"></a>
## [0.280.0] - 2026-05-29

- feat(view): iOS-D.3b Slice 4 — surface presentable flag through canvas.getContext('webgpu') paths ([#3157](https://github.com/danielraffel/pulp/pull/3157))

<a id="v02790"></a>
## [0.279.0] - 2026-05-29

- feat(view+ios): iOS-D.3b Slice 3 — wire Three.js IIFE bundler into .appex build + bundler smoke test ([#3156](https://github.com/danielraffel/pulp/pull/3156))
- feat(view+ios): iOS-D.3b Slice 2 — Three.js IIFE bundle for JSC + evaluate() load + import shim ([#3154](https://github.com/danielraffel/pulp/pull/3154))

<a id="v02780"></a>
## [0.278.0] - 2026-05-29

- feat(figma-import): plugin + lane + visual-fidelity hardening + silver knob ([#3138](https://github.com/danielraffel/pulp/pull/3138))
- docs(ci): fix merge_commit_sha verification advice (Codex P2 on #3140) ([#3149](https://github.com/danielraffel/pulp/pull/3149))
- refactor(frontend-ir): explicit binary_dependencies provenance via claims[] ([#3152](https://github.com/danielraffel/pulp/pull/3152))
- test(audio): json-escape native path strings injected into JSON fixtures ([#3150](https://github.com/danielraffel/pulp/pull/3150))

<a id="v02770"></a>
## [0.277.0] - 2026-05-29

- feat(view+format): GpuSurface late-attach into WidgetBridge from plugin hosts (iOS-D.3b Slice 1) ([#3146](https://github.com/danielraffel/pulp/pull/3146))
- fix(ios-host): pair editor observer registration with dismantle cleanup ([#3139](https://github.com/danielraffel/pulp/pull/3139))
- refactor(view): single ResolvedBindingPlan for codegen binding outputs ([#3145](https://github.com/danielraffel/pulp/pull/3145))
- refactor(view): typed NativeBindingMetadata for the pulp* binding contract ([#3144](https://github.com/danielraffel/pulp/pull/3144))
- refactor(frontend-ir): schema-name registry + gate trust-boundary docstrings ([#3143](https://github.com/danielraffel/pulp/pull/3143))

<a id="v02760"></a>
## [0.276.0] - 2026-05-29

- refactor(view): split design_import.hpp into focused headers behind umbrella ([#3142](https://github.com/danielraffel/pulp/pull/3142))
- refactor(view): split design_import.cpp JSON ser/de into design_ir_json.cpp ([#3141](https://github.com/danielraffel/pulp/pull/3141))
- docs(ci): guard against stale-SHA merge race in shipyard ship ([#3140](https://github.com/danielraffel/pulp/pull/3140))

<a id="v02750"></a>
## [0.275.0] - 2026-05-29

- docs(skills): reword native-import gotchas to stable rationale ([#3136](https://github.com/danielraffel/pulp/pull/3136))
- fix(audio): reject `..` segments in hf:// checkpoint paths + Windows-safe model.json assertions ([#3131](https://github.com/danielraffel/pulp/pull/3131))

<a id="v02740"></a>
## [0.274.0] - 2026-05-29

- feat(frontend-ir): Phase 0/1 contract surface + native-readiness gate composition ([#3128](https://github.com/danielraffel/pulp/pull/3128))
- fix(scan-worker): preserve symlink-alias identity in bundle filtering ([#3132](https://github.com/danielraffel/pulp/pull/3132))

<a id="v02730"></a>
## [0.273.0] - 2026-05-29

- test(tools): cover requested MCP and audio edges ([#3124](https://github.com/danielraffel/pulp/pull/3124))
- test(cli): cover inspect command contracts ([#3121](https://github.com/danielraffel/pulp/pull/3121))
- test(inspect): cover motion scrubber edge contracts ([#3120](https://github.com/danielraffel/pulp/pull/3120))
- test(tools): expand requested coverage contracts ([#3118](https://github.com/danielraffel/pulp/pull/3118))
- test(tools): cover targeted inspect design audio contracts ([#3113](https://github.com/danielraffel/pulp/pull/3113))
- test(ship): cover platform signing package contracts ([#3112](https://github.com/danielraffel/pulp/pull/3112))

<a id="v02721"></a>
## [0.272.1] - 2026-05-28

- fix(auv3): open fixed-design editors at design size ([#3109](https://github.com/danielraffel/pulp/pull/3109))
- test(scan-worker): cover path normalization contracts ([#3110](https://github.com/danielraffel/pulp/pull/3110))
- test(audio): cover excerpt bundle edge contracts ([#3111](https://github.com/danielraffel/pulp/pull/3111))
- test(cli): cover sdk status contracts ([#3108](https://github.com/danielraffel/pulp/pull/3108))
- test(tools): cover inspect mcp screenshot design contracts ([#3107](https://github.com/danielraffel/pulp/pull/3107))
- test(tools): cover audio linux package contracts ([#3105](https://github.com/danielraffel/pulp/pull/3105))
- test(platform): cover bridge signing contracts ([#3104](https://github.com/danielraffel/pulp/pull/3104))
- test(tools): cover import scan contracts ([#3103](https://github.com/danielraffel/pulp/pull/3103))

<a id="v02720"></a>
## [0.272.0] - 2026-05-28

- feat(ios): Phase iOS-D.1 — minimal 2D GPU AUv3 proof + FindSkia iOS framework split + GPU-required hard-fail ([#3100](https://github.com/danielraffel/pulp/pull/3100))
- test(tools): cover requested debug surfaces ([#3102](https://github.com/danielraffel/pulp/pull/3102))
- fix(ios): AUv3 audio path — MH_EXECUTE binary, .loadOutOfProcess host load, MIDI event wiring ([#3095](https://github.com/danielraffel/pulp/pull/3095))
- test(python): cover binding export contracts ([#3099](https://github.com/danielraffel/pulp/pull/3099))
- test(coverage): cover requested surfaces batch ([#3096](https://github.com/danielraffel/pulp/pull/3096))

<a id="v02710"></a>
## [0.271.0] - 2026-05-28

- test(inspect): cover inspector public contracts ([#3094](https://github.com/danielraffel/pulp/pull/3094))
- test(coverage): cover requested tool contracts ([#3093](https://github.com/danielraffel/pulp/pull/3093))
- test(coverage): cover requested Swift and Python binding gaps ([#3092](https://github.com/danielraffel/pulp/pull/3092))
- test(coverage): cover requested tool and platform edges ([#3084](https://github.com/danielraffel/pulp/pull/3084))

<a id="v02704"></a>
## [0.270.4] - 2026-05-27

- fix(view): unblock full iOS Simulator build of pulp-view-* lib chain + flip smoke to true build ([#3088](https://github.com/danielraffel/pulp/pull/3088))
- feature/coverage mcp requested 20260527 ([#3083](https://github.com/danielraffel/pulp/pull/3083))
- test: cover requested packaging and import surfaces ([#3082](https://github.com/danielraffel/pulp/pull/3082))
- test(inspect): cover overlay color hex formatting ([#3075](https://github.com/danielraffel/pulp/pull/3075))
- test(coverage): cover bridge and python binding edges ([#3073](https://github.com/danielraffel/pulp/pull/3073))

<a id="v02702"></a>
## [0.270.2] - 2026-05-27

- feat(ios): scaffold AUv3 HostApp via pulp_add_ios_host_app helper (Phase iOS-B) ([#3059](https://github.com/danielraffel/pulp/pull/3059))
- test(screenshot): cover unknown backend rejection ([#3067](https://github.com/danielraffel/pulp/pull/3067))
- test(import-design): cover benchmark lane contracts ([#3065](https://github.com/danielraffel/pulp/pull/3065))

<a id="v02701"></a>
## [0.270.1] - 2026-05-26

- feat(format): 4 iPlug2-audit DAW quirks (REAPER AUv3, Studio One, DP, Cubase 13+) ([#3053](https://github.com/danielraffel/pulp/pull/3053))
- test(import-design): cover detector edge contracts ([#3063](https://github.com/danielraffel/pulp/pull/3063))
- test(cli): cover audio command contracts ([#3048](https://github.com/danielraffel/pulp/pull/3048))

<a id="v02700"></a>
## [0.270.0] - 2026-05-26

- feat(ship): wire App Store Connect API key flow into pulp ship notarize ([#3054](https://github.com/danielraffel/pulp/pull/3054))

<a id="v02691"></a>
## [0.269.1] - 2026-05-27

- feat(core): batch-ship 5 gap-doc tail items — URL/MAC/Result/OSC-colour/StateTree-clone ([#3049](https://github.com/danielraffel/pulp/pull/3049))

<a id="v02681"></a>
## [0.268.1] - 2026-05-26

- fix(skia): correct linux-arm64 Skia SHA-256 (#3036 follow-up) ([#3041](https://github.com/danielraffel/pulp/pull/3041))
- docs: surface community help-wanted issues in README + CONTRIBUTING ([#3043](https://github.com/danielraffel/pulp/pull/3043))
- ci(skia): backfill real SHA-256 for linux-arm64 Skia asset ([#3036](https://github.com/danielraffel/pulp/pull/3036))

<a id="v02680"></a>
## [0.268.0] - 2026-05-26

- ci(skia): publish linux-arm64 Skia release asset (#47) ([#3030](https://github.com/danielraffel/pulp/pull/3030))

<a id="v02660"></a>
## [0.266.0] - 2026-05-26

- feat(view): CodeEditor tokenizers + PropertyPanel variants + BubbleMessage + styled AlertWindow ([#3026](https://github.com/danielraffel/pulp/pull/3026))
- test(inspect): cover domain helper contracts ([#3034](https://github.com/danielraffel/pulp/pull/3034))

<a id="v02641"></a>
## [0.264.1] - 2026-05-26

- fix: sweep Codex P1+P2 review comments from /goal-run PRs #2985-#3022 ([#3032](https://github.com/danielraffel/pulp/pull/3032))

<a id="v02640"></a>
## [0.264.0] - 2026-05-26

- test(cli): run ship shellouts against C++ delegate ([#3023](https://github.com/danielraffel/pulp/pull/3023))

<a id="v02630"></a>
## [0.263.0] - 2026-05-26

- feat(view): RelativePoint / RelativeExpression layout primitive ([#3025](https://github.com/danielraffel/pulp/pull/3025))

<a id="v02620"></a>
## [0.262.0] - 2026-05-26

- feat(events): IapClient scaffold + JSFX @gfx boundary fix ([#3022](https://github.com/danielraffel/pulp/pull/3022))

<a id="v02610"></a>
## [0.261.0] - 2026-05-26

- feat(signal): multi-backend FFT facade (vdsp / kissfft / fftw3 / mkl) ([#3021](https://github.com/danielraffel/pulp/pull/3021))

<a id="v02600"></a>
## [0.260.0] - 2026-05-26

- feat: WebView2/WebKitGTK runtime probes + PushNotifications local-only scaffold ([#3016](https://github.com/danielraffel/pulp/pull/3016))

<a id="v02590"></a>
## [0.259.0] - 2026-05-26

- feat(ios): wire iOS Skia release assets for Phase iOS-D scaffold ([#3011](https://github.com/danielraffel/pulp/pull/3011))
- test(cli): cover ship command contracts ([#3018](https://github.com/danielraffel/pulp/pull/3018))
- test(host): developer-supplied lane for auth-gated real-plugin fixtures (item 4.2) ([#3015](https://github.com/danielraffel/pulp/pull/3015))

<a id="v02580"></a>
## [0.258.0] - 2026-05-26

- feat: BLE MIDI (CoreBluetooth) + GIF/TIFF image codecs ([#3017](https://github.com/danielraffel/pulp/pull/3017))

<a id="v02570"></a>
## [0.257.0] - 2026-05-26

- feat(runtime): add AbstractFifo + FileSearchPath ([#2985](https://github.com/danielraffel/pulp/pull/2985))
- feat(events): Linux Avahi + Windows Bonjour NSD backends (closes #2990 follow-up) ([#3003](https://github.com/danielraffel/pulp/pull/3003))
- test(cli): cover version command contracts ([#2998](https://github.com/danielraffel/pulp/pull/2998))
- fix(audio): shared token-id counter for MIDI subscription hub (closes #2976 P1 batch 2) ([#3001](https://github.com/danielraffel/pulp/pull/3001))
- feat(macos-plan): items 4.4 + 6.4b + 6.5 — HostedEditor migration + plugin-mode MainThreadDispatcher + DocumentWindow/DialogWindow/AlertWindow ([#3006](https://github.com/danielraffel/pulp/pull/3006))
- feat(view): add PreferencesPanel + TooltipWindow + ComponentDragger widgets (gap-doc P2/P3) ([#2987](https://github.com/danielraffel/pulp/pull/2987))

<a id="v02560"></a>
## [0.256.0] - 2026-05-26

- feat(cli): mac runtime validators for chainer plan Phase 5 ([#3005](https://github.com/danielraffel/pulp/pull/3005))

<a id="v02553"></a>
## [0.255.3] - 2026-05-26

- docs(state+audio): pin StateTree thread-safety + WASAPI share-mode coverage (gap-doc Phase 0) ([#3004](https://github.com/danielraffel/pulp/pull/3004))
- ci(create): add template integrity smoke for `pulp create` materializer ([#3002](https://github.com/danielraffel/pulp/pull/3002))
- fix(test): avoid M_PI in signal tests ([#2986](https://github.com/danielraffel/pulp/pull/2986))

<a id="v02552"></a>
## [0.255.2] - 2026-05-26

- test(mcp): cover command contract edges ([#2995](https://github.com/danielraffel/pulp/pull/2995))
- fix(ios+cmake): unblock iOS Simulator AUv3 build (Phase iOS-A) ([#2994](https://github.com/danielraffel/pulp/pull/2994))
- test(design): cover debug helper edge contracts ([#2983](https://github.com/danielraffel/pulp/pull/2983))
- Fix macOS WebGPU runtime rpath ([#2981](https://github.com/danielraffel/pulp/pull/2981))

<a id="v02551"></a>
## [0.255.1] - 2026-05-26

- fix: bundle 8 P2 Codex findings from #2976 (auv3 bundle path, coreaudio wg, vst3 59.94, clap lifecycle, ship, audio cache, midi-ci, screenshot) ([#2991](https://github.com/danielraffel/pulp/pull/2991))

<a id="v02550"></a>
## [0.255.0] - 2026-05-26

- feat(events): Bonjour backend + TXT records + RAII NSD wrappers (gap-doc #NSD) ([#2990](https://github.com/danielraffel/pulp/pull/2990))
- fix: bundle 5 P1 Codex findings from #2976 (host quirks, audit, midi-ci, elliptic, thumbnail) ([#2984](https://github.com/danielraffel/pulp/pull/2984))

<a id="v02541"></a>
## [0.254.1] - 2026-05-26

- fix(sdk): install au_view_controller_mac.mm for macOS AUv3 framework builds ([#2982](https://github.com/danielraffel/pulp/pull/2982))
- feat(macos-cross): upstream Linux-hosted macOS arm64 cross lane scaffold ([#2980](https://github.com/danielraffel/pulp/pull/2980))
- docs(skills): sweep auv2/auv3/clap/vst3/hosting/cli-maintenance for /goal-run PR coverage ([#2979](https://github.com/danielraffel/pulp/pull/2979))
- test(cli): cover docs command contracts ([#2921](https://github.com/danielraffel/pulp/pull/2921))

<a id="v02540"></a>
## [0.254.0] - 2026-05-26

- feat: DAW-bench harness package for graduating quirk tiers ([#2971](https://github.com/danielraffel/pulp/pull/2971))

<a id="v02530"></a>
## [0.253.0] - 2026-05-26

- test(scan-worker): cover JSON worker edge contracts ([#2899](https://github.com/danielraffel/pulp/pull/2899))
- fix(audit): allow Pulp Pro Tools host quirk header ([#2939](https://github.com/danielraffel/pulp/pull/2939))

<a id="v02520"></a>
## [0.252.0] - 2026-05-26

- feat(audio): AudioWorkgroup wiring + AudioDeviceManager lifecycle (items 1.1 + 1.2b) ([#2970](https://github.com/danielraffel/pulp/pull/2970))
- test(import-design): cover detector edge contracts ([#2913](https://github.com/danielraffel/pulp/pull/2913))
- fix(test) + feat(cli): pin real-plugin sha256s (item 4.2) + add ship auv3-xcodeproj one-click flow (item 3.10) ([#2969](https://github.com/danielraffel/pulp/pull/2969))
- feat(format): AU v3 per-method audit + AUHostingService host detection (item 3.1) ([#2967](https://github.com/danielraffel/pulp/pull/2967))

<a id="v02500"></a>
## [0.250.0] - 2026-05-26

- feat(audio,midi): close 6.12 thumbnail disk-persist + 8.4 MIDI CI RT-safety annotations ([#2966](https://github.com/danielraffel/pulp/pull/2966))
- test(cli): cover ship command edge contracts ([#2962](https://github.com/danielraffel/pulp/pull/2962))
- feat(signal): elliptic IIR + non-uniform partitioned convolver (closes deferred 2.1 + 2.3 Slice B) ([#2964](https://github.com/danielraffel/pulp/pull/2964))

<a id="v02490"></a>
## [0.249.0] - 2026-05-26

- feat(format): wire item-1.3 AudioPlayHead transport extensions into every adapter ([#2963](https://github.com/danielraffel/pulp/pull/2963))
- fix(screenshot): preserve help option short-circuit ([#2957](https://github.com/danielraffel/pulp/pull/2957))
- feat(signal+midi): close 3 deferred follow-ups — Oversampler polyphase IIR + PE zlib + PE Subscribe/Notify ([#2959](https://github.com/danielraffel/pulp/pull/2959))

<a id="v02480"></a>
## [0.248.0] - 2026-05-26

- feat(signal): elliptic/Jacobi special functions for filter design ([#2958](https://github.com/danielraffel/pulp/pull/2958))
- test(screenshot): cover cli option contracts ([#2956](https://github.com/danielraffel/pulp/pull/2956))
- test(ship): cover linux packaging edge contracts ([#2955](https://github.com/danielraffel/pulp/pull/2955))
- test(apple): cover bridge edge contracts ([#2954](https://github.com/danielraffel/pulp/pull/2954))
- test(inspect): cover inspector window edge contracts ([#2953](https://github.com/danielraffel/pulp/pull/2953))
- test(ship): cover mac codesign parser contracts ([#2950](https://github.com/danielraffel/pulp/pull/2950))
- test(mcp): cover import-design default status contracts ([#2855](https://github.com/danielraffel/pulp/pull/2855))
- test(audio): cover model registry edge contracts ([#2949](https://github.com/danielraffel/pulp/pull/2949))
- test(import-design): cover benchmark helper contracts ([#2948](https://github.com/danielraffel/pulp/pull/2948))
- test(audio): cover excerpt service edge contracts ([#2947](https://github.com/danielraffel/pulp/pull/2947))
- test(inspect): cover inspector server edge contracts ([#2946](https://github.com/danielraffel/pulp/pull/2946))

<a id="v02470"></a>
## [0.247.0] - 2026-05-26

- feat(format): AU v3 + VST3 adapter bypass hardening (items 3.1 + 3.2) ([#2937](https://github.com/danielraffel/pulp/pull/2937))
- feat(audio): AudioDeviceManager persistence + MIDI hub (item 1.2a) ([#2936](https://github.com/danielraffel/pulp/pull/2936))

<a id="v02460"></a>
## [0.246.0] - 2026-05-26

- feat(host): real-plugin integration test scaffold (item 4.2) ([#2935](https://github.com/danielraffel/pulp/pull/2935))

<a id="v02450"></a>
## [0.245.0] - 2026-05-26

- feat(format): bus-layout hook + precision audit + latency/tail notifications (items 3.7 + 3.8 + 3.11) ([#2934](https://github.com/danielraffel/pulp/pull/2934))

<a id="v02440"></a>
## [0.244.0] - 2026-05-26

- feat(cli): macOS notarize e2e + build --install gate + .pkg/.dmg orchestration (items 7.4 + 7.4b + 7.5) ([#2932](https://github.com/danielraffel/pulp/pull/2932))
- feat(canvas): Bidi text shaping via SheenBidi (item 6.8) ([#2931](https://github.com/danielraffel/pulp/pull/2931))
- feat(format/host-quirks): extract REAPER + Pro Tools per-host headers (items 5.8 + 5.9) ([#2930](https://github.com/danielraffel/pulp/pull/2930))

<a id="v02430"></a>
## [0.243.0] - 2026-05-26

- feat(view+host): PluginManagerPanel drag-add into SignalGraph (item 4.3) ([#2929](https://github.com/danielraffel/pulp/pull/2929))

<a id="v02420"></a>
## [0.242.0] - 2026-05-26

- feat(midi): UMP Session + Endpoint + VirtualEndpoint (item 8.1) ([#2916](https://github.com/danielraffel/pulp/pull/2916))
- feat(view): CommandRegistry + KeyMappingEditor (item 6.4, Pulp-native) ([#2902](https://github.com/danielraffel/pulp/pull/2902))

<a id="v02410"></a>
## [0.241.0] - 2026-05-25

- feat(audio): WAV metadata round-trip (BWAV/iXML/ASWG/ACID) — item 6.11 ([#2901](https://github.com/danielraffel/pulp/pull/2901))
- test(view+render): multi-window rendering acceptance proof (item 6.6) ([#2915](https://github.com/danielraffel/pulp/pull/2915))

<a id="v02400"></a>
## [0.240.0] - 2026-05-25

- feat(audio): AudioThumbnail + cache wired into WaveformView (item 6.12) ([#2898](https://github.com/danielraffel/pulp/pull/2898))

<a id="v02390"></a>
## [0.239.0] - 2026-05-25

- feat(format): 6 iPlug2-audit host-quirks lessons (Ardour/Mixbus, Wavelab IBStream, Live 10.1.13, Reaper kb, Pro Tools AAX) ([#2908](https://github.com/danielraffel/pulp/pull/2908))
- feat(render): HeadlessSurface CI wrapper for offscreen Dawn/Skia (item 6.7) ([#2914](https://github.com/danielraffel/pulp/pull/2914))
- feat(host): crash-isolated plugin scanner (item 4.1) ([#2906](https://github.com/danielraffel/pulp/pull/2906))

<a id="v02380"></a>
## [0.238.0] - 2026-05-25

- feat(format): per-quirk validation tiers + opt-in/opt-out filter (item 5.15) ([#2909](https://github.com/danielraffel/pulp/pull/2909))
- feat(view): standalone ScrollBar widget + slide-in SidePanel (item 6.3) ([#2900](https://github.com/danielraffel/pulp/pull/2900))

<a id="v02370"></a>
## [0.237.0] - 2026-05-25

- feat(midi): MIDI-CI Property Exchange spec compliance (item 8.4) ([#2905](https://github.com/danielraffel/pulp/pull/2905))
- feat(events): add native main-thread dispatcher ([#2825](https://github.com/danielraffel/pulp/pull/2825))

<a id="v02350"></a>
## [0.235.0] - 2026-05-25

- feat(format): host quirks — Cubase 10/9, Live, Wavelab (items 5.2/5.3/5.4/5.6) ([#2893](https://github.com/danielraffel/pulp/pull/2893))

<a id="v02340"></a>
## [0.234.0] - 2026-05-25

- feat(host): ExtensionsVisitor + SignalGraph automation slew + sidechain (items 4.5/4.6/4.7) ([#2892](https://github.com/danielraffel/pulp/pull/2892))
- feat(midi): extract shared UmpSysex7Reassembler from AUv3 + CoreMIDI (item 8.2) ([#2891](https://github.com/danielraffel/pulp/pull/2891))
- fix(signal): ConvolverIrSwapper retired ring keeps frees off audio thread (Codex P1 on #2881) ([#2890](https://github.com/danielraffel/pulp/pull/2890))
- feat(format): extend ProcessContext with AudioPlayHead transport surface (item 1.3 struct-only slice) ([#2883](https://github.com/danielraffel/pulp/pull/2883))

<a id="v02300"></a>
## [0.230.0] - 2026-05-25

- feat(signal): polyphase IIR half-band filter for 2x oversampling (item 2.2) ([#2885](https://github.com/danielraffel/pulp/pull/2885))
- feat(view): FileChooser builder wraps native FileDialog (macOS plan 6.2) ([#2884](https://github.com/danielraffel/pulp/pull/2884))

<a id="v02280"></a>
## [0.228.0] - 2026-05-25

- feat(cli): pulp identity record + build --check-identity (item 3.12) ([#2882](https://github.com/danielraffel/pulp/pull/2882))
- feat(signal): Convolver background IR swap (item 2.3 Slice A) ([#2881](https://github.com/danielraffel/pulp/pull/2881))
- feat(signal): Chebyshev I/II IIR filter design via biquad cascades (item 2.1) ([#2880](https://github.com/danielraffel/pulp/pull/2880))

<a id="v02260"></a>
## [0.226.0] - 2026-05-25

- feat(signal): polyphase-FIR Resampler with Kaiser-window prototype (item 1.7) ([#2879](https://github.com/danielraffel/pulp/pull/2879))

<a id="v02250"></a>
## [0.225.0] - 2026-05-25

- feat(audio): AudioSource hierarchy — Positionable + FormatReader + Transport + Player (item 1.8) ([#2878](https://github.com/danielraffel/pulp/pull/2878))

<a id="v02241"></a>
## [0.224.1] - 2026-05-25

- fix(midi): SynthesiserVoice::on_note_off syncs note_.releasing (Codex P2 on #2870) ([#2873](https://github.com/danielraffel/pulp/pull/2873))

<a id="v02240"></a>
## [0.224.0] - 2026-05-25

- feat(audio): SampleConverter — PCM ↔ planar float across format × endian × layout (item 1.5) ([#2871](https://github.com/danielraffel/pulp/pull/2871))
- feat(midi): generic Synthesiser polyphony framework (item 2.5) ([#2870](https://github.com/danielraffel/pulp/pull/2870))

<a id="v02230"></a>
## [0.223.0] - 2026-05-25

- fix(signal): Wavetable preserves crossfade continuity + guards non-positive sample rate (Codex #2865 P1+P2) ([#2867](https://github.com/danielraffel/pulp/pull/2867))

<a id="v02221"></a>
## [0.222.1] - 2026-05-25

- fix(audio): buffer_ops::clip sanitizes NaN before clamp (Codex P1 on #2864) ([#2866](https://github.com/danielraffel/pulp/pull/2866))

<a id="v02220"></a>
## [0.222.0] - 2026-05-25

- feat(signal): Wavetable oscillator with band-switching + WavetableBank morph (item 2.6) ([#2865](https://github.com/danielraffel/pulp/pull/2865))

<a id="v02210"></a>
## [0.221.0] - 2026-05-25

- feat(midi): MpeVoiceTracker consumes UMP per-note management + assignable PNC (item 8.5b) ([#2860](https://github.com/danielraffel/pulp/pull/2860))

<a id="v02200"></a>
## [0.220.0] - 2026-05-25

- feat(audio): BufferOps SIMD-backed helpers — apply_gain / ramp / clip / min-max / magnitude (item 1.6) ([#2864](https://github.com/danielraffel/pulp/pull/2864))

<a id="v02190"></a>
## [0.219.0] - 2026-05-25

- fix(midi+test): collector continue-drain + 64-slot pending ring; audit test uses in-range port id ([#2859](https://github.com/danielraffel/pulp/pull/2859))
- test(midi): audit MIDI 1.0 backend contract on macOS / cross-platform (item 8.6) ([#2857](https://github.com/danielraffel/pulp/pull/2857))

<a id="v02180"></a>
## [0.218.0] - 2026-05-25

- fix: Codex P1/P2 round 3 — MidiMessageCollector zero-loss overflow slot + ARA SDK-ON assert non-null ([#2856](https://github.com/danielraffel/pulp/pull/2856))
- test(format): ARA scaffold validation — default-OFF defaults + SDK-ON conditional (item 3.6) ([#2854](https://github.com/danielraffel/pulp/pull/2854))

<a id="v02170"></a>
## [0.217.0] - 2026-05-25

- fix(midi): MidiMessageCollector — always drain queue + multi-slot pending ring (Codex P1 on #2845) ([#2853](https://github.com/danielraffel/pulp/pull/2853))
- test(audio): cover audio tool edge contracts ([#2842](https://github.com/danielraffel/pulp/pull/2842))

<a id="v02160"></a>
## [0.216.0] - 2026-05-25

- feat(macos-plan): Batch G (partial) — Compressor sidechain HPF + lookahead (item 2.4) ([#2846](https://github.com/danielraffel/pulp/pull/2846))

<a id="v02150"></a>
## [0.215.0] - 2026-05-25

- fix(macos-plan): Codex P1 follow-ups — HMAC mbedTLS error propagation + MidiMessageCollector SPSC-safe deferral ([#2845](https://github.com/danielraffel/pulp/pull/2845))

<a id="v02140"></a>
## [0.214.0] - 2026-05-25

- feat(macos-plan): Batch C (partial) — MidiMessageCollector SPSC UI→audio hub (item 1.9) ([#2843](https://github.com/danielraffel/pulp/pull/2843))
- test(python): cover binding API contracts ([#2837](https://github.com/danielraffel/pulp/pull/2837))

<a id="v02130"></a>
## [0.213.0] - 2026-05-25

- feat(macos-plan): Batch D (partial) — HMAC + AES-256-GCM AEAD + constant_time_equal (item 7.1a) ([#2841](https://github.com/danielraffel/pulp/pull/2841))

<a id="v02120"></a>
## [0.212.0] - 2026-05-25

- feat(macos-plan): Batch E (UMP per-note + JR Clock) + Sqrt4_5dB midpoint fix ([#2836](https://github.com/danielraffel/pulp/pull/2836))
- test(apple): cover PulpBridge edge contracts ([#2835](https://github.com/danielraffel/pulp/pull/2835))
- test(host): cover scan worker output contracts ([#2831](https://github.com/danielraffel/pulp/pull/2831))

<a id="v02110"></a>
## [0.211.0] - 2026-05-25

- feat(macos-plan): Batch B (mixers) — DryWetMixer + Panner pan-law expansion (item 2.9) ([#2834](https://github.com/danielraffel/pulp/pull/2834))

<a id="v02100"></a>
## [0.210.0] - 2026-05-25

- feat(macos-plan): Batch P — HostQuirks scaffolding + HostVersion detection + Reference-Lineage trailer doc ([#2833](https://github.com/danielraffel/pulp/pull/2833))

<a id="v02090"></a>
## [0.209.0] - 2026-05-25

- feat(macos-plan): Batch A — Foundations (DcBlocker, denormal snap, NormalisableRange, ADSR buffer-apply, SmoothedValue audit, time-window undo, HighResolutionTimer OS-queue) ([#2832](https://github.com/danielraffel/pulp/pull/2832))
- test(cli): cover run parser edge contracts ([#2830](https://github.com/danielraffel/pulp/pull/2830))
- test(apple): cover motion probe contracts ([#2829](https://github.com/danielraffel/pulp/pull/2829))
- test(import-design): cover benchmark helper contracts ([#2828](https://github.com/danielraffel/pulp/pull/2828))
- test(screenshot): cover cli helper contracts ([#2827](https://github.com/danielraffel/pulp/pull/2827))
- test(design): cover debug helper contracts ([#2826](https://github.com/danielraffel/pulp/pull/2826))
- test(ship): cover linux packaging contracts ([#2824](https://github.com/danielraffel/pulp/pull/2824))
- test(ship): cover mac codesign failure contracts ([#2823](https://github.com/danielraffel/pulp/pull/2823))
- test(cli): cover common parser contracts ([#2821](https://github.com/danielraffel/pulp/pull/2821))
- test(inspect): cover protocol JSON contracts ([#2820](https://github.com/danielraffel/pulp/pull/2820))
- test(inspect): cover inspector server contracts ([#2819](https://github.com/danielraffel/pulp/pull/2819))
- test(signal): cover biquad response contracts ([#2814](https://github.com/danielraffel/pulp/pull/2814))

<a id="v02080"></a>
## [0.208.0] - 2026-05-25

- feat(platform): consolidate child process lifecycle ([#2815](https://github.com/danielraffel/pulp/pull/2815))
- test(import-design): cover script detector contracts ([#2816](https://github.com/danielraffel/pulp/pull/2816))
- fix(runtime): serialize high-resolution timer stop ownership ([#2817](https://github.com/danielraffel/pulp/pull/2817))
- test(signal): cover noise gate guard contracts ([#2813](https://github.com/danielraffel/pulp/pull/2813))
- test(signal): cover chorus edge contracts ([#2812](https://github.com/danielraffel/pulp/pull/2812))
- test(signal): cover processor duplicator edge contracts ([#2811](https://github.com/danielraffel/pulp/pull/2811))
- test(signal): cover multi-channel meter edges ([#2809](https://github.com/danielraffel/pulp/pull/2809))
- test(midi): cover MIDI file edge cases ([#2807](https://github.com/danielraffel/pulp/pull/2807))
- test(format): cover remote view session branches ([#2806](https://github.com/danielraffel/pulp/pull/2806))
- test(state): cover undo transaction history edges ([#2805](https://github.com/danielraffel/pulp/pull/2805))
- test(canvas): cover canvas widget dispatch edges ([#2804](https://github.com/danielraffel/pulp/pull/2804))
- test(view): cover button component branches ([#2803](https://github.com/danielraffel/pulp/pull/2803))
- test(format): cover screenshot capture edge cases ([#2802](https://github.com/danielraffel/pulp/pull/2802))
- test(view): cover text overflow continuation gaps ([#2800](https://github.com/danielraffel/pulp/pull/2800))

<a id="v02070"></a>
## [0.207.0] - 2026-05-24

- test(view): cover text overflow edge cases ([#2799](https://github.com/danielraffel/pulp/pull/2799))
- test(audio): cover CoreAudio CAF reader ([#2798](https://github.com/danielraffel/pulp/pull/2798))
- test(canvas): cover recording canvas edge contracts ([#2797](https://github.com/danielraffel/pulp/pull/2797))
- test(signal): cover shaping primitives ([#2796](https://github.com/danielraffel/pulp/pull/2796))
- test(state): cover parameter cursor and edit history edges ([#2795](https://github.com/danielraffel/pulp/pull/2795))
- test(runtime): cover http network edge cases ([#2794](https://github.com/danielraffel/pulp/pull/2794))
- test(signal): cover matrix and window edge paths ([#2793](https://github.com/danielraffel/pulp/pull/2793))
- test(ship): cover appcast edge branches ([#2792](https://github.com/danielraffel/pulp/pull/2792))
- test(canvas): cover PSDF atlas lifecycle ([#2791](https://github.com/danielraffel/pulp/pull/2791))
- test(state): cover state bindings ([#2790](https://github.com/danielraffel/pulp/pull/2790))
- test(audio): cover OGG reader success paths ([#2787](https://github.com/danielraffel/pulp/pull/2787))
- test(audio): cover AIFF reader edge cases ([#2783](https://github.com/danielraffel/pulp/pull/2783))

<a id="v02060"></a>
## [0.206.0] - 2026-05-23

- feat(canvas+sdk): Skia chrome/m149 + DESIGN_WIDTH/HEIGHT auto-sizing (#2784 stage A) ([#2785](https://github.com/danielraffel/pulp/pull/2785))

<a id="v02050"></a>
## [0.205.0] - 2026-05-23

- test(render): cover atlas introspection edge cases ([#2782](https://github.com/danielraffel/pulp/pull/2782))
- test(audio): cover streaming writer lifecycle contracts ([#2776](https://github.com/danielraffel/pulp/pull/2776))
- Phase 3.5: macOS AU v3 editor support (framework + stub appex + container app) ([#2779](https://github.com/danielraffel/pulp/pull/2779))
- test(host): cover scanner metadata edge cases ([#2781](https://github.com/danielraffel/pulp/pull/2781))
- test(platform): cover child process reuse contracts ([#2775](https://github.com/danielraffel/pulp/pull/2775))
- test(runtime): cover signed license contracts ([#2772](https://github.com/danielraffel/pulp/pull/2772))
- test(runtime): cover zip inflater edge contracts ([#2771](https://github.com/danielraffel/pulp/pull/2771))
- test(audio): cover compressed registry reader contracts ([#2767](https://github.com/danielraffel/pulp/pull/2767))
- test(host): cover CLAP scanner slot contracts ([#2770](https://github.com/danielraffel/pulp/pull/2770))
- docs: codify Release as the default build type for Pulp ([#2769](https://github.com/danielraffel/pulp/pull/2769))

<a id="v02040"></a>
## [0.204.0] - 2026-05-22

- test(signal): cover FFT convolver edge contracts ([#2768](https://github.com/danielraffel/pulp/pull/2768))
- test(format): cover host type classifier contracts ([#2766](https://github.com/danielraffel/pulp/pull/2766))

<a id="v02030"></a>
## [0.203.0] - 2026-05-23

- Phase 3: editor resize negotiation (CLAP + VST3) + #2762 post-merge fixes ([#2765](https://github.com/danielraffel/pulp/pull/2765))
- test(dsl): cover Faust processor edge contracts ([#2764](https://github.com/danielraffel/pulp/pull/2764))
- test(events): cover timer and discovery edge paths ([#2763](https://github.com/danielraffel/pulp/pull/2763))

<a id="v02020"></a>
## [0.202.0] - 2026-05-22

- feat(view): port WindowHost::set_design_viewport + aspect-lock to PluginViewHost ([#2762](https://github.com/danielraffel/pulp/pull/2762))
- test(canvas): cover recording fallback edge paths ([#2761](https://github.com/danielraffel/pulp/pull/2761))
- test(render): cover CPU render edge paths ([#2760](https://github.com/danielraffel/pulp/pull/2760))
- test(state): cover properties and migration copy edges ([#2759](https://github.com/danielraffel/pulp/pull/2759))
- test(signal): cover spectral utility branch edges ([#2758](https://github.com/danielraffel/pulp/pull/2758))
- test(osc): cover UDP and channel branch edges ([#2757](https://github.com/danielraffel/pulp/pull/2757))
- test(host): cover scanner and scan cache branch edges ([#2755](https://github.com/danielraffel/pulp/pull/2755))
- test(local-ci): cover CLI scheduler branch edges ([#2754](https://github.com/danielraffel/pulp/pull/2754))
- docs(skills): cli-maintenance — shell-quote PULP_BUILD_TYPE (injection guard) ([#2753](https://github.com/danielraffel/pulp/pull/2753))

<a id="v02010"></a>
## [0.201.0] - 2026-05-22

- feat(inspect+view): WYSIWYG in-canvas inspector — move/resize/reflow, tool palette + inline text editing, GPU render time, lifecycle hardening ([#2702](https://github.com/danielraffel/pulp/pull/2702))
- test(local-ci): cover desktop artifact helper edges ([#2752](https://github.com/danielraffel/pulp/pull/2752))
- test(local-ci): cover cloud helper edge paths ([#2750](https://github.com/danielraffel/pulp/pull/2750))

<a id="v02000"></a>
## [0.200.0] - 2026-05-22

- docs(skills): cli-maintenance — repo/example builds default to Release + build-type flag ([#2749](https://github.com/danielraffel/pulp/pull/2749))
- test(local-ci): cover bundle upload probe edges ([#2748](https://github.com/danielraffel/pulp/pull/2748))
- test(import-validation): cover source contract helper edges ([#2747](https://github.com/danielraffel/pulp/pull/2747))
- test(headers): cover self-contained header checker ([#2746](https://github.com/danielraffel/pulp/pull/2746))
- ci: header self-containment gate + watchdog reaper (#2576 prevention) ([#2745](https://github.com/danielraffel/pulp/pull/2745))
- test(import-validation): cover region diff CLI edges ([#2741](https://github.com/danielraffel/pulp/pull/2741))

<a id="v01990"></a>
## [0.199.0] - 2026-05-22

- docs(skills): AU v2 Cocoa view advertisement + embedded-view input gotchas ([#2740](https://github.com/danielraffel/pulp/pull/2740))
- test(local-ci): cover desktop doctor edge paths ([#2739](https://github.com/danielraffel/pulp/pull/2739))
- test(coverage): omit CI bootstrap tests from Python surface ([#2735](https://github.com/danielraffel/pulp/pull/2735))
- test(local-ci): cover desktop command edge paths ([#2734](https://github.com/danielraffel/pulp/pull/2734))
- test(local-ci): cover queue log command edges ([#2733](https://github.com/danielraffel/pulp/pull/2733))
- test(local-ci): cover command submission paths ([#2732](https://github.com/danielraffel/pulp/pull/2732))
- test(local-ci): cover scheduler execution edges ([#2731](https://github.com/danielraffel/pulp/pull/2731))
- test(local-ci): cover exact sha source helpers ([#2730](https://github.com/danielraffel/pulp/pull/2730))
- test(local-ci): cover desktop helper edges ([#2729](https://github.com/danielraffel/pulp/pull/2729))
- test(harness): cover evidence validator refs ([#2728](https://github.com/danielraffel/pulp/pull/2728))
- test(tools): cover motion visual analyzer helpers ([#2727](https://github.com/danielraffel/pulp/pull/2727))
- test(tools): cover local ci core helper branches ([#2726](https://github.com/danielraffel/pulp/pull/2726))
- test(tools): cover local ci cloud cli helpers ([#2725](https://github.com/danielraffel/pulp/pull/2725))
- test(tools): cover Android local ci target helpers ([#2724](https://github.com/danielraffel/pulp/pull/2724))
- test(tools): cover local ci helper modules ([#2723](https://github.com/danielraffel/pulp/pull/2723))
- test(tools): cover local ci desktop helpers ([#2722](https://github.com/danielraffel/pulp/pull/2722))
- test(tools): refresh embed js generator coverage ([#2721](https://github.com/danielraffel/pulp/pull/2721))
- test(tools): cover Yoga harness adapter classifier ([#2720](https://github.com/danielraffel/pulp/pull/2720))
- test(tools): cover RN harness adapter classifier ([#2718](https://github.com/danielraffel/pulp/pull/2718))
- test(tools): cover harness verifier orchestration ([#2714](https://github.com/danielraffel/pulp/pull/2714))
- test(tools): cover import reference diff helpers ([#2713](https://github.com/danielraffel/pulp/pull/2713))

<a id="v01980"></a>
## [0.198.0] - 2026-05-22

- docs(skills): rewrite issue/PR cite-phrases as stable rationale (docs-noise-lint) ([#2712](https://github.com/danielraffel/pulp/pull/2712))
- test(tools): cover motion visual capture helpers ([#2711](https://github.com/danielraffel/pulp/pull/2711))

<a id="v01974"></a>
## [0.197.4] - 2026-05-22

- fix(view,host): honor 0% dimensions, fix grid-template:none throw, cap CLAP scan count (#2703 #2704) ([#2710](https://github.com/danielraffel/pulp/pull/2710))
- test(tools): cover motion visual analyzer helpers ([#2709](https://github.com/danielraffel/pulp/pull/2709))

<a id="v01973"></a>
## [0.197.3] - 2026-05-22

- fix(ship): allow bare appcast output paths ([#2705](https://github.com/danielraffel/pulp/pull/2705))
- ci: add release-dry-run workflow to catch release-chain breakage pre-tag (P9-2, #2576) ([#2701](https://github.com/danielraffel/pulp/pull/2701))
- test(canvas): cover text layout edge cases ([#2700](https://github.com/danielraffel/pulp/pull/2700))

<a id="v01971"></a>
## [0.197.1] - 2026-05-22

- fix(audio,signal,clap): harden 4 confirmed edge-case defects from the P7 audit ([#2697](https://github.com/danielraffel/pulp/pull/2697))

<a id="v01970"></a>
## [0.197.0] - 2026-05-22

- fix(import-design): add default preference overrides ([#2696](https://github.com/danielraffel/pulp/pull/2696))
- test(canvas): cover color conversion edge cases ([#2689](https://github.com/danielraffel/pulp/pull/2689))
- test(format): cover CLAP adapter edge cases ([#2684](https://github.com/danielraffel/pulp/pull/2684))
- test(host): cover signal graph edge cases ([#2688](https://github.com/danielraffel/pulp/pull/2688))

<a id="v01960"></a>
## [0.196.0] - 2026-05-21

- fix(import-design): close review followups ([#2685](https://github.com/danielraffel/pulp/pull/2685))
- test(format): cover CLAP entry extensions ([#2683](https://github.com/danielraffel/pulp/pull/2683))
- test(host): cover graph serializer edge cases ([#2680](https://github.com/danielraffel/pulp/pull/2680))
- test(audio): cover core helper edge cases ([#2669](https://github.com/danielraffel/pulp/pull/2669))

<a id="v01953"></a>
## [0.195.3] - 2026-05-21

- refactor(inspect): split InspectorOverlay paint methods into a sibling TU (P11-5, #2647) ([#2679](https://github.com/danielraffel/pulp/pull/2679))
- test(format): cover validation harness report edges ([#2678](https://github.com/danielraffel/pulp/pull/2678))

<a id="v01952"></a>
## [0.195.2] - 2026-05-21

- refactor(test): split SignalGraph tests out of test_host.cpp (P11-5, #2647) ([#2677](https://github.com/danielraffel/pulp/pull/2677))

<a id="v01951"></a>
## [0.195.1] - 2026-05-21

- refactor(local-ci): extract cloud provider integration into cloud.py (R2-1, #2645) ([#2675](https://github.com/danielraffel/pulp/pull/2675))
- test(format): cover plugin state migration edges ([#2674](https://github.com/danielraffel/pulp/pull/2674))
- test(osc): cover lifecycle and pattern edges ([#2673](https://github.com/danielraffel/pulp/pull/2673))

<a id="v01941"></a>
## [0.194.1] - 2026-05-21

- refactor(test): split test_inspector.cpp by concern (P11-5, #2647) ([#2666](https://github.com/danielraffel/pulp/pull/2666))

<a id="v01940"></a>
## [0.194.0] - 2026-05-21

- feat(import): complete design import cli wiring ([#2665](https://github.com/danielraffel/pulp/pull/2665))

<a id="v01931"></a>
## [0.193.1] - 2026-05-21

- refactor(view): embed JS preludes as an extern-linked TU (P8-5, #2641) ([#2659](https://github.com/danielraffel/pulp/pull/2659))
- feat(examples): proportional resize for imported designs in ui-preview ([#2660](https://github.com/danielraffel/pulp/pull/2660))
- fix(signal): guard coverage edge helpers ([#2657](https://github.com/danielraffel/pulp/pull/2657))
- feat(import): add baked C++ exporter ([#2656](https://github.com/danielraffel/pulp/pull/2656))

<a id="v01930"></a>
## [0.193.0] - 2026-05-21

- feat(render): GPU render time via Skia Graphite GpuStats (#2611) ([#2658](https://github.com/danielraffel/pulp/pull/2658))

<a id="v01922"></a>
## [0.192.2] - 2026-05-21

- fix(cli): select build config at build time for multi-config generators ([#2655](https://github.com/danielraffel/pulp/pull/2655))
- test(import): expand design import benchmark coverage ([#2652](https://github.com/danielraffel/pulp/pull/2652))
- test(import): add screenshot parity oracle ([#2651](https://github.com/danielraffel/pulp/pull/2651))

<a id="v01921"></a>
## [0.192.1] - 2026-05-21

- fix(runtime): close timer and license review gaps ([#2649](https://github.com/danielraffel/pulp/pull/2649))
- feat(import): add design import benchmark harness ([#2640](https://github.com/danielraffel/pulp/pull/2640))

<a id="v01920"></a>
## [0.192.0] - 2026-05-21

- test: expand phase3 coverage across core subsystems ([#2639](https://github.com/danielraffel/pulp/pull/2639))

<a id="v01910"></a>
## [0.191.0] - 2026-05-21

- feat(view): add baked native materializer ([#2636](https://github.com/danielraffel/pulp/pull/2636))
- test(audio,midi,runtime): expand phase3 coverage batch ([#2634](https://github.com/danielraffel/pulp/pull/2634))
- test(runtime): expand phase3 runtime coverage batch ([#2635](https://github.com/danielraffel/pulp/pull/2635))

<a id="v01900"></a>
## [0.190.0] - 2026-05-21

- feat(view): add design import native resolver ([#2630](https://github.com/danielraffel/pulp/pull/2630))
- test(tools): harden harness coverage adapters ([#2633](https://github.com/danielraffel/pulp/pull/2633))
- ci: harden pages and release watchdog ([#2629](https://github.com/danielraffel/pulp/pull/2629))
- test(tools): expand phase3 coverage adapters ([#2626](https://github.com/danielraffel/pulp/pull/2626))

<a id="v01890"></a>
## [0.189.0] - 2026-05-21

- feat(view): add layout tree parity oracle ([#2627](https://github.com/danielraffel/pulp/pull/2627))

<a id="v01880"></a>
## [0.188.0] - 2026-05-21

- fix(import): normalize design ir diagnostics ([#2610](https://github.com/danielraffel/pulp/pull/2610))
- test(tools): expand phase3 coverage helpers ([#2618](https://github.com/danielraffel/pulp/pull/2618))
- ci: skip Android Kotlin coverage on PRs ([#2603](https://github.com/danielraffel/pulp/pull/2603))

<a id="v01870"></a>
## [0.187.0] - 2026-05-21

- feat(render+view): VBlank-locked safe-repaint pattern across all platforms ([#2580](https://github.com/danielraffel/pulp/pull/2580))
- test(tools): cover version gate guard edges ([#2602](https://github.com/danielraffel/pulp/pull/2602))
- fix(import): harden asset manifest refresh ([#2600](https://github.com/danielraffel/pulp/pull/2600))
- ci: keep coverage watchdog alive until legs exist ([#2597](https://github.com/danielraffel/pulp/pull/2597))
- test(format): expand host/format Phase 3 coverage ([#2594](https://github.com/danielraffel/pulp/pull/2594))

<a id="v01860"></a>
## [0.186.0] - 2026-05-21

- test(runtime): expand Phase 3 state/runtime coverage ([#2563](https://github.com/danielraffel/pulp/pull/2563))

<a id="v01850"></a>
## [0.185.0] - 2026-05-21

- feat(view): lock-to-source Path B — hand-authored JSX/TSX patch (#1308) ([#2549](https://github.com/danielraffel/pulp/pull/2549))

<a id="v01841"></a>
## [0.184.1] - 2026-05-21

- refactor(view): split web-compat-style-decl _applyProperty by domain (P5-5) ([#2525](https://github.com/danielraffel/pulp/pull/2525))
- Disable plugin editor launches in validation ([#2588](https://github.com/danielraffel/pulp/pull/2588))
- fix(midi): preserve raw parser state across chunks ([#2582](https://github.com/danielraffel/pulp/pull/2582))

<a id="v01840"></a>
## [0.184.0] - 2026-05-21

- fix(audio): harden file and buffering edge cases ([#2581](https://github.com/danielraffel/pulp/pull/2581))

<a id="v01836"></a>
## [0.183.6] - 2026-05-21

- refactor(cli): Phase 11 CLI splits — cmd_project.cpp + package_commands.cpp ([#2497](https://github.com/danielraffel/pulp/pull/2497))

<a id="v01835"></a>
## [0.183.5] - 2026-05-20

- refactor(design-tool): split design-tool.js into per-concern modules (P8-NEW) ([#2589](https://github.com/danielraffel/pulp/pull/2589))
- docs: outline modular runtime spec ([#2584](https://github.com/danielraffel/pulp/pull/2584))

<a id="v01834"></a>
## [0.183.4] - 2026-05-20

- refactor(render): split gpu_surface_android.cpp JNI bridge into sibling TU (P8-1) ([#2554](https://github.com/danielraffel/pulp/pull/2554))
- refactor: Phase 7 large-file splits (skia_canvas_filter + motion_geometry + claude_bundle_sources) ([#2474](https://github.com/danielraffel/pulp/pull/2474))
- docs: clarify experimental node ABI direction ([#2583](https://github.com/danielraffel/pulp/pull/2583))
- refactor(design-tool): wrap top-level imperative build code in ordered functions ([#2573](https://github.com/danielraffel/pulp/pull/2573))
- ci: centralize react codecov uploads on main ([#2568](https://github.com/danielraffel/pulp/pull/2568))

<a id="v01831"></a>
## [0.183.1] - 2026-05-20

- fix: address post-merge review follow-ups ([#2546](https://github.com/danielraffel/pulp/pull/2546))

<a id="v01830"></a>
## [0.183.0] - 2026-05-20

- fix(host): complete custom node registry behavior ([#2572](https://github.com/danielraffel/pulp/pull/2572))

<a id="v01820"></a>
## [0.182.0] - 2026-05-20

- feat(host): preserve custom graph node identities ([#2566](https://github.com/danielraffel/pulp/pull/2566))
- fix(render): survive GPU-backed images + write .skp atomically ([#2560](https://github.com/danielraffel/pulp/pull/2560))
- test(tools): cover coverage helper edges ([#2553](https://github.com/danielraffel/pulp/pull/2553))
- ci: document leftover CI-watch Monitor cleanup in the ci skill ([#2564](https://github.com/danielraffel/pulp/pull/2564))

<a id="v01810"></a>
## [0.181.0] - 2026-05-20

- feat(runtime): add node ABI capability gate ([#2558](https://github.com/danielraffel/pulp/pull/2558))
- feat(host): deliver audio-rate modulation edges ([#2551](https://github.com/danielraffel/pulp/pull/2551))

<a id="v01804"></a>
## [0.180.4] - 2026-05-20

- ci: fix Codex P1s on coverage matrix encoding + watchdog scope ([#2561](https://github.com/danielraffel/pulp/pull/2561))
- feat(inspect): add selection-mode toggle (FOLLOWS_MOUSE / FOLLOWS_FOCUS) ([#2556](https://github.com/danielraffel/pulp/pull/2556))
- refactor(inspect): split inspector_overlay.cpp by concern (P10-2) ([#2555](https://github.com/danielraffel/pulp/pull/2555))
- refactor(scripts): split version_bump_check.py by function cluster (P9-NEW) ([#2552](https://github.com/danielraffel/pulp/pull/2552))
- test(harness): lint visual-harness Dockerfile against canonical Skia pin (P9-4) ([#2550](https://github.com/danielraffel/pulp/pull/2550))
- feat(render): add Skia .skp frame capture (inspector Phase 6.4) ([#2548](https://github.com/danielraffel/pulp/pull/2548))
- refactor(test): split test_inspector.cpp by concern into four sibling TUs ([#2545](https://github.com/danielraffel/pulp/pull/2545))
- refactor(view): split window_host_mac geometry helpers into sibling TU (R2-5) ([#2529](https://github.com/danielraffel/pulp/pull/2529))
- test(coverage): cover phase3 runtime audio edges ([#2519](https://github.com/danielraffel/pulp/pull/2519))
- ci: fix resolve-provider busy-probe over-counting (M1 runners starved during deep queues) ([#2557](https://github.com/danielraffel/pulp/pull/2557))

<a id="v01800"></a>
## [0.180.0] - 2026-05-20

- ci: retune coverage-queue-watchdog grace 25m->150m (data: 25m false-cancelled most runs) ([#2543](https://github.com/danielraffel/pulp/pull/2543))
- fix(audio): respect BufferView slice offsets when clearing ([#2542](https://github.com/danielraffel/pulp/pull/2542))
- feat(state): add serialized state migrations ([#2537](https://github.com/danielraffel/pulp/pull/2537))
- ci: address Codex P1s on coverage + cross-platform workflows ([#2541](https://github.com/danielraffel/pulp/pull/2541))
- ci: make per-PR coverage macOS-only (Linux/Windows -> nightly lane) ([#2540](https://github.com/danielraffel/pulp/pull/2540))

<a id="v01790"></a>
## [0.179.0] - 2026-05-20

- ci: add nightly cross-platform check with auto-filed tracking issues ([#2536](https://github.com/danielraffel/pulp/pull/2536))
- feat(inspect): add Phase 6.2 texture atlas viewer to the inspector overlay ([#2535](https://github.com/danielraffel/pulp/pull/2535))
- test(tools): cover CI gate helper edges ([#2534](https://github.com/danielraffel/pulp/pull/2534))
- test(ci): cover runner and classify helper edges ([#2533](https://github.com/danielraffel/pulp/pull/2533))
- refactor(compat): split compat.json into per-surface parts (P5-NEW-B) ([#2531](https://github.com/danielraffel/pulp/pull/2531))
- fix(inspect): guard source jumps in headless runs ([#2520](https://github.com/danielraffel/pulp/pull/2520))
- feat(params): expose sample-accurate processor events ([#2494](https://github.com/danielraffel/pulp/pull/2494))
- ci: gate coverage.yml on classify + add missing concurrency blocks ([#2532](https://github.com/danielraffel/pulp/pull/2532))
- test(tools): cover local ci path and version checks ([#2528](https://github.com/danielraffel/pulp/pull/2528))
- test(view): cover lock source and visual edge paths ([#2526](https://github.com/danielraffel/pulp/pull/2526))

<a id="v01782"></a>
## [0.178.2] - 2026-05-20

- feat(view): token lock-to-source via DESIGN.md rewrite (Phase 4c) ([#2508](https://github.com/danielraffel/pulp/pull/2508))
- feat(inspect): add Phase 5.2 reconciliation tab to the inspector overlay ([#2524](https://github.com/danielraffel/pulp/pull/2524))

<a id="v01781"></a>
## [0.178.1] - 2026-05-20

- fix(view): defuse deferred mouseUp click after host teardown ([#2523](https://github.com/danielraffel/pulp/pull/2523))
- test(view): cover phase3 widget and asset edges ([#2522](https://github.com/danielraffel/pulp/pull/2522))
- ci: backstop Coverage workflow against multi-hour hangs ([#2521](https://github.com/danielraffel/pulp/pull/2521))
- refactor(view): split legacy web-compat.js into selector + runtime-api siblings ([#2512](https://github.com/danielraffel/pulp/pull/2512))
- test(tools): batch phase 3 coverage hardening ([#2507](https://github.com/danielraffel/pulp/pull/2507))
- test(render): prove SkPicture .skp serialization round-trips on Graphite ([#2506](https://github.com/danielraffel/pulp/pull/2506))
- refactor(pulp-react): split prop-applier switch by domain (P5-NEW-A) ([#2505](https://github.com/danielraffel/pulp/pull/2505))
- fix(state): route StateTree sync deltas by node path (P7-3) ([#2492](https://github.com/danielraffel/pulp/pull/2492))
- test(runtime): batch phase 3 coverage hardening ([#2318](https://github.com/danielraffel/pulp/pull/2318))
- fix(cli): bound json parser recovery ([#2272](https://github.com/danielraffel/pulp/pull/2272))
- test(host): cover VST3 unusable moduleinfo classes ([#2310](https://github.com/danielraffel/pulp/pull/2310))

<a id="v01780"></a>
## [0.178.0] - 2026-05-20

- fix(inspect): suppress real editor launch in source-jump tests/CI ([#2498](https://github.com/danielraffel/pulp/pull/2498))
- test: stop view-host-bridge tests from popping real windows on screen ([#2501](https://github.com/danielraffel/pulp/pull/2501))
- feat(render): add Dawn GPU timestamp queries for true per-pass GPU timing ([#2516](https://github.com/danielraffel/pulp/pull/2516))
- ci(reaper): age in_progress runs from run_started_at; 4h threshold ([#2514](https://github.com/danielraffel/pulp/pull/2514))
- refactor(test): split test_signal.cpp by DSP concern (P5 test-split) ([#2504](https://github.com/danielraffel/pulp/pull/2504))
- fix(import-design): harden CLI fetch and flag parsing ([#2509](https://github.com/danielraffel/pulp/pull/2509))
- refactor(harness): split verifier.py into focused validator modules (P9-3) ([#2503](https://github.com/danielraffel/pulp/pull/2503))

<a id="v01771"></a>
## [0.177.1] - 2026-05-20

- feat(view): lock-to-source for generated-TSX path (Phase 4a) ([#2470](https://github.com/danielraffel/pulp/pull/2470))

<a id="v01770"></a>
## [0.177.0] - 2026-05-20

- refactor(view): rename native design codegen mode ([#2439](https://github.com/danielraffel/pulp/pull/2439))
- ci(runner): enable ccache depend mode + time_macros sloppiness ([#2493](https://github.com/danielraffel/pulp/pull/2493))
- refactor(local-ci): split test_local_ci.py per source module (P11-3) ([#2490](https://github.com/danielraffel/pulp/pull/2490))
- ci(nightly): gate the run on the build step only, ctest informational ([#2491](https://github.com/danielraffel/pulp/pull/2491))
- feat(inspect): add 20x zoom loupe to inspector overlay (Phase 3e) ([#2464](https://github.com/danielraffel/pulp/pull/2464))
- ci: add stale-run-reaper janitor workflow ([#2489](https://github.com/danielraffel/pulp/pull/2489))

<a id="v01760"></a>
## [0.176.0] - 2026-05-20

- feat(inspect): drift drawer + `pulp tweaks diff` CLI (Inspector Phase 2) ([#2437](https://github.com/danielraffel/pulp/pull/2437))
- feat(inspect): Phase 3c — color eyedropper ([#2434](https://github.com/danielraffel/pulp/pull/2434))
- feat(inspect): Phase 5.1 source-jump foundation — open editor at JSX file:line ([#2465](https://github.com/danielraffel/pulp/pull/2465))

<a id="v01750"></a>
## [0.175.0] - 2026-05-20

- feat(params): preserve sample-accurate automation ([#2445](https://github.com/danielraffel/pulp/pull/2445))

<a id="v01740"></a>
## [0.174.0] - 2026-05-20

- test: restore mac gate text and harness coverage ([#2479](https://github.com/danielraffel/pulp/pull/2479))
- feat(inspect): Phase 2.5 — Photoshop-layers tweak management panel ([#2432](https://github.com/danielraffel/pulp/pull/2432))

<a id="v017313"></a>
## [0.173.13] - 2026-05-20

- fix(canvas): match_registered_typeface must miss on off-style requests ([#2471](https://github.com/danielraffel/pulp/pull/2471))
- test(signal): cover delay and ladder edges ([#2475](https://github.com/danielraffel/pulp/pull/2475))
- ci(build): default workflow dispatch to hosted runners ([#2453](https://github.com/danielraffel/pulp/pull/2453))
- feat(inspect): add Phase 6.1 per-pass GPU/render attribution viewer ([#2469](https://github.com/danielraffel/pulp/pull/2469))
- ci: bound iOS AUv3 configure smoke ([#2483](https://github.com/danielraffel/pulp/pull/2483))
- test(runtime): cover ScopedNoAlloc and restore Shipyard mac routing ([#2473](https://github.com/danielraffel/pulp/pull/2473))

<a id="v017312"></a>
## [0.173.12] - 2026-05-20

- ci: add scheduled nightly full-build workflow ([#2468](https://github.com/danielraffel/pulp/pull/2468))
- fix(test): #2462 — shared canvas_pixel_probe.hpp, repair shadow test compile ([#2463](https://github.com/danielraffel/pulp/pull/2463))
- ci(build): build macOS with the Ninja generator ([#2466](https://github.com/danielraffel/pulp/pull/2466))
- fix(mcp): restore test_mcp_server linkage to extracted mcp_compat version helpers ([#2454](https://github.com/danielraffel/pulp/pull/2454))
- ci(build): default runner_provider to github-hosted (Namespace drained) ([#2460](https://github.com/danielraffel/pulp/pull/2460))
- ci(skia): make fetch_skia_for_release.py idempotency manifest-aware ([#2459](https://github.com/danielraffel/pulp/pull/2459))
- ci(build): fetch prebuilt Skia before Configure on the macOS leg ([#2458](https://github.com/danielraffel/pulp/pull/2458))
- ci: parameterize bootstrap runner naming for multi-runner hosts ([#2457](https://github.com/danielraffel/pulp/pull/2457))
- ci: add macOS CI host bootstrap script + Brewfile ([#2456](https://github.com/danielraffel/pulp/pull/2456))
- ci(build): make the macOS overflow disable switch work (Codex P2) ([#2452](https://github.com/danielraffel/pulp/pull/2452))
- ci(build): route macOS overflow to free GitHub-hosted macos-15 ([#2451](https://github.com/danielraffel/pulp/pull/2451))
- ci(build): classify gate — skip native build for non-code PRs ([#2446](https://github.com/danielraffel/pulp/pull/2446))

<a id="v017311"></a>
## [0.173.11] - 2026-05-19

- refactor(canvas): R2-3 — extract skia_canvas_text.cpp (2,873 → 1,879) ([#2444](https://github.com/danielraffel/pulp/pull/2444))
- refactor(cli): R2-4 — extract cli_sdk.cpp from cli_common.cpp (2,022 → 1,257) ([#2443](https://github.com/danielraffel/pulp/pull/2443))

<a id="v01738"></a>
## [0.173.8] - 2026-05-19

- refactor(view): P6-A3 final cut — extract claude_bundle.cpp (design_import.cpp -80%) ([#2438](https://github.com/danielraffel/pulp/pull/2438))

<a id="v01737"></a>
## [0.173.7] - 2026-05-19

- refactor(mcp): P6-B4 third cut — extract mcp_tools + mcp_shell (597 → 451) ([#2436](https://github.com/danielraffel/pulp/pull/2436))
- refactor(mcp): P6-B4 second cut — extract mcp_compat (844 → 597) ([#2435](https://github.com/danielraffel/pulp/pull/2435))
- refactor(view): P6-A3 second cut — extract design_codegen.cpp (4,155 → 3,324) ([#2433](https://github.com/danielraffel/pulp/pull/2433))
- refactor(view): P6-A3 first cut — extract design_tokens.cpp (4,691 → 4,155) ([#2428](https://github.com/danielraffel/pulp/pull/2428))
- feat(inspect): Phase 0b PR-C-2 — property panel dot indicators for tweaks ([#2367](https://github.com/danielraffel/pulp/pull/2367))
- ci: cache Namespace macOS toolchain state ([#2400](https://github.com/danielraffel/pulp/pull/2400))
- refactor(mcp): P6-B4 first cut — JSON-RPC framing to mcp_json.hpp ([#2427](https://github.com/danielraffel/pulp/pull/2427))

<a id="v01731"></a>
## [0.173.1] - 2026-05-19

- refactor(test): P5-4 final cut — lifecycle tests under 2k (2,314 → 1,919) ([#2426](https://github.com/danielraffel/pulp/pull/2426))

<a id="v01730"></a>
## [0.173.0] - 2026-05-19

- feat(design-tool): auto-measure design viewport via 4-layer resolver ([#2409](https://github.com/danielraffel/pulp/pull/2409))
- fix(inspect+view): codex P1+P2 review fixes on Phase 0b ([#2374](https://github.com/danielraffel/pulp/pull/2374))
- feat(inspect): Phase 3a — drag handles on selected node ([#2378](https://github.com/danielraffel/pulp/pull/2378))
- fix(harness): wire react-runtime target into roundtrip scripts (Codex P2 on #2422) ([#2425](https://github.com/danielraffel/pulp/pull/2425))
- refactor(test): P5-4 extract pulp pr shell-out tests (2,646 → 2,314) ([#2424](https://github.com/danielraffel/pulp/pull/2424))
- refactor(test): P5-1 widget_bridge under 3k milestone (3,056 → 2,815) ([#2423](https://github.com/danielraffel/pulp/pull/2423))
- refactor(test): P5-3 extract React-runtime parsers (2,182 → 1,356) ([#2422](https://github.com/danielraffel/pulp/pull/2422))
- refactor(test): P5-1 extract animation API cluster (3,862 → 3,032) ([#2421](https://github.com/danielraffel/pulp/pull/2421))
- refactor(test): P5-1 extract Canvas2D bridge-fns + Yoga borderWidth (4,444 → 3,862) ([#2420](https://github.com/danielraffel/pulp/pull/2420))
- refactor(test): P5-4 split test_cli_shellout — loop + scan/projects (3,439 → 2,646) ([#2416](https://github.com/danielraffel/pulp/pull/2416))

<a id="v01726"></a>
## [0.172.6] - 2026-05-19

- refactor(test): P5-2 + P5-3 bundled cuts (-2,190 lines across 4 parents) ([#2418](https://github.com/danielraffel/pulp/pull/2418))

<a id="v01722"></a>
## [0.172.2] - 2026-05-19

- refactor(test): P5-1 bundled — pulp #1434 per-edge + pulp #1737 RN-OOS-fixup (5,483 → 4,444) ([#2415](https://github.com/danielraffel/pulp/pull/2415))

<a id="v01720"></a>
## [0.172.0] - 2026-05-19

- fix(runtime): batch phase 3 coverage hardening ([#2298](https://github.com/danielraffel/pulp/pull/2298))
- fix(cli): batch phase 3 parser coverage ([#2292](https://github.com/danielraffel/pulp/pull/2292))

<a id="v01710"></a>
## [0.171.0] - 2026-05-19

- refactor(test): P5-1 bundled — yoga + A4 OOS + Wave 2 cheap-wiring (6,711 → 5,483) ([#2414](https://github.com/danielraffel/pulp/pull/2414))
- test: consolidate phase 3 codecov coverage batches ([#2268](https://github.com/danielraffel/pulp/pull/2268))
- feat(inspect): editor URI config for source-jump (Phase 5.3) ([#2388](https://github.com/danielraffel/pulp/pull/2388))

<a id="v01690"></a>
## [0.169.0] - 2026-05-19

- refactor(test): P5-1 bundled — RN style + Tier-4 OOS (7,475 → 6,711) ([#2376](https://github.com/danielraffel/pulp/pull/2376))
- fix(ci): restrict Namespace cache action to macOS lane ([#2410](https://github.com/danielraffel/pulp/pull/2410))
- fix(ci): brew update on Namespace macOS leg before brew install ([#2407](https://github.com/danielraffel/pulp/pull/2407))
- feat(inspect): persist TweakStore to pulp-tweaks.json (Phase 1) ([#2389](https://github.com/danielraffel/pulp/pull/2389))
- feat(inspect): Phase 3b — live-editable box-model fields in property panel ([#2390](https://github.com/danielraffel/pulp/pull/2390))

<a id="v01670"></a>
## [0.167.0] - 2026-05-19

- fix(ci): namespace-cache gate accepts nscloud-* labels (Codex P2 on #2399) ([#2402](https://github.com/danielraffel/pulp/pull/2402))
- refactor(test): P5-2 follow-up — extract arc fixtures (2,358 → 2,026 under 2k target) ([#2380](https://github.com/danielraffel/pulp/pull/2380))
- ci(namespace): add nscloud-cache-action for brew + ccache + FetchContent ([#2399](https://github.com/danielraffel/pulp/pull/2399))
- chore(ci): add tools/scripts/gates.sh — on-demand cheap-gates preflight ([#2384](https://github.com/danielraffel/pulp/pull/2384))
- test(canvas2d): non-identity toFloat32Array order test (Codex P2 on #2387) ([#2396](https://github.com/danielraffel/pulp/pull/2396))
- refactor(midi): extract mpe_voice_tracker.hpp impl bodies to .cpp (U-9) ([#2327](https://github.com/danielraffel/pulp/pull/2327))

<a id="v01660"></a>
## [0.166.0] - 2026-05-19

- test(canvas2d): direct DOMMatrix arithmetic tests for _PulpCanvasMatrix ([#2387](https://github.com/danielraffel/pulp/pull/2387))

<a id="v01610"></a>
## [0.161.0] - 2026-05-19

- test(tools): unit tests for version_consistency_check.py ([#2383](https://github.com/danielraffel/pulp/pull/2383))
- fix(cli): cross-platform exit_status decode in pulp fmt (Windows release build) ([#2375](https://github.com/danielraffel/pulp/pull/2375))

<a id="v01581"></a>
## [0.158.1] - 2026-05-19

- feat(canvas): Vulkan + D3D rendering-goldens scaffold (font v2 Slice 3.4 future-CI lanes) ([#2370](https://github.com/danielraffel/pulp/pull/2370))
- chore(deps): bump shipyard 0.58.0 → 0.59.0 for Phase 2 watch diagnostics (#310) ([#2368](https://github.com/danielraffel/pulp/pull/2368))

<a id="v01580"></a>
## [0.158.0] - 2026-05-19

- refactor(test): P5-1 follow-up — extract Wave 5 css.5 audit (9,074 → 7,475) ([#2373](https://github.com/danielraffel/pulp/pull/2373))

<a id="v01550"></a>
## [0.155.0] - 2026-05-19

- fix(tools): add .clang-format to source-tree pollution allowlist ([#2369](https://github.com/danielraffel/pulp/pull/2369))
- refactor(test): P5-3 follow-up — extract Label cluster (2,288 → 1,716) ([#2371](https://github.com/danielraffel/pulp/pull/2371))

<a id="v01540"></a>
## [0.154.0] - 2026-05-19

- refactor(test): P5-2 follow-up — extract Canvas2D shim post-Wave-2 (3,121 → 1,634) ([#2372](https://github.com/danielraffel/pulp/pull/2372))
- refactor(render): extract texture_atlas.hpp impl bodies to .cpp (U-10) ([#2330](https://github.com/danielraffel/pulp/pull/2330))
- chore(ci): apply Namespace macOS overflow routing on workflow_dispatch too (refs #2314) () ([#2315](https://github.com/danielraffel/pulp/pull/2315))

<a id="v01510"></a>
## [0.151.0] - 2026-05-19

- feat(inspect): Phase 0b PR-C-1 — overlay gesture-tweak emission wiring ([#2363](https://github.com/danielraffel/pulp/pull/2363))
- feat(inspect): Phase 3f — Alt-hover sibling distance lines ([#2328](https://github.com/danielraffel/pulp/pull/2328))
- feat(view): Phase 3d — per-component paint timing ([#2338](https://github.com/danielraffel/pulp/pull/2338))

<a id="v01500"></a>
## [0.150.0] - 2026-05-19

- feat(inspect): Phase 0b PR-A — Inspector.applyTweak protocol + TweakStore ([#2300](https://github.com/danielraffel/pulp/pull/2300))

<a id="v01480"></a>
## [0.148.0] - 2026-05-19

- feat(cli): pulp fmt + .clang-format (Tier A Slice 10) ([#2350](https://github.com/danielraffel/pulp/pull/2350))
- feat(inspect): LiveConstant RPC surfaces PULP_LIVE_CONSTANT in the inspector ([#2353](https://github.com/danielraffel/pulp/pull/2353))

<a id="v01470"></a>
## [0.147.0] - 2026-05-19

- feat(format): Processor::suspend() / resume() virtuals (Tier B Slice 15) ([#2352](https://github.com/danielraffel/pulp/pull/2352))

<a id="v01461"></a>
## [0.146.1] - 2026-05-19

- feat(inspect): Performance.setRepaintFlash toggles DirtyTracker overlay ([#2349](https://github.com/danielraffel/pulp/pull/2349))
- fix(view): TextEditor single-line snapshot honors horizontal scroll ([#2324](https://github.com/danielraffel/pulp/pull/2324))

<a id="v01460"></a>
## [0.146.0] - 2026-05-19

- chore(tools): add version_consistency_check.py — catches Codex P2 #2331 drift ([#2341](https://github.com/danielraffel/pulp/pull/2341))
- feat(cli): pulp doctor --au-cache refreshes macOS AU registrar ([#2336](https://github.com/danielraffel/pulp/pull/2336))
- feat(runtime): generated pulp_build_info.hpp with build stamp constants ([#2335](https://github.com/danielraffel/pulp/pull/2335))
- feat(view): macOS NSAccessibility backend for TextAccessibilityNode (font v2 Slice 2.6 macOS) ([#2307](https://github.com/danielraffel/pulp/pull/2307))
- refactor(test): P5-1 bundled — extract RN outline + clip-path/mask (9,737 → 9,074) ([#2342](https://github.com/danielraffel/pulp/pull/2342))

<a id="v01430"></a>
## [0.143.0] - 2026-05-19

- fix(harness): HtmlAdapter reads element-events split (Codex P2 on #2337) ([#2345](https://github.com/danielraffel/pulp/pull/2345))
- feat(view): Windows UIA + Linux AccessKit a11y backends (font v2 Slice 2.6 complete) ([#2326](https://github.com/danielraffel/pulp/pull/2326))
- refactor(view-js): P5-7 follow-up — extract Element events + pointer capture (1,689 → 1,291) ([#2337](https://github.com/danielraffel/pulp/pull/2337))

<a id="v01420"></a>
## [0.142.0] - 2026-05-19

- feat(canvas): real ICU bidi + script run iterators (font v2 Slice 1.2.a) ([#2311](https://github.com/danielraffel/pulp/pull/2311))
- feat(runtime): PULP_DBG_VAR macro for name-printing debug inspection ([#2312](https://github.com/danielraffel/pulp/pull/2312))
- feat(runtime): ScopedNoAlloc debug guard around audio + paint paths ([#2316](https://github.com/danielraffel/pulp/pull/2316))
- docs: coming-from-juce migration guide (Tier A Slice 14) ([#2329](https://github.com/danielraffel/pulp/pull/2329))
- feat(tools/debug): natvis + LLDB Python formatters for Pulp types ([#2321](https://github.com/danielraffel/pulp/pull/2321))
- feat(state): StateStore::snapshot() block-local helper + DSP threading guide ([#2320](https://github.com/danielraffel/pulp/pull/2320))

<a id="v01390"></a>
## [0.139.0] - 2026-05-18

- feat(canvas): cross-backend rendering goldens — Skia GPU lane (font v2 Slice 3.4 GPU) ([#2306](https://github.com/danielraffel/pulp/pull/2306))
- refactor(test): P5-3 follow-up — extract mask + overflow extension (1,466 → 1,307) ([#2333](https://github.com/danielraffel/pulp/pull/2333))

<a id="v01372"></a>
## [0.137.2] - 2026-05-18

- fix(canvas): canonicalize signed zero in FontOptions hash (Codex P2 on #2169) ([#2332](https://github.com/danielraffel/pulp/pull/2332))

<a id="v01371"></a>
## [0.137.1] - 2026-05-18

- refactor(test): P5-2 follow-up — extract CSS filter chain cluster (2,735 → 2,358) ([#2331](https://github.com/danielraffel/pulp/pull/2331))
- fix(canvas): detached font worker survives register_font_file throws (Codex on #2308) ([#2323](https://github.com/danielraffel/pulp/pull/2323))

<a id="v01370"></a>
## [0.137.0] - 2026-05-18

- refactor(test): P5-1 bundled — extract CSS animations + CSS Grid (10,191 → 9,737) ([#2325](https://github.com/danielraffel/pulp/pull/2325))

<a id="v01360"></a>
## [0.136.0] - 2026-05-18

- refactor(test): P5-1 bundled — extract SVG + HTML ARIA clusters (11,502 → 10,191) ([#2322](https://github.com/danielraffel/pulp/pull/2322))
- refactor(inspect): migrate StateInspector to ListenerToken (Slice 3) ([#2309](https://github.com/danielraffel/pulp/pull/2309))
- build(cmake): statically link the MSVC runtime on Windows ([#2313](https://github.com/danielraffel/pulp/pull/2313))
- fix(ci): clap-validator asset URL (macos-universal, version-prefixed) ([#2302](https://github.com/danielraffel/pulp/pull/2302))

<a id="v01352"></a>
## [0.135.2] - 2026-05-18

- fix(harness): canvas2d adapter reads split preludes (Codex P1 on #2253) ([#2317](https://github.com/danielraffel/pulp/pull/2317))
- feat(state): RT-safe set_value path with deferred main-listener pump ([#2294](https://github.com/danielraffel/pulp/pull/2294))

<a id="v01351"></a>
## [0.135.1] - 2026-05-18

- fix(canvas): address Codex review findings — async future safety, UTF-8/16 bridge, color mode strictness, golden ink probe ([#2308](https://github.com/danielraffel/pulp/pull/2308))
- ci(release): route macOS legs through Namespace + tighten ui-preview gate ([#2301](https://github.com/danielraffel/pulp/pull/2301))
- feat(view): Phase 0b PR-B — View::anchor_id + setAnchor bridge wiring ([#2303](https://github.com/danielraffel/pulp/pull/2303))
- chore(tools): root-allowlist mode for source-tree pollution check (U-1) ([#2190](https://github.com/danielraffel/pulp/pull/2190))

<a id="v01350"></a>
## [0.135.0] - 2026-05-18

- feat(view): Phase 0a — stable_anchor_id on IRNode (inspector round-trip foundation) ([#2295](https://github.com/danielraffel/pulp/pull/2295))
- fix(examples): gate ui-preview --font-probe behind PULP_HAS_SKIA ([#2281](https://github.com/danielraffel/pulp/pull/2281))
- chore(deps): bump shipyard 0.56.2 → 0.58.0 for #303 phase 1 failure diagnostics ([#2293](https://github.com/danielraffel/pulp/pull/2293))
- refactor(events): extract socket endpoint parser; dedup IPC tests (U-8) ([#2214](https://github.com/danielraffel/pulp/pull/2214))
- chore(ci): add format-validator baseline diff gate (U-3) ([#2218](https://github.com/danielraffel/pulp/pull/2218))

<a id="v01340"></a>
## [0.134.0] - 2026-05-18

- feat(state): ListenerToken + thread routing for StateStore listeners ([#2270](https://github.com/danielraffel/pulp/pull/2270))
- docs(ci): pre-push rebase hygiene for Namespace cutover ([#2283](https://github.com/danielraffel/pulp/pull/2283))
- chore(ci): re-commission Namespace for macOS PR work ([#2271](https://github.com/danielraffel/pulp/pull/2271))
- ci(shipyard): route mac validation through Namespace cloud ([#2269](https://github.com/danielraffel/pulp/pull/2269))

<a id="v01330"></a>
## [0.133.0] - 2026-05-18

- feat(canvas): word + line break opportunities (font v2 Slice 2.4) ([#2249](https://github.com/danielraffel/pulp/pull/2249))

<a id="v01320"></a>
## [0.132.0] - 2026-05-18

- refactor(test): P5-1 follow-up — extract Yoga-layer bridge cluster (11,888 → 11,455) ([#2255](https://github.com/danielraffel/pulp/pull/2255))
- refactor(test): P5-3 follow-up — extract z-index + overflow cluster (1,839 → 1,466) ([#2254](https://github.com/danielraffel/pulp/pull/2254))
- feat(canvas): cross-backend rendering goldens harness (font v2 Slice 3.4) ([#2261](https://github.com/danielraffel/pulp/pull/2261))

<a id="v01280"></a>
## [0.128.0] - 2026-05-18

- feat(view): TextAccessibilityNode scaffold (font v2 Slice 2.6) ([#2257](https://github.com/danielraffel/pulp/pull/2257))

<a id="v01270"></a>
## [0.127.0] - 2026-05-18

- feat(canvas): WOFF2 runtime decoding (font v2 Slice 3.5) ([#2250](https://github.com/danielraffel/pulp/pull/2250))

<a id="v01260"></a>
## [0.126.0] - 2026-05-18

- refactor(test): P5-2 follow-up — extract CG gradient/pattern cluster (3,363 → 2,735) ([#2247](https://github.com/danielraffel/pulp/pull/2247))

<a id="v01250"></a>
## [0.125.0] - 2026-05-18

- refactor(view-js): P5-7 follow-up — extract WebGPU mock factories (1,579 → 1,016) ([#2251](https://github.com/danielraffel/pulp/pull/2251))
- refactor(view-js): P5-6 follow-up — extract canvas matrix + image API (1,581 → 1,223) ([#2253](https://github.com/danielraffel/pulp/pull/2253))

<a id="v01210"></a>
## [0.121.0] - 2026-05-18

- refactor(test): P5-1 follow-up — extract canvas2d Wave 2 cluster (12,443 → 11,888) ([#2248](https://github.com/danielraffel/pulp/pull/2248))

<a id="v01160"></a>
## [0.116.0] - 2026-05-18

- feat(canvas): register_font_url async lifecycle (font v2 Slice 2.1) ([#2246](https://github.com/danielraffel/pulp/pull/2246))
- refactor(view-js): Phase 5 P5-7 first cut — extract CSS selector engine ([#2245](https://github.com/danielraffel/pulp/pull/2245))
- feat(canvas): color-font predicate on ResolvedFont (font v2 Slice 3.1) ([#2243](https://github.com/danielraffel/pulp/pull/2243))
- refactor(view-js): Phase 5 P5-6 first cut — extract native GPU canvas helpers ([#2242](https://github.com/danielraffel/pulp/pull/2242))
- feat(canvas): TextRunPlanner::shape_batch parallel shaping (font v2 Slice 3.7) ([#2240](https://github.com/danielraffel/pulp/pull/2240))
- fix(release): supersede only past latest-published — stop SDK starvation cascade ([#2238](https://github.com/danielraffel/pulp/pull/2238))
- test: batch phase3 coverage format runtime edges ([#2208](https://github.com/danielraffel/pulp/pull/2208))
- refactor(view): Phase 4 R2-2 header-diet first cut — view_fwd.hpp + out-of-line dtor ([#2236](https://github.com/danielraffel/pulp/pull/2236))
- docs(experimental): document pulp-rs production status (U-4) ([#2187](https://github.com/danielraffel/pulp/pull/2187))

<a id="v01150"></a>
## [0.115.0] - 2026-05-18

- fix(motion): MotionPreferences mutex + PulpMotionProbe ambient race + FixtureFileSink mutex (closes #2148 #2150 #2151) ([#2225](https://github.com/danielraffel/pulp/pull/2225))

<a id="v01140"></a>
## [0.114.0] - 2026-05-18

- feat(motion): Phase 11 — native bridges, visual+ enhancements, scroll/outlier, CLI/MCP ([#2168](https://github.com/danielraffel/pulp/pull/2168))

<a id="v01139"></a>
## [0.113.9] - 2026-05-18

- refactor(view-js): Phase 5 P5-5 first cut — extract CSSStyleDeclaration helpers ([#2217](https://github.com/danielraffel/pulp/pull/2217))
- fix(canvas+view): address Codex review findings on font v2 PRs ([#2215](https://github.com/danielraffel/pulp/pull/2215))
- feat(canvas): LRU cache cap for variable-font animation (font v2 Slice 3.3) ([#2212](https://github.com/danielraffel/pulp/pull/2212))
- test: batch phase3 coverage runtime and canvas edges ([#2202](https://github.com/danielraffel/pulp/pull/2202))

<a id="v01138"></a>
## [0.113.8] - 2026-05-18

- refactor(test): Phase 5 P5-1 continuation — extract canvas2d tests from test_widget_bridge.cpp ([#2201](https://github.com/danielraffel/pulp/pull/2201))
- refactor(test): Phase 5 P5-3 first cut — extract corner-radius tests from test_view.cpp ([#2200](https://github.com/danielraffel/pulp/pull/2200))
- refactor(test): Phase 5 P5-2 first cut — extract font tests from test_canvas.cpp ([#2196](https://github.com/danielraffel/pulp/pull/2196))

<a id="v01135"></a>
## [0.113.5] - 2026-05-18

- refactor(view): Phase 4 R2-5 first cut — extract PNG capture helpers from window_host_mac.mm ([#2194](https://github.com/danielraffel/pulp/pull/2194))
- refactor(test): Phase 3 R2-7 — pulp_add_test_suite() helper + 200 test migrations ([#2192](https://github.com/danielraffel/pulp/pull/2192))
- feat(canvas): FontFlightRecorder — diagnostics CLI foundation (font v2 Slice 2.2) ([#2191](https://github.com/danielraffel/pulp/pull/2191))
- fix(canvas): repair PR #2183 split-build regression — shared internal helpers + stub broken Dawn path ([#2189](https://github.com/danielraffel/pulp/pull/2189))

<a id="v01133"></a>
## [0.113.3] - 2026-05-18

- refactor(canvas): Phase 4 R2-3 cg mirror — gradients + drop-shadow slices ([#2188](https://github.com/danielraffel/pulp/pull/2188))
- feat(canvas): centralize AA/hinting policy on ResolvedFont (font v2 Slice 3.2) ([#2186](https://github.com/danielraffel/pulp/pull/2186))
- feat(canvas): UAX #29-lite cluster_step (font v2 Slice 3.6) ([#2185](https://github.com/danielraffel/pulp/pull/2185))

<a id="v01132"></a>
## [0.113.2] - 2026-05-18

- refactor(canvas): Phase 4 R2-3 — gradients + box-shadow + opacity + shaders slices ([#2183](https://github.com/danielraffel/pulp/pull/2183))
- feat(view): HTML `dir` attribute wires to View::WritingDirection (font v2 Slice 2.5) ([#2181](https://github.com/danielraffel/pulp/pull/2181))
- feat(canvas): wire variation axes through FontResolver (font v2 Slice 2.3) ([#2180](https://github.com/danielraffel/pulp/pull/2180))

<a id="v01131"></a>
## [0.113.1] - 2026-05-18

- refactor(phase-4): R2-1 cluster finish — provenance + job_queue + targets + github_workflows ([#2179](https://github.com/danielraffel/pulp/pull/2179))
- feat(canvas): structural TTF/OTF sanitizer in validate_font_bytes (font v2 Slice 2.8) ([#2177](https://github.com/danielraffel/pulp/pull/2177))
- feat(canvas): wire FontScope::set_memory_budget to resolver eviction (font v2 Slice 2.7) ([#2175](https://github.com/danielraffel/pulp/pull/2175))
- feat: font subsystem v2 hardening — Phase 1 complete (CHAIN INFO canary flipped), Phase 2/3 skeletons ([#2169](https://github.com/danielraffel/pulp/pull/2169))

<a id="v01130"></a>
## [0.113.0] - 2026-05-17

- feat(import-design): JSX-instrument runtime-import (additive slice) [rebased] ([#2163](https://github.com/danielraffel/pulp/pull/2163))

<a id="v01120"></a>
## [0.112.0] - 2026-05-17

- feat(motion): Phases 8-10 + cost attribution + timeline scrubber ([#2142](https://github.com/danielraffel/pulp/pull/2142))

<a id="v01110"></a>
## [0.111.0] - 2026-05-17

- test(midi): batch ci and ump coverage edges ([#2126](https://github.com/danielraffel/pulp/pull/2126))
- feat(design-import): Phase A — default shortcuts (source-matched heuristic) [rebased] ([#2161](https://github.com/danielraffel/pulp/pull/2161))
- refactor(test): split runtime-import tests out of test_widget_bridge.cpp (P5-1 first cut) ([#2160](https://github.com/danielraffel/pulp/pull/2160))
- feat(view+render+docs): partial-rendering POC + TextEditor multi-line + DrawBatcher delete + shader docs ([#2162](https://github.com/danielraffel/pulp/pull/2162))
- feat(canvas+view): color-emoji parity for Canvas2D text ([#2157](https://github.com/danielraffel/pulp/pull/2157))

<a id="v01100"></a>
## [0.110.0] - 2026-05-17

- feat(view): Esc closes React popovers via document outside-click fan-out (#2128 follow-up) ([#2139](https://github.com/danielraffel/pulp/pull/2139))
- refactor(local-ci): R2-1 cluster — normalize + git_helpers + io_utils + footprint ([#2159](https://github.com/danielraffel/pulp/pull/2159))
- refactor/p4 batch ([#2158](https://github.com/danielraffel/pulp/pull/2158))
- refactor(phase-3): C2 root CMakeLists split + R2-9 PulpUtils split ([#2147](https://github.com/danielraffel/pulp/pull/2147))
- test: batch broad phase3 coverage edges ([#2143](https://github.com/danielraffel/pulp/pull/2143))
- refactor(phase-2): R2-4 cli_common split + R2-6 widgets split + R2-8 doctor filters ([#2145](https://github.com/danielraffel/pulp/pull/2145))

<a id="v01090"></a>
## [0.109.0] - 2026-05-17

- fix(view): WidgetBridge static registry — generic keyboard dispatch (closes platform-key-wireup gap, #2128 follow-up) ([#2137](https://github.com/danielraffel/pulp/pull/2137))
- docs(pulp-react): document useShortcut hook in README ([#2154](https://github.com/danielraffel/pulp/pull/2154))
- feat(pulp-react): useShortcut hook + clash detection (pulp #135 Phase B) ([#2144](https://github.com/danielraffel/pulp/pull/2144))
- test: batch runtime primitive coverage edges ([#2124](https://github.com/danielraffel/pulp/pull/2124))
- test(canvas): batch attributed text layout coverage edges ([#2135](https://github.com/danielraffel/pulp/pull/2135))
- test: batch runtime utility and environment coverage edges ([#2132](https://github.com/danielraffel/pulp/pull/2132))
- test(view): batch lifecycle widget coverage edges ([#2134](https://github.com/danielraffel/pulp/pull/2134))
- test(runtime): batch network stream socket coverage edges ([#2130](https://github.com/danielraffel/pulp/pull/2130))
- test(osc): batch codec and bundle coverage edges ([#2129](https://github.com/danielraffel/pulp/pull/2129))
- test: batch runtime and audio coverage edges ([#2123](https://github.com/danielraffel/pulp/pull/2123))
- refactor: ops batch 2 — 9 first-cuts (C3 + B3 + B1 + A2 + A3 + B4 + #19/#22/#23) ([#2138](https://github.com/danielraffel/pulp/pull/2138))

<a id="v01080"></a>
## [0.108.0] - 2026-05-17

- feat(motion): agent-first motion observability (Phases 0-7) ([#2136](https://github.com/danielraffel/pulp/pull/2136))
- refactor: ops batch 2026-05-16 (B2 + A1 first-cut + D1 + C1 + C2) ([#2131](https://github.com/danielraffel/pulp/pull/2131))
- test: batch state coverage edges ([#2103](https://github.com/danielraffel/pulp/pull/2103))

<a id="v01070"></a>
## [0.107.0] - 2026-05-17

- feat(create): default new projects to Release; --debug opts in ([#2133](https://github.com/danielraffel/pulp/pull/2133))
- test(runtime): batch helper and host coverage edges ([#2108](https://github.com/danielraffel/pulp/pull/2108))
- test: batch runtime and host coverage edges ([#2122](https://github.com/danielraffel/pulp/pull/2122))

<a id="v01060"></a>
## [0.106.0] - 2026-05-17

- feat(design-import): V2 wire-up — emit registerShortcut + synthetic keydown re-dispatch ([#2119](https://github.com/danielraffel/pulp/pull/2119))

<a id="v01050"></a>
## [0.105.0] - 2026-05-17

- feat(view): focus-guard for bare-key global shortcuts ([#2120](https://github.com/danielraffel/pulp/pull/2120))
- docs(ci): codify REST fallback when GraphQL is exhausted ([#2118](https://github.com/danielraffel/pulp/pull/2118))
- test(midi): batch coverage edges ([#2104](https://github.com/danielraffel/pulp/pull/2104))
- test(tools): batch coverage gate and visual harness edges ([#2114](https://github.com/danielraffel/pulp/pull/2114))
- test(cli): batch package registry coverage edges ([#2110](https://github.com/danielraffel/pulp/pull/2110))

<a id="v01040"></a>
## [0.104.0] - 2026-05-16

- feat(design-import): extract keyboard shortcuts from React source (V1) ([#2116](https://github.com/danielraffel/pulp/pull/2116))
- test(runtime): batch crypto and osc coverage edges ([#2109](https://github.com/danielraffel/pulp/pull/2109))
- test(audio): batch tools coverage edges ([#2111](https://github.com/danielraffel/pulp/pull/2111))
- feat(pulp-react): auto-claim overlay for ARIA modal/popup roles ([#2115](https://github.com/danielraffel/pulp/pull/2115))
- fix(view): restore native Dawn GPU bridge in JS preludes (cube renders again) ([#2101](https://github.com/danielraffel/pulp/pull/2101))
- test(runtime): cover memory message channel edges ([#2107](https://github.com/danielraffel/pulp/pull/2107))
- test(signal): batch DSP coverage edges ([#2106](https://github.com/danielraffel/pulp/pull/2106))
- fix(view): SvgPathWidget exposes viewBox as intrinsic size (#72) ([#2105](https://github.com/danielraffel/pulp/pull/2105))

<a id="v01030"></a>
## [0.103.0] - 2026-05-16

- feat(cli): SDK-update UX — auto-update default, easy pin opt-out (#2087) ([#2091](https://github.com/danielraffel/pulp/pull/2091))
- test: consolidate phase 3 codecov coverage queue ([#2102](https://github.com/danielraffel/pulp/pull/2102))

<a id="v01021"></a>
## [0.102.1] - 2026-05-15

- fix(view/mac): re-sync _focusedView at every deref site (closes #2088) ([#2093](https://github.com/danielraffel/pulp/pull/2093))
- docs(release): add end-to-end release pipeline doc ([#2071](https://github.com/danielraffel/pulp/pull/2071))
- ci(release): auto-supersede in-flight builds when a newer version tags ([#2076](https://github.com/danielraffel/pulp/pull/2076))
- feat(tooling): refuse to run validation harness on stale checkout (#2087) ([#2090](https://github.com/danielraffel/pulp/pull/2090))
- feat(install): install CLI + matching SDK in one shot (closes #2087 slice 1) ([#2095](https://github.com/danielraffel/pulp/pull/2095))

<a id="v01020"></a>
## [0.102.0] - 2026-05-15

- feat(mcp): ship pulp-mcp in tarball + per-tool feature detection (#2067, #2070) ([#2068](https://github.com/danielraffel/pulp/pull/2068))

<a id="v01018"></a>
## [0.101.8] - 2026-05-15

- feat(view,cli,import): bundle 4 first-user friction fixes + tests for resize fix ([#1984](https://github.com/danielraffel/pulp/pull/1984))

<a id="v01017"></a>
## [0.101.7] - 2026-05-15

- fix(view/host/mac): host-level ESC closes ComboBox even with stolen focus (#68) ([#2010](https://github.com/danielraffel/pulp/pull/2010))
- docs: document docs-only PR ship invocation in CLAUDE.md ([#2066](https://github.com/danielraffel/pulp/pull/2066))
- docs(reviews): land repo readability cleanup plan ([#2020](https://github.com/danielraffel/pulp/pull/2020))

<a id="v01015"></a>
## [0.101.5] - 2026-05-15

- fix(cmake): blanket-apply fontconfig link helper to every executable on Linux ([#2060](https://github.com/danielraffel/pulp/pull/2060))

<a id="v01014"></a>
## [0.101.4] - 2026-05-15

- fix(canvas): force -lfontconfig at end of every downstream link line ([#2058](https://github.com/danielraffel/pulp/pull/2058))

<a id="v01013"></a>
## [0.101.3] - 2026-05-15

- fix(cmake): apply fontconfig link-order helper to all Standalone targets (#2018 follow-up) ([#2056](https://github.com/danielraffel/pulp/pull/2056))

<a id="v01012"></a>
## [0.101.2] - 2026-05-15

- fix(cmake): link fontconfig after libskia.a in pulp-import-design (#1986 follow-up) ([#2018](https://github.com/danielraffel/pulp/pull/2018))

<a id="v01011"></a>
## [0.101.1] - 2026-05-15

- fix(canvas/svg): emit fills + cubic Bezier curves in SvgImage::render (#72) ([#2011](https://github.com/danielraffel/pulp/pull/2011))
- fix(test/mac-harness): real scroll deltas + per-button selector routing ([#2015](https://github.com/danielraffel/pulp/pull/2015))

<a id="v01010"></a>
## [0.101.0] - 2026-05-15

- fix(view/widgets): programmatic set_value/set_on/set_label requests repaint (#73) ([#2013](https://github.com/danielraffel/pulp/pull/2013))

<a id="v01000"></a>
## [0.100.0] - 2026-05-15

- test(view): mac platform-test harness scaffold + back-buffer capture seam (#2001) ([#2009](https://github.com/danielraffel/pulp/pull/2009))
- docs+plugin: TLDR install at top of README + agent-integrations doc + Setup hook for missing CLI ([#2000](https://github.com/danielraffel/pulp/pull/2000))
- fix(view/auto_ui): centered wrapping grid layout (closes #97) ([#2007](https://github.com/danielraffel/pulp/pull/2007))
- fix(deps): pin Highway to 1.2.0 release SHA to stop FetchContent flake ([#2005](https://github.com/danielraffel/pulp/pull/2005))
- test(mcp): close coverage + parity gaps from #1997 ([#1999](https://github.com/danielraffel/pulp/pull/1999))
- fix(cmake): wrap Skia archives in --start-group on Linux/Android (SkUnicode core/icu) ([#1991](https://github.com/danielraffel/pulp/pull/1991))
- ci(release): add release-path PR gate + draft-stuck watchdog (#1962 prevention) ([#1994](https://github.com/danielraffel/pulp/pull/1994))

<a id="v0990"></a>
## [0.99.0] - 2026-05-14

- fix(plugin): bump marketplace plugins[0].version + relative .mcp.json path ([#1996](https://github.com/danielraffel/pulp/pull/1996))
- fix(view): reserve Yoga height for multi-line Labels (pulp-internal #74) ([#1969](https://github.com/danielraffel/pulp/pull/1969))
- fix(codecov): align bot ignore list with Pulp diff-cover gate exclusions ([#1988](https://github.com/danielraffel/pulp/pull/1988))
- fix(view): bump line-height multiplier to 1.6 for small font Labels (#76) ([#1990](https://github.com/danielraffel/pulp/pull/1990))
- fix(cli): explicitly link fontconfig after libskia.a in pulp-cli ([#1986](https://github.com/danielraffel/pulp/pull/1986))
- ci(import-validation): gate source-contract registry ([#1983](https://github.com/danielraffel/pulp/pull/1983))
- fix(canvas): use pkg-config IMPORTED_TARGET for fontconfig ([#1982](https://github.com/danielraffel/pulp/pull/1982))
- test(import-validation): backfill source-contract verification ([#1980](https://github.com/danielraffel/pulp/pull/1980))
- fix(canvas,ci): propagate fontconfig PUBLIC + extend backfill overlay ([#1979](https://github.com/danielraffel/pulp/pull/1979))
- feat(import-validation): add source-contract registry linter ([#1977](https://github.com/danielraffel/pulp/pull/1977))
- fix(ci): install libfontconfig1-dev on Linux for chrome/m144 Skia ([#1974](https://github.com/danielraffel/pulp/pull/1974))
- fix(import-validation): fall back to public roundtrip fixtures ([#1976](https://github.com/danielraffel/pulp/pull/1976))
- fix(view+design-tool): import-design escapes JS-string user text (#81); design-tool window title derives from --script (#61) ([#1966](https://github.com/danielraffel/pulp/pull/1966))
- fix(release): unblock SDK pipeline — flatten Skia arch subdir + safe backfill ([#1965](https://github.com/danielraffel/pulp/pull/1965))
- feat(import): add Pencil React export parser (Phase 6.6.6) ([#1964](https://github.com/danielraffel/pulp/pull/1964))
- feat(view+pulp-react+tools): live-host parity hardening — drive setInterval, recursive asText, lint+smoke, hidden-window flag ([#1960](https://github.com/danielraffel/pulp/pull/1960))
- feat(import,design): DESIGN.md adoption — Phase 1 + 2 + Phase 3 scaffold ([#1934](https://github.com/danielraffel/pulp/pull/1934))
- fix(view+cmake): port viewport_reconcile to core/view; LOUD perf-regression guard when Skia not linked (#1957) ([#1959](https://github.com/danielraffel/pulp/pull/1959))
- feat(import): add React Native export parser (Phase 6.6.5) ([#1949](https://github.com/danielraffel/pulp/pull/1949))
- fix(design-tool): install runtime-import handlers + deferred settle pump for React imports ([#1956](https://github.com/danielraffel/pulp/pull/1956))
- feat(view): extend listStyleType with roman/alpha keywords (storage-only, Tier 2) ([#1878](https://github.com/danielraffel/pulp/pull/1878))
- test(view): pin 6 perf-hint/interaction CSS no-op properties + close 7 coverage-gap rows (Bundle-C, finishes Tier-4 sweep) ([#1904](https://github.com/danielraffel/pulp/pull/1904))
- docs(ci): adopt Shipyard v0.56.2 runner ops toolkit ([#1908](https://github.com/danielraffel/pulp/pull/1908))
- fix(view+web-compat): route <path> to SvgPathWidget; <input type=range> defaults horizontal (closes gaps #4+#5 of #1899) ([#1917](https://github.com/danielraffel/pulp/pull/1917))
- feat(import-validation): per-region masked diff for runtime-import validation ([#1871](https://github.com/danielraffel/pulp/pull/1871))
- feat(tools): add coordination scripts (health-check, quick-spectr-cycle, capture-webview-baseline) ([#1912](https://github.com/danielraffel/pulp/pull/1912))
- fix(ci): path-filter iOS auv3 try-compile (closes #1887) ([#1913](https://github.com/danielraffel/pulp/pull/1913))
- feat(pulp-screenshot): default to Skia backend; expose --backend flag (closes #1899 harness divergence) ([#1919](https://github.com/danielraffel/pulp/pull/1919))
- fix(react): display:flex defaults to flex-direction:row in flat-prop path (closes #1894) ([#1898](https://github.com/danielraffel/pulp/pull/1898))
- fix(view+canvas): accept 4-arg canvasFillText + auto-inject set_font for third-party shims (partial #1899) ([#1901](https://github.com/danielraffel/pulp/pull/1901))
- fix(pulp-react): clear disappearing style keys in applyChangedProps (closes #1925) ([#1932](https://github.com/danielraffel/pulp/pull/1932))
- feat(pulp-screenshot): viewport-shift for oversize absolute children (#1899) ([#1906](https://github.com/danielraffel/pulp/pull/1906))
- test(view): pin 10 OOS-3D/content/scroll CSS no-op properties + close 10 coverage-gap rows (Bundle-B) ([#1903](https://github.com/danielraffel/pulp/pull/1903))
- test(view): pin 15 Yoga-arch CSS no-op properties + close 15 coverage-gap rows (Bundle-A) ([#1902](https://github.com/danielraffel/pulp/pull/1902))
- test(view/css): close 6 coverage-gap rows (Tier 2 trivial + catalog-flip honest reclass) ([#1870](https://github.com/danielraffel/pulp/pull/1870))
- fix(css): resolve calc/min/max/clamp everywhere via resolveCSSLength + 3 Codex closeouts (replaces stale pulp #1576) ([#1862](https://github.com/danielraffel/pulp/pull/1862))
- feat(view): support text-align: match-parent via paint-time parent walk ([#1879](https://github.com/danielraffel/pulp/pull/1879))
- feat(view): wire 5 CSS cursor keywords to macOS NSCursor (alias/copy/zoom-in/zoom-out/context-menu) ([#1877](https://github.com/danielraffel/pulp/pull/1877))
- test(view/css): close css/__selector_class as INTENTIONAL design (Tier 4) ([#1874](https://github.com/danielraffel/pulp/pull/1874))
- feat(ci): vitest v8 coverage + Cobertura upload for packages/pulp-react (Phase 1, refs #1886) ([#1915](https://github.com/danielraffel/pulp/pull/1915))
- fix(canvas/cg): implement setLineDash on macOS CoreGraphics backend ([#1914](https://github.com/danielraffel/pulp/pull/1914))
- fix(ci): scope concurrency cancel-in-progress to protect coverage uploads (closes #1884) ([#1911](https://github.com/danielraffel/pulp/pull/1911))
- diag(view/yoga): PULP_DUMP_BOUNDS env + Spectr-shape regression anchor (#1899) ([#1905](https://github.com/danielraffel/pulp/pull/1905))
- feat(view): add Element.prototype.contains — DOM Node.contains contract ([#1872](https://github.com/danielraffel/pulp/pull/1872))
- fix(tooling): chmod +x tools/scripts/verify_gpu_build.sh (Codex P2 on #1847) ([#1865](https://github.com/danielraffel/pulp/pull/1865))
- fix(import-validation): length-gate substring-fallback in check_label_coverage (Codex P1 on #1847) ([#1864](https://github.com/danielraffel/pulp/pull/1864))
- fix(view-bridge): aggregate ALL transient runtime-import error globals (Codex P1+P2 on #1856) ([#1863](https://github.com/danielraffel/pulp/pull/1863))
- test(view): pin borderRadius % effective radius + paint for #1731 ([#1867](https://github.com/danielraffel/pulp/pull/1867))
- test(view): pin outlineColor currentColor resolution for #1728 ([#1869](https://github.com/danielraffel/pulp/pull/1869))
- feat(import-validation): semantic-probe vector for runtime-import harness ([#1875](https://github.com/danielraffel/pulp/pull/1875))
- fix(ci): static name for Diff coverage required gate so it actually enforces ([#1883](https://github.com/danielraffel/pulp/pull/1883))
- fix(view+screenshot): drive runtime-import event loop in pulp-screenshot for headless React captures (#1899) ([#1900](https://github.com/danielraffel/pulp/pull/1900))
- fix(canvas+pulp-react): disable LCD subpixel AA inside non-opaque layers; resolve var() in React fontFamily/color (closes gap #3 of #1899) ([#1918](https://github.com/danielraffel/pulp/pull/1918))
- feat(view+web-compat): route <rect> <line> <circle> SVG primitives + attribute replay (closes #1926) ([#1928](https://github.com/danielraffel/pulp/pull/1928))
- feat(import): add Stitch React export parser (Phase 6.6.4) ([#1946](https://github.com/danielraffel/pulp/pull/1946))
- feat(ci): pulp overflow CLI for macOS-runner overflow routing ([#1942](https://github.com/danielraffel/pulp/pull/1942))
- feat(ci): opportunistic macOS-overflow reroute watcher ([#1944](https://github.com/danielraffel/pulp/pull/1944))
- fix(ci): macOS overflow probe counts queued-to-local only ([#1943](https://github.com/danielraffel/pulp/pull/1943))
- feat(ci): pulp macos retarget — per-PR macOS-runner override without full rerun ([#1941](https://github.com/danielraffel/pulp/pull/1941))
- feat(import): Figma Make React export parser (Phase 6.6.3) ([#1910](https://github.com/danielraffel/pulp/pull/1910))
- fix(ci): macOS overflow probe — use in_progress runs proxy (no admin scope) ([#1936](https://github.com/danielraffel/pulp/pull/1936))
- ci(build): macOS overflow routing to Namespace when local Mac is busy ([#1935](https://github.com/danielraffel/pulp/pull/1935))
- fix(view): Label wraps with CSS default white-space:normal (closes #1924) ([#1933](https://github.com/danielraffel/pulp/pull/1933))
- ci: add path-scoped parser validation profile (closes #1916) ([#1920](https://github.com/danielraffel/pulp/pull/1920))
- fix(widget_bridge): pump message loop after JS event dispatch so drag-state commits before next event (closes #1923) ([#1929](https://github.com/danielraffel/pulp/pull/1929))
- docs(ci): document recovery + maintenance toolkit (v0.56.2) ([#1927](https://github.com/danielraffel/pulp/pull/1927))
- ci/exclude slow label from pr validation ([#1876](https://github.com/danielraffel/pulp/pull/1876))
- feat(import): v0.dev React export parser (Phase 6.6.2) ([#1897](https://github.com/danielraffel/pulp/pull/1897))
- test(canvas2d): pin 9-arg drawImage coverage for #1739 ([#1868](https://github.com/danielraffel/pulp/pull/1868))
- fix(pulp-react): getPublicInstance returns DOM-shim Element ([#1859](https://github.com/danielraffel/pulp/pull/1859))
- fix(host): cache dlerror() before std::format call in CLAP scanner (ASan SEGV) ([#1873](https://github.com/danielraffel/pulp/pull/1873))
- test(view/css): pin Tier-1 supported core + reclassify 5 rows (closes 5 coverage-gaps) ([#1857](https://github.com/danielraffel/pulp/pull/1857))
- fix(css): nested var() fallback + var-in-calc resolution via balanced-paren walker (closes coverage-gap css/__var) ([#1855](https://github.com/danielraffel/pulp/pull/1855))
- feat(view): SDK C++ runtime-import API — install_runtime_import_handlers ([#1856](https://github.com/danielraffel/pulp/pull/1856))
- test(view/css): pin display:contents arch-deferred no-op (closes coverage-gap rn/display + yoga/display) ([#1852](https://github.com/danielraffel/pulp/pull/1852))
- feat(css): alignItems / justifyContent CSS-spec value aliases (Tier 1 #1434) ([#1853](https://github.com/danielraffel/pulp/pull/1853))
- test(import-validation): use POSIX [[:space:]] instead of GNU \s in #81 regression guard (Codex P2 on PR #1849) ([#1851](https://github.com/danielraffel/pulp/pull/1851))
- fix(import-validation): no full-screen capture fallback — preserves window-only diff against REFERENCE (closes pulp-internal #81) ([#1849](https://github.com/danielraffel/pulp/pull/1849))
- feat(import-validation): structural label-coverage check ([#1847](https://github.com/danielraffel/pulp/pull/1847))
- feat(cmake): Debug-SDK perf-killer guard in PulpConfig.cmake (closes pulp-internal #35) ([#1828](https://github.com/danielraffel/pulp/pull/1828))
- feat(import-design): promote interactive frames to buttons (post-parse pass) ([#1824](https://github.com/danielraffel/pulp/pull/1824))
- test(spectr): autonomous regression suite composing the #1792 event-bridge contracts ([#1827](https://github.com/danielraffel/pulp/pull/1827))
- fix(cli): version-pin SDK tarball filename — kills pulp install stale-cache (closes #1814) ([#1825](https://github.com/danielraffel/pulp/pull/1825))
- fix(mcp): wrap pulp-mcp in launcher so .mcp.json resolves regardless of cwd (closes #1821) ([#1822](https://github.com/danielraffel/pulp/pull/1822))
- fix(release): ship SDK tarballs with Skia/GPU — fix pulp #1817 ([#1820](https://github.com/danielraffel/pulp/pull/1820))
- fix(react): implement resetTextContent for text→non-text transition (Codex P1 on #1840) ([#1842](https://github.com/danielraffel/pulp/pull/1842))
- fix(react): TEXT_BEARING children-aware short-circuit (Codex P1 on #1836) ([#1840](https://github.com/danielraffel/pulp/pull/1840))
- docs(compat): document CG canvas-backend gap on canvas2d/filter (Codex P? on #1724) ([#1839](https://github.com/danielraffel/pulp/pull/1839))

<a id="v0940"></a>
## [0.94.0] - 2026-05-12

- feat(format): StandaloneApp headless --screenshot capture (SDK-codified) ([#1838](https://github.com/danielraffel/pulp/pull/1838))
- fix(react): flexDirection prop case + TEXT_BEARING HTML aliases ([#1836](https://github.com/danielraffel/pulp/pull/1836))
- fix(react): 6 more React-applier prop gaps (Spectr coverage batch 2) ([#1831](https://github.com/danielraffel/pulp/pull/1831))
- fix(react,view): CSS `color` alias + window.parent self-reference ([#1830](https://github.com/danielraffel/pulp/pull/1830))

<a id="v0930"></a>
## [0.93.0] - 2026-05-11

- fix(view): mac mouseDown/keyDown stale pointers — PAC crash on react unmount ([#1819](https://github.com/danielraffel/pulp/pull/1819))

<a id="v0920"></a>
## [0.92.0] - 2026-05-11

- feat(view): rn/borderCurve squircle + css/rn isolation flip (RN to 100% PASS) ([#1816](https://github.com/danielraffel/pulp/pull/1816))
- fix(canvas): fill_current_path / stroke_current_path preserve the scratch path (closes #1806) ([#1807](https://github.com/danielraffel/pulp/pull/1807))

<a id="v0910"></a>
## [0.91.0] - 2026-05-11

- feat(rn): honest flip rn/includeFontPadding wontfix → supported (rn 97.5% → 98.3%) ([#1809](https://github.com/danielraffel/pulp/pull/1809))

<a id="v0900"></a>
## [0.90.0] - 2026-05-11

- feat(rn): shim Material elevation to boxShadow (closes last implementable RN OOS — rn 97.5%) ([#1808](https://github.com/danielraffel/pulp/pull/1808))

<a id="v0890"></a>
## [0.89.0] - 2026-05-11

- feat(rn/css): box-shadow longhand + scroll-behavior + overscroll-behavior (rn 96.7% / css 84.8%) ([#1805](https://github.com/danielraffel/pulp/pull/1805))
- fix(harness): canvas2d transform oracle validates canvasSetTransform (Codex P2 on #1729) ([#1804](https://github.com/danielraffel/pulp/pull/1804))

<a id="v0880"></a>
## [0.88.0] - 2026-05-11

- fix(view): paint honors percent corner-radius + Panel uses View slot (Codex P1×2 on #1731) ([#1802](https://github.com/danielraffel/pulp/pull/1802))

<a id="v0873"></a>
## [0.87.3] - 2026-05-10

- fix(canvas2d): DOMMatrix scaleSelf/rotateSelf/inverse spec conformance (Codex P1×2 + P2 on #1730) ([#1754](https://github.com/danielraffel/pulp/pull/1754))

<a id="v0872"></a>
## [0.87.2] - 2026-05-10

- fix(view): mac frame loop honors active CSS animations (Codex P1 on #1734) ([#1749](https://github.com/danielraffel/pulp/pull/1749))

<a id="v0871"></a>
## [0.87.1] - 2026-05-10

- fix(view-bridge): Skia m144 compat + 6 silent @pulp/react event regressions ([#1792](https://github.com/danielraffel/pulp/pull/1792))

<a id="v0870"></a>
## [0.87.0] - 2026-05-10

- feat(rn): walk back 5 stale wontfix classifications to supported (rn 89.2% → 93.3%) ([#1800](https://github.com/danielraffel/pulp/pull/1800))

<a id="v0860"></a>
## [0.86.0] - 2026-05-10

- feat(view): SvgPath gradient fill via setSvgFillGradient (closes rn/fill DIVERGE → rn 100% achievable) ([#1799](https://github.com/danielraffel/pulp/pull/1799))

<a id="v0850"></a>
## [0.85.0] - 2026-05-10

- feat(view): wire CSS mask-image + mask-size paint composite (closes css/maskSize → css 100% achievable) ([#1798](https://github.com/danielraffel/pulp/pull/1798))

<a id="v0840"></a>
## [0.84.0] - 2026-05-10

- feat(view): wire Label::paint to TextShaper::layout_with_lines for CSS overflow-wrap (PR-2 of 2) ([#1797](https://github.com/danielraffel/pulp/pull/1797))

<a id="v0831"></a>
## [0.83.1] - 2026-05-10

- fix: 4 Codex sweep findings on #1791 and #1794 ([#1796](https://github.com/danielraffel/pulp/pull/1796))

<a id="v0830"></a>
## [0.83.0] - 2026-05-10

- feat(text-shaper): add BreakMode for CSS overflow-wrap (PR-1 of 2) ([#1795](https://github.com/danielraffel/pulp/pull/1795))

<a id="v0822"></a>
## [0.82.2] - 2026-05-10

- fix(view): clear input-focus slot on widget destruction (#1708) ([#1726](https://github.com/danielraffel/pulp/pull/1726))
- chore(catalog): css/touchAction noop → wontfix (architectural — pointerEvents covers hit-test contract) ([#1794](https://github.com/danielraffel/pulp/pull/1794))

<a id="v0821"></a>
## [0.82.1] - 2026-05-10

- fix(view): currentColor honors element's own text color (Codex P2 on #1728) ([#1778](https://github.com/danielraffel/pulp/pull/1778))

<a id="v0820"></a>
## [0.82.0] - 2026-05-10

- feat(rn): wire CSS font-variant to SkShaper Feature API (closes rn/fontVariant DIVERGE) ([#1791](https://github.com/danielraffel/pulp/pull/1791))
- chore(catalog): reclassify 9 css NO-OPs to wontfix (architectural) ([#1790](https://github.com/danielraffel/pulp/pull/1790))
- fix(css): preserve newlines for white-space: pre + sync legacy setter (Codex P1+P2 on #1786) ([#1788](https://github.com/danielraffel/pulp/pull/1788))
- feat(css): wire full white-space enum (closes css/whiteSpace DIVERGE) ([#1786](https://github.com/danielraffel/pulp/pull/1786))
- chore(catalog): rn/isolation noop → wontfix (architectural — no stacking-context engine) ([#1785](https://github.com/danielraffel/pulp/pull/1785))
- fix(css): tie :root pseudo to body identity, not null parent (Codex P2 on #1779) ([#1783](https://github.com/danielraffel/pulp/pull/1783))
- feat(rn): SkFontMgr fallback chain for fontFamily list (closes pulp #932 followup) ([#1781](https://github.com/danielraffel/pulp/pull/1781))
- fix(css): restore :root matcher for stylesheet rules (Codex P2 followup on #1773) ([#1779](https://github.com/danielraffel/pulp/pull/1779))
- feat(html): catalog flip — html/dialog noop → supported (html → 100%) ([#1775](https://github.com/danielraffel/pulp/pull/1775))
- fix: 4 Codex sweep findings on #1759/#1761/#1763/#1768 ([#1773](https://github.com/danielraffel/pulp/pull/1773))
- fix(a11y): surface ARIA state attributes through NSAccessibility + snapshot (Codex P1+P2 on #1766) ([#1771](https://github.com/danielraffel/pulp/pull/1771))
- test(canvas): backfill direct unit tests for draw_image_from_*_rect (PR #1739 codecov) ([#1770](https://github.com/danielraffel/pulp/pull/1770))
- feat(harness): verify-tests step — grep TEST_CASE tags + names against catalog refs ([#1768](https://github.com/danielraffel/pulp/pull/1768))
- feat(html): wire ARIA state attributes (aria-pressed/checked/disabled/hidden) ([#1766](https://github.com/danielraffel/pulp/pull/1766))
- feat(release): drop example plugin .pkg artifacts from GitHub release page (closes task #49) ([#1764](https://github.com/danielraffel/pulp/pull/1764))
- guard: strengthen pollution check with positive `project(Pulp` assertion ([#1763](https://github.com/danielraffel/pulp/pull/1763))
- guard: source-tree pollution check (prevents #1755-class CMakeLists corruption) ([#1761](https://github.com/danielraffel/pulp/pull/1761))
- feat(html): querySelector evaluates pseudo-classes spec-correctly (closes html DIVERGE) ([#1759](https://github.com/danielraffel/pulp/pull/1759))
- fix: restore CMakeLists.txt + drop pulp.toml after #1755 corruption ([#1757](https://github.com/danielraffel/pulp/pull/1757))
- feat(css): consume object-fit + object-position in ImageView::paint (closes pulp #1707) ([#1755](https://github.com/danielraffel/pulp/pull/1755))
- feat(catalog): hygiene PR — yoga 100%, 5 grace saves, 5 test-ref backfills ([#1752](https://github.com/danielraffel/pulp/pull/1752))
- fix(canvas): restore text CTM before stroke gradients (Codex P1 on #1736) ([#1750](https://github.com/danielraffel/pulp/pull/1750))
- feat(rn): catalog flip — rn/userSelect noop → supported (matches wired impl) ([#1746](https://github.com/danielraffel/pulp/pull/1746))
- feat(rn): catalog flip — rn/overflow lists all 3 RN keywords incl. scroll ([#1745](https://github.com/danielraffel/pulp/pull/1745))
- feat(canvas2d): wire putImageData 7-arg sub-rect form (issue-916) ([#1741](https://github.com/danielraffel/pulp/pull/1741))

<a id="v0810"></a>
## [0.81.0] - 2026-05-10

- feat(canvas2d): wire drawImage 9-arg sprite-sheet source-rect form (issue-916) ([#1739](https://github.com/danielraffel/pulp/pull/1739))

<a id="v0800"></a>
## [0.80.0] - 2026-05-10

- feat(canvas2d): wire fill/stroke gradients on CoreGraphics text + paths ([#1736](https://github.com/danielraffel/pulp/pull/1736))

<a id="v07925"></a>
## [0.79.25] - 2026-05-10

- feat(view): wire tick_animations into mac + android frame loops (closes pulp #1668) ([#1734](https://github.com/danielraffel/pulp/pull/1734))

<a id="v07924"></a>
## [0.79.24] - 2026-05-10

- fix(view): removeAttribute(role)/(aria-label) resets View slot (Codex P1 on #1641) ([#1733](https://github.com/danielraffel/pulp/pull/1733))

<a id="v07923"></a>
## [0.79.23] - 2026-05-10

- fix(view): querySelector handles colons inside attribute brackets (Codex P1 on #1641) ([#1732](https://github.com/danielraffel/pulp/pull/1732))

<a id="v07922"></a>
## [0.79.22] - 2026-05-10

- feat(view): rn/borderRadius % support — paint-time bounds resolution (closes pulp #1663, 5 entries) ([#1731](https://github.com/danielraffel/pulp/pull/1731))

<a id="v07921"></a>
## [0.79.21] - 2026-05-10

- feat(canvas2d): DOMMatrix mutator methods on getTransform snapshot (closes pulp #1527 followup) ([#1730](https://github.com/danielraffel/pulp/pull/1730))

<a id="v07920"></a>
## [0.79.20] - 2026-05-10

- test(harness): align canvas2d oracle with post-#1701 + post-#1724 catalog (canvas2d 83→92%) ([#1729](https://github.com/danielraffel/pulp/pull/1729))

<a id="v07919"></a>
## [0.79.19] - 2026-05-10

- feat(view): rn/outlineColor supports currentColor keyword (closes pulp #1710) ([#1728](https://github.com/danielraffel/pulp/pull/1728))

<a id="v07918"></a>
## [0.79.18] - 2026-05-10

- fix(compat): canvas2d/transform + css/grid → supported (work already wired) ([#1727](https://github.com/danielraffel/pulp/pull/1727))

<a id="v07917"></a>
## [0.79.17] - 2026-05-10

- fix(test): restore broken yoga/rn TEST_CASE bodies + arch-diverge catalog cleanup ([#1724](https://github.com/danielraffel/pulp/pull/1724))

<a id="v07916"></a>
## [0.79.16] - 2026-05-10

- feat(view/css): wire 5 ABSENT css entries — appearance + maskSize + objectFit + objectPosition + grid shorthand ([#1709](https://github.com/danielraffel/pulp/pull/1709))

<a id="v07913"></a>
## [0.79.13] - 2026-05-10

- test(css/html/canvas2d): NO-EV evidence backfill — 233 entries (closes #1711 batches #3-5) ([#1719](https://github.com/danielraffel/pulp/pull/1719))

<a id="v07912"></a>
## [0.79.12] - 2026-05-10

- test(rn): NO-EV evidence backfill — 92 entries (closes #1711 batch #2) ([#1718](https://github.com/danielraffel/pulp/pull/1718))

<a id="v07911"></a>
## [0.79.11] - 2026-05-10

- test(yoga): NO-EV evidence backfill — 47 entries (closes #1711 batch #1) ([#1717](https://github.com/danielraffel/pulp/pull/1717))
- fix(compat): rn/height vh reclassified as architectural-OOS (closes #1712) ([#1715](https://github.com/danielraffel/pulp/pull/1715))

<a id="v07910"></a>
## [0.79.10] - 2026-05-10

- fix(view/css): parse gap before clearing per-axis (Codex P2 followup on #1700, closes #1707) ([#1713](https://github.com/danielraffel/pulp/pull/1713))

<a id="v0799"></a>
## [0.79.9] - 2026-05-10

- feat(view/css): real wiring for css/userSelect (Tier-2 follow-up to #1656) ([#1703](https://github.com/danielraffel/pulp/pull/1703))

<a id="v0798"></a>
## [0.79.8] - 2026-05-09

- fix(canvas2d): transform() forwards composed matrix to bridge (closes pulp #1348 / #1666 followup) ([#1701](https://github.com/danielraffel/pulp/pull/1701))
- fix(canvas2d): rotated ellipse extends current contour (Codex P1, pulp #1556 followup) ([#1699](https://github.com/danielraffel/pulp/pull/1699))

<a id="v0796"></a>
## [0.79.6] - 2026-05-09

- fix(view): setClipPath unwraps path() + skips deferred shapes (Codex P1, pulp #1540 followup) ([#1698](https://github.com/danielraffel/pulp/pull/1698))

<a id="v0795"></a>
## [0.79.5] - 2026-05-09

- fix(view): setBoxSizing inherit walks parent chain (Codex P1, pulp #1538 followup) ([#1697](https://github.com/danielraffel/pulp/pull/1697))

<a id="v0794"></a>
## [0.79.4] - 2026-05-09

- fix(view/css): single-token gap clears per-axis row_gap/column_gap ([#1700](https://github.com/danielraffel/pulp/pull/1700))
- fix(compat): walk back 10 catalog overstatements flagged in #1616 sweep ([#1656](https://github.com/danielraffel/pulp/pull/1656))

<a id="v0790"></a>
## [0.79.0] - 2026-05-09

- feat(canvas2d): wire drop-shadow filter in SkiaCanvas + #1657 systemic controls ([#1679](https://github.com/danielraffel/pulp/pull/1679))

<a id="v0784"></a>
## [0.78.4] - 2026-05-08

- fix(cli): refresh explicit upgrade discovery ([#1686](https://github.com/danielraffel/pulp/pull/1686))

<a id="v0783"></a>
## [0.78.3] - 2026-05-08

- chore: bump SDK to v0.78.3 ([#1678](https://github.com/danielraffel/pulp/pull/1678))
- fix(ci): enable manual main cache saves ([#1683](https://github.com/danielraffel/pulp/pull/1683))
- fix(ci): avoid PR cloud cache uploads on mac runner ([#1681](https://github.com/danielraffel/pulp/pull/1681))
- feat(harness): add visual actuals and png verification ([#1631](https://github.com/danielraffel/pulp/pull/1631))
- fix(cli): install upgrade sibling payloads ([#1676](https://github.com/danielraffel/pulp/pull/1676))
- fix(compat): canvas2d/font notes — explicit architectural classification (no metric flip) ([#1674](https://github.com/danielraffel/pulp/pull/1674))
- feat(html): wire <img> through ImageView + Skia decode (closes pulp #1658) ([#1670](https://github.com/danielraffel/pulp/pull/1670))
- fix(compat): rn/boxShadow array form drift — work shipped, catalog stale (closes pulp #1664) ([#1668](https://github.com/danielraffel/pulp/pull/1668))
- fix(compat): rn textShadow* drift — work shipped, catalog stale (closes pulp #1548) ([#1661](https://github.com/danielraffel/pulp/pull/1661))
- fix(compat): correct html/img deferred-issue link to pulp #1658 ([#1659](https://github.com/danielraffel/pulp/pull/1659))
- feat(css): Wave 5 audit — re-verify the 49 PR #1649 reclassifications + wire 3 still-missing entries ([#1654](https://github.com/danielraffel/pulp/pull/1654))
- chore(html): Wave 4 cleanup — restore PR #1641 + close 5 architectural DIVERGE entries (86.4% → 96.6%) ([#1653](https://github.com/danielraffel/pulp/pull/1653))
- chore(harness): canvas2d Wave 4 cleanup — flip 4 stale partials to supported (80.3% → 86.4%) ([#1648](https://github.com/danielraffel/pulp/pull/1648))
- feat(rn): Wave 4 extensive DIVERGE sweep — close all 16 rn DIVERGE entries ([#1651](https://github.com/danielraffel/pulp/pull/1651))
- feat(css): Wave 4 css extensive — DIVERGE sweep (49 -> 0; 60.4% -> 83.5% PASS) ([#1649](https://github.com/danielraffel/pulp/pull/1649))
- feat(css): Wave 3 css.3 — animation-play-state pause/resume + #1508 P1 audit closure ([#1640](https://github.com/danielraffel/pulp/pull/1640))
- feat(canvas2d): Wave 3 real-engineering — arcTo correctness audit + fillText gradient + strokeStyle gradients ([#1643](https://github.com/danielraffel/pulp/pull/1643))
- fix: Codex P2 batch — file-isolated quality findings (pulp #1616) ([#1644](https://github.com/danielraffel/pulp/pull/1644))
- feat(html): Wave 3 — wire ARIA setters + minimal querySelector engine ([#1641](https://github.com/danielraffel/pulp/pull/1641))
- feat(css): Wave 2 — wire ~25 cheap value-coverage gaps in css surface ([#1638](https://github.com/danielraffel/pulp/pull/1638))
- fix(canvas2d): Wave 2 cheap wiring — fillRule, 4-corner roundRect, strokeText, ellipse rotation ([#1636](https://github.com/danielraffel/pulp/pull/1636))
- feat(rn): Wave 2 cheap value-coverage wiring (17 entries) ([#1634](https://github.com/danielraffel/pulp/pull/1634))
- fix(compat): Wave 1 drift cleanup + architectural reclassification ([#1632](https://github.com/danielraffel/pulp/pull/1632))
- feat(harness): account visual coverage by compat entry ([#1623](https://github.com/danielraffel/pulp/pull/1623))
- fix(compat): align rn catalog mapsTo with shipped bridge fns (drift cleanup, refs pulp #1434) ([#1627](https://github.com/danielraffel/pulp/pull/1627))
- feat(canvas2d): close 3 DIVERGE→PASS gaps + wire fillRule arg ([#1625](https://github.com/danielraffel/pulp/pull/1625))
- feat(html): close 7 of 14 DIVERGE→PASS gaps in JS-shim sweep ([#1624](https://github.com/danielraffel/pulp/pull/1624))
- feat(yoga): close all 14 DIVERGE→PASS gaps in one sweep ([#1622](https://github.com/danielraffel/pulp/pull/1622))
- fix(harness): close 7 NOT-IMPL drift entries by reconciling catalog/oracle (pulp #1615) ([#1619](https://github.com/danielraffel/pulp/pull/1619))
- ci(visual-harness): include core/view, core/render, compat.json, adapters in path filter (pulp #1598 review r3200704130) ([#1618](https://github.com/danielraffel/pulp/pull/1618))
- fix(test): use pid+chrono+counter for temp WAV paths to avoid ctest -j parallel collisions (pulp #1257 review r3175949820) ([#1617](https://github.com/danielraffel/pulp/pull/1617))
- feat(css): close 30 css NOT-IMPL entries (A4 Bundles 2-7, refs pulp #1434) ([#1613](https://github.com/danielraffel/pulp/pull/1613))
- feat(rn): close 8 rn NOT-IMPL entries — prop-applier wiring + catalog hygiene (A4 Bundle 1, refs pulp #1434) ([#1611](https://github.com/danielraffel/pulp/pull/1611))
- audit(yoga): flip boxSizing + flexBasis% to supported (closes pulp #1545) ([#1568](https://github.com/danielraffel/pulp/pull/1568))
- fix(canvas2d): fillText/strokeText maxWidth + true outlined glyphs (closes pulp #1525) ([#1555](https://github.com/danielraffel/pulp/pull/1555))
- fix(css): mask + clip-path cluster — paint-time SkPath::FromSVGString clip + storage-only mask slots (closes pulp #1515) ([#1540](https://github.com/danielraffel/pulp/pull/1540))
- feat(view/text): RTL text shaping + bidi flow (Phase A2-3, refs pulp #1434) ([#1506](https://github.com/danielraffel/pulp/pull/1506))
- fix(view): PulpView isOpaque=YES so NSWindow.backgroundColor doesn't flash on hover (closes #1382) ([#1386](https://github.com/danielraffel/pulp/pull/1386))
- fix(rn): wire mixBlendMode (RN 0.76 New Arch) (closes pulp #1549) ([#1570](https://github.com/danielraffel/pulp/pull/1570))
- test(inspect): cover domain dispatch edge cases ([#1263](https://github.com/danielraffel/pulp/pull/1263))
- test: cover Claude bundle edge parsing ([#1269](https://github.com/danielraffel/pulp/pull/1269))
- fix(canvas): override concat_transform on CoreGraphicsCanvas (#943 / #933 P1) ([#963](https://github.com/danielraffel/pulp/pull/963))
- feat(view/css): css animations + transitions subsystem (Phase A2-1, refs pulp #1434) ([#1508](https://github.com/danielraffel/pulp/pull/1508))
- fix(coverage): remove unread filters/exclude_filters keys from coverage_config.json ([#1152](https://github.com/danielraffel/pulp/pull/1152))
- chore(shipyard): bump mac target timeout to 3600s ([#792](https://github.com/danielraffel/pulp/pull/792))
- fix(css): list-style cluster — type/image/position bridge wiring (refs pulp #1514) ([#1532](https://github.com/danielraffel/pulp/pull/1532))
- fix(css): box-sizing wired to Yoga 3.x YGNodeStyleSetBoxSizing (closes pulp #1516) ([#1538](https://github.com/danielraffel/pulp/pull/1538))
- audit(rn): reclassify shadow* + isolation as wontfix (closes pulp #1546) ([#1565](https://github.com/danielraffel/pulp/pull/1565))
- fix(view/css): wire lineClamp + webkitLineClamp + backgroundRepeat (closes pulp #1552) ([#1573](https://github.com/danielraffel/pulp/pull/1573))
- fix(canvas2d): native arc/arcTo/ellipse/roundRect via SkPath::arcTo + SkRRect (pulp #1521 — DIVERGE → PASS) ([#1556](https://github.com/danielraffel/pulp/pull/1556))
- chore(compat): rn catalog hygiene partial -> supported (closes pulp #1550) ([#1567](https://github.com/danielraffel/pulp/pull/1567))
- fix(css): catalog 13 already-implemented features (closes pulp #1551) ([#1574](https://github.com/danielraffel/pulp/pull/1574))
- fix(cli): make pulp upgrade refresh GitHub synchronously when cache is stale (closes pulp #1599) ([#1600](https://github.com/danielraffel/pulp/pull/1600))
- docs: document flex+grid layout model rationale (refs #1434) ([#1583](https://github.com/danielraffel/pulp/pull/1583))
- ci: flip macOS resolver to provider mode + decouple LFS from checkout (closes #155 followup) ([#1594](https://github.com/danielraffel/pulp/pull/1594))
- fix(coverage): classify dsl/apple/android into tiers + lock in completeness ([#1154](https://github.com/danielraffel/pulp/pull/1154))
- test(platform): cover file dialog backend replacement ([#1256](https://github.com/danielraffel/pulp/pull/1256))
- test(format): cover test signal file playback edges ([#1257](https://github.com/danielraffel/pulp/pull/1257))
- test(events): cover IPC endpoint parse edges ([#1266](https://github.com/danielraffel/pulp/pull/1266))
- test(cli): cover create target edge formats ([#1271](https://github.com/danielraffel/pulp/pull/1271))
- test(cli): cover package target compatibility warning ([#1273](https://github.com/danielraffel/pulp/pull/1273))
- test(cli): cover ship sign missing identity ([#1274](https://github.com/danielraffel/pulp/pull/1274))
- test: add Phase 3 Android MIDI manager coverage ([#1275](https://github.com/danielraffel/pulp/pull/1275))
- test(platform): cover popup menu fallback paths ([#1276](https://github.com/danielraffel/pulp/pull/1276))
- ci: harden doctor and android coverage checks ([#1278](https://github.com/danielraffel/pulp/pull/1278))
- test(android): cover audio controller lifecycle edges ([#1279](https://github.com/danielraffel/pulp/pull/1279))
- test(audio): cover mmap reader edge paths ([#1280](https://github.com/danielraffel/pulp/pull/1280))
- test(view): cover text editor navigation edges ([#1282](https://github.com/danielraffel/pulp/pull/1282))
- test(bindings): cover python helper edges ([#1283](https://github.com/danielraffel/pulp/pull/1283))
- test(view): cover splash screen lifecycle ([#1285](https://github.com/danielraffel/pulp/pull/1285))
- test(runtime): cover named pipe edge paths ([#1286](https://github.com/danielraffel/pulp/pull/1286))
- test(cli): cover audio command parser edges ([#1287](https://github.com/danielraffel/pulp/pull/1287))
- ci(perf): fast-CI / full-CI split via ctest labels (closes pulp #1589) ([#1592](https://github.com/danielraffel/pulp/pull/1592))
- test(visual): add yoga layout snapshot harness ([#1598](https://github.com/danielraffel/pulp/pull/1598))
- chore(visual): add deterministic visual harness smoke ([#1580](https://github.com/danielraffel/pulp/pull/1580))
- fix(yoga): wire borderWidth + 4 per-edge variants to YGNodeStyleSetBorder (closes pulp #1543) ([#1566](https://github.com/danielraffel/pulp/pull/1566))
- fix(canvas2d): catalog hygiene — wire `tests` field for the 10-entry already-supported set (closes pulp #1526) ([#1560](https://github.com/danielraffel/pulp/pull/1560))
- feat(view/css): css/filter chain beyond blur (Phase A2-4, refs pulp #1434) ([#1503](https://github.com/danielraffel/pulp/pull/1503))
- ci: route macOS leg to local self-hosted runner via repo var; Linux + Windows on GitHub-hosted ([#1585](https://github.com/danielraffel/pulp/pull/1585))
- ci(perf): bump ctest -j8; persist self-hosted workspace; skip example plugins on PR; per-OS alias gates ([#1588](https://github.com/danielraffel/pulp/pull/1588))
- fix(rn): textDecoration cluster + textAlignVertical at JSX layer (closes pulp #1547) ([#1564](https://github.com/danielraffel/pulp/pull/1564))
- fix(css): reclassify 8 entries wontfix/missing → noop (closes pulp #1528) ([#1561](https://github.com/danielraffel/pulp/pull/1561))
- fix(harness): yoga catalog — promote `flex` and `flexFlow` shorthands (closes pulp #1544) ([#1563](https://github.com/danielraffel/pulp/pull/1563))
- fix(canvas2d): wire fillRule arg through fill() and clip() (refs pulp #1522) ([#1554](https://github.com/danielraffel/pulp/pull/1554))
- feat(canvas2d): wire getTransform + isPointInPath + isPointInStroke (closes pulp #1527) ([#1559](https://github.com/danielraffel/pulp/pull/1559))
- fix(css): background sub-properties — Attachment / Clip / Origin (closes pulp #1517) ([#1531](https://github.com/danielraffel/pulp/pull/1531))
- fix(rn): wire `flex: <number>` shorthand to grow/shrink/basis (closes pulp #1518) ([#1530](https://github.com/danielraffel/pulp/pull/1530))
- feat(view/yoga): logical-edge fan-out — start/end + marginStart/End + paddingStart/End (closes pulp #1542) ([#1562](https://github.com/danielraffel/pulp/pull/1562))
- fix(canvas2d): CG-degraded gradient/pattern cluster — proper CGShading callbacks (closes pulp #1524) ([#1557](https://github.com/danielraffel/pulp/pull/1557))
- feat(view): unblock fontFamily JSX dispatch + container cascade (pulp #1434 A2-5) ([#1512](https://github.com/danielraffel/pulp/pull/1512))
- fix(rn): outline cluster — Color/Offset/Style/Width paint-time ring (closes pulp #1519) ([#1533](https://github.com/danielraffel/pulp/pull/1533))
- feat(canvas2d): wire ctx.direction + ctx.filter — close last canvas2d NOT-IMPLs (closes pulp #1520) ([#1535](https://github.com/danielraffel/pulp/pull/1535))
- fix(import-ir): codex post-merge audit follow-up — anchor dedupe + parser advance + ESM artifact (#1499 follow-up) ([#1523](https://github.com/danielraffel/pulp/pull/1523))
- fix(view/css): cursor enum + userSelect/pointerEvents trim + flexWrap reverse (Triage #7+#12+#13+#14, refs pulp #1434) ([#1493](https://github.com/danielraffel/pulp/pull/1493))
- feat(canvas2d): parse full CSS font shorthand (style/variant/weight/lineHeight) (refs pulp #1434) ([#1495](https://github.com/danielraffel/pulp/pull/1495))
- fix(rn): wire {margin,padding,inset}{Start,End,InlineStart,InlineEnd} as LTR-only fast path (refs pulp #1434, sub-agent #27 finding) ([#1498](https://github.com/danielraffel/pulp/pull/1498))
- feat(view/css): css grid layout extension (Phase A2-2 PR 1, refs pulp #1434) ([#1509](https://github.com/danielraffel/pulp/pull/1509))
- feat(canvas2d): wire createPattern -> SkShader::MakeImage / CG fallback (refs pulp #1434) ([#1492](https://github.com/danielraffel/pulp/pull/1492))
- feat(view/css+rn): transform function fan-out — scaleX/Y, skewX/Y, rotateX/Y/Z, matrix3d (refs pulp #1434, Triage #9) ([#1487](https://github.com/danielraffel/pulp/pull/1487))

<a id="v0782"></a>
## [0.78.2] - 2026-05-06

- feat(import-ir): Phase 1 spike — IR types + Claude Design HTML adapter + 5-scenario harness (closes pulp #1486 phase 1) ([#1499](https://github.com/danielraffel/pulp/pull/1499))
- feat(view): yoga alignContent + width/height auto (sub-agent #12 followup, refs pulp #1434) ([#1494](https://github.com/danielraffel/pulp/pull/1494))
- fix(rn): wire backfaceVisibility / cursor / filter / pointerEvents / textTransform / transformOrigin / userSelect (refs pulp #1434, sub-agent #27 finding) ([#1497](https://github.com/danielraffel/pulp/pull/1497))
- feat(view): borderStyle dashed/dotted/double/groove/ridge renderer (refs pulp #1434, Triage #10/#13) ([#1490](https://github.com/danielraffel/pulp/pull/1490))
- feat(view/css): parse oklch / oklab / lch / lab / color() (refs pulp #1434, Triage #8) ([#1481](https://github.com/danielraffel/pulp/pull/1481))
- feat(view): yoga per-edge margin/padding accept percent + auto (cross-surface, refs pulp #1434) ([#1477](https://github.com/danielraffel/pulp/pull/1477))
- feat(canvas2d): wire conicGradient + imageSmoothing + miterLimit bridge setters (refs pulp #1434) ([#1480](https://github.com/danielraffel/pulp/pull/1480))
- feat(harness): add 'noop' catalog status — close css/animation* drift (closes pulp #1475) ([#1478](https://github.com/danielraffel/pulp/pull/1478))
- fix(harness): html catalog cleanup — push surface to 100% (refs pulp #1434) ([#1479](https://github.com/danielraffel/pulp/pull/1479))
- fix(harness): css catalog hygiene — 8 stale entries, drift 60 → 54 (refs pulp #1434) ([#1474](https://github.com/danielraffel/pulp/pull/1474))
- fix(harness): canvas2d catalog hygiene — flip 5 over-claimed entries to partial (refs pulp #1434) ([#1472](https://github.com/danielraffel/pulp/pull/1472))
- feat(rn): walk transform array → bridge translate/rotate/scale calls (refs pulp #1434, Triage #9) ([#1468](https://github.com/danielraffel/pulp/pull/1468))

<a id="v0781"></a>
## [0.78.1] - 2026-05-05

- fix(cli): bake SDK version into Rust release binary ([#1466](https://github.com/danielraffel/pulp/pull/1466))
- fix(view/text): textAlign 'auto' + 'justify' (refs pulp #1434 Triage #11) ([#1458](https://github.com/danielraffel/pulp/pull/1458))
- fix(view/css): parse boxShadow shorthand → setBoxShadow bridge (refs pulp #1434, Triage #15) ([#1461](https://github.com/danielraffel/pulp/pull/1461))
- fix(view): yoga value-aliasing for flex-start/flex-end + column/row-reverse + baseline (closes pulp #1434 rn batch B) ([#1452](https://github.com/danielraffel/pulp/pull/1452))

<a id="v0780"></a>
## [0.78.0] - 2026-05-05

- feature/phase8 default flip prepped ([#1450](https://github.com/danielraffel/pulp/pull/1450))
- fix(rn): wire display: flex / none to existing visible/yoga path (refs pulp #1434) ([#1456](https://github.com/danielraffel/pulp/pull/1456))
- fix(rn): forward percent strings + 'auto' for width/height/min/max/flexBasis (rn Batch C, refs pulp #1434) ([#1455](https://github.com/danielraffel/pulp/pull/1455))
- fix(view/css): yoga top/right/bottom/left percent (closes pulp #1434 batch 6) ([#1451](https://github.com/danielraffel/pulp/pull/1451))
- fix(harness/rn): catalog hygiene for stale NOT-IMPL entries (refs pulp #1434) ([#1449](https://github.com/danielraffel/pulp/pull/1449))
- feat(canvas2d): sticky shadow cluster — color/blur/offsetX/offsetY (closes pulp #1434 batch 7) ([#1446](https://github.com/danielraffel/pulp/pull/1446))
- validate/phase8 integrated ([#1005](https://github.com/danielraffel/pulp/pull/1005))
- fix(view/css): yoga RN-alias fan-out (closes pulp #1434 batch 4) ([#1445](https://github.com/danielraffel/pulp/pull/1445))
- fix(view/css): wire backdropFilter, textDecoration longhands, and fontWeight keywords (pulp #1434 batch 3) ([#1444](https://github.com/danielraffel/pulp/pull/1444))

<a id="v0770"></a>
## [0.77.0] - 2026-05-05

- fix(view/css): translate width/height percent values through to Yoga (closes pulp #1423) ([#1426](https://github.com/danielraffel/pulp/pull/1426))
- codex/shipyard rust docs ([#1441](https://github.com/danielraffel/pulp/pull/1441))
- feat(yoga): aspect-ratio support across FlexStyle, CSS shim, and @pulp/react (pulp #1434 batch 5) ([#1437](https://github.com/danielraffel/pulp/pull/1437))
- feat(view/css): translate `white-space: nowrap` to native single-line layout (closes #1410) ([#1411](https://github.com/danielraffel/pulp/pull/1411))
- fix(harness): catalog-hygiene sweep + css OOS reclassification (refs pulp #1434) ([#1436](https://github.com/danielraffel/pulp/pull/1436))
- feat(harness): rn surface adapter via auto-discover (closes #1392 partial, #1393 partial) ([#1396](https://github.com/danielraffel/pulp/pull/1396))
- feat(@pulp/react): SvgRect + SvgLine intrinsics (closes pulp #1416) ([#1421](https://github.com/danielraffel/pulp/pull/1421))
- fix(harness/yoga): claim display:flex+none, add inline-block/inline-flex routing (closes pulp #1420) ([#1422](https://github.com/danielraffel/pulp/pull/1422))
- feat(harness): css surface adapter via auto-discover (closes #1392 partial, #1393 partial) ([#1397](https://github.com/danielraffel/pulp/pull/1397))
- feat(harness): canvas2d surface adapter via auto-discover (closes #1392 partial, #1393 partial) ([#1399](https://github.com/danielraffel/pulp/pull/1399))
- feat(harness): html surface adapter via auto-discover (closes #1392 partial, #1393 partial) ([#1398](https://github.com/danielraffel/pulp/pull/1398))
- fix(view): host idle pumps must drain timers, not just rAF (pulp #1412) ([#1425](https://github.com/danielraffel/pulp/pull/1425))
- feat(harness): auto-discover surface adapters via pkgutil + decorator (closes #1401) ([#1415](https://github.com/danielraffel/pulp/pull/1415))
- test(view): regression guard for overflow:hidden parent clip-rect (refs pulp #1409) ([#1414](https://github.com/danielraffel/pulp/pull/1414))
- fix(view/css): translate `text-overflow: ellipsis` to native truncation (closes #1407) ([#1408](https://github.com/danielraffel/pulp/pull/1408))
- fix(harness/yoga): normalize annotated catalog values before exact-string match ([#1417](https://github.com/danielraffel/pulp/pull/1417))

<a id="v0760"></a>
## [0.76.0] - 2026-05-04

- feat(hooks): pre-push detects shipyard-vs-direct push origin (pulp #1406) ([#1406](https://github.com/danielraffel/pulp/pull/1406))
- fix(view/ios): wire IOSGpuWindowHost::set_idle_callback into CADisplayLink (closes pulp #1402 — iOS slice) ([#1405](https://github.com/danielraffel/pulp/pull/1405))
- feat(harness): yoga surface adapter + first machine-derived coverage (closes #1391, #1392 partial, #1393 partial, #1394 partial) ([#1395](https://github.com/danielraffel/pulp/pull/1395))
- fix(view/mac): wire MacGpuWindowHost::set_idle_callback into CVDisplayLink (closes pulp #1387 gap #3) ([#1400](https://github.com/danielraffel/pulp/pull/1400))
- fix(view/android): pump WidgetBridge idle each vsync (closes pulp #1402 — Android slice) ([#1404](https://github.com/danielraffel/pulp/pull/1404))
- fix(view): NaN/±Inf defense in CanvasWidget::add_command (closes pulp #1387 gap #2) ([#1389](https://github.com/danielraffel/pulp/pull/1389))

<a id="v0756"></a>
## [0.75.6] - 2026-05-04

- fix(@pulp/react): prop-applier auto-arms registerPointer/Wheel + routes overflow (closes #1381, #1387 gaps #1+#4) ([#1388](https://github.com/danielraffel/pulp/pull/1388))

<a id="v0755"></a>
## [0.75.5] - 2026-05-04

- fix(view/mac): PulpMetalView opaque + wantsUpdateLayer (closes pulp #1382) ([#1385](https://github.com/danielraffel/pulp/pull/1385))

<a id="v0754"></a>
## [0.75.4] - 2026-05-04

- fix(canvas/mac): CoreGraphicsCanvas::set_blend_mode honors all BlendMode values (closes pulp #1371) ([#1377](https://github.com/danielraffel/pulp/pull/1377))

<a id="v0752"></a>
## [0.75.2] - 2026-05-04

- fix(view): position:absolute children skip flex-line participation (closes pulp #1379, #998) ([#1380](https://github.com/danielraffel/pulp/pull/1380))

<a id="v0751"></a>
## [0.75.1] - 2026-05-04

- fix(view): UTF-8-safe JS prelude embedding (closes pulp #1382) ([#1383](https://github.com/danielraffel/pulp/pull/1383))
- fix(ci): retry-on-fail Yoga priming + asset watchdog for release-cli (closes #1375) ([#1378](https://github.com/danielraffel/pulp/pull/1378))
- docs(skills): codify Canvas2D bridge gotchas for importer + shim authors ([#1376](https://github.com/danielraffel/pulp/pull/1376))

<a id="v0750"></a>
## [0.75.0] - 2026-05-04

- fix(view): instrument CanvasWidget::paint + layout repro test for #1368 round 2 ([#1370](https://github.com/danielraffel/pulp/pull/1370))

<a id="v0741"></a>
## [0.74.1] - 2026-05-04

- fix(view): per-CanvasWidget save_layer for HTML <canvas> sibling isolation (closes pulp #1368) ([#1372](https://github.com/danielraffel/pulp/pull/1372))

<a id="v0740"></a>
## [0.74.0] - 2026-05-04

- fix(view): canvas widget paint balances save/restore (closes pulp #1368) ([#1369](https://github.com/danielraffel/pulp/pull/1369))

<a id="v0730"></a>
## [0.73.0] - 2026-05-03

- fix(view): overlay auto-dismisses on ESC + outside-click (closes pulp #1361) ([#1365](https://github.com/danielraffel/pulp/pull/1365))

<a id="v0726"></a>
## [0.72.6] - 2026-05-03

- fix(format): StandaloneApp honors view_size().min_* (closes pulp #1362) ([#1364](https://github.com/danielraffel/pulp/pull/1364))
- audit(compat): refresh matrix from spec walk + post-#1027 surface ([#1366](https://github.com/danielraffel/pulp/pull/1366))
- feat(@pulp/react): synthesize event for JSX handlers (closes pulp #1352) ([#1363](https://github.com/danielraffel/pulp/pull/1363))

<a id="v0725"></a>
## [0.72.5] - 2026-05-03

- fix(canvas/mac): shape fills honor active gradient (closes pulp #1359) ([#1360](https://github.com/danielraffel/pulp/pull/1360))

<a id="v0724"></a>
## [0.72.4] - 2026-05-03

- fix(view/skia): shape fills honor active gradient_shader_ (closes #1350) ([#1353](https://github.com/danielraffel/pulp/pull/1353))
- fix(view): inline <svg width/height> attrs reserve flex layout space (closes pulp #1147) ([#1347](https://github.com/danielraffel/pulp/pull/1347))
- feat(view): translate CSS :hover rules to mouseenter/mouseleave listeners (closes #1323, #1149 part b) ([#1345](https://github.com/danielraffel/pulp/pull/1345))
- fix(view): wire Canvas2D shim methods so FilterBank renders (closes pulp #1346 + augments #1322) ([#1348](https://github.com/danielraffel/pulp/pull/1348))

<a id="v0723"></a>
## [0.72.3] - 2026-05-03

- fix(canvas/mac): CoreGraphicsCanvas implements path / gradient / transform (closes pulp #1322) ([#1329](https://github.com/danielraffel/pulp/pull/1329))
- fix(view): auto-claim active_overlay from CSS position:absolute + high z-index (pulp #1148 slice b) ([#1344](https://github.com/danielraffel/pulp/pull/1344))

<a id="v0722"></a>
## [0.72.2] - 2026-05-03

- fix(view): make overflow:visible hit_test extension symmetric on all 4 sides (pulp #1148 slice a) ([#1343](https://github.com/danielraffel/pulp/pull/1343))
- fix(view): overlay_contains walks overflow:visible children (closes pulp #1320) ([#1328](https://github.com/danielraffel/pulp/pull/1328))
- fix(view, mac): NSWindow resize triggers root View relayout (closes pulp #1321) ([#1327](https://github.com/danielraffel/pulp/pull/1327))
- fix(ci): remove silent-no-op patterns from skill_path_map.json (closes #1053) ([#1325](https://github.com/danielraffel/pulp/pull/1325))
- fix(ci): thread lcov --remove through coverage pipeline so LCOV_EXCL markers propagate (closes #1058) ([#1326](https://github.com/danielraffel/pulp/pull/1326))
- fix(ci): make codecov component classification mutually exclusive (closes #1055) ([#1324](https://github.com/danielraffel/pulp/pull/1324))
- fix(ci): add core/dsl/** to infrastructure tier (closes #1056) ([#1319](https://github.com/danielraffel/pulp/pull/1319))
- fix(ci): per-surface Version-Bump: skip requires non-empty reason (closes #1054) ([#1318](https://github.com/danielraffel/pulp/pull/1318))
- fix(ci): flip pre-push gates from advisory to enforcing by default (closes #1144) ([#1317](https://github.com/danielraffel/pulp/pull/1317))

<a id="v0721"></a>
## [0.72.1] - 2026-05-03

- fix(view): overlay fast-path honors hit_test guards (Codex P1 #1313 + P2 #1314) ([#1316](https://github.com/danielraffel/pulp/pull/1316))
- fix(ci): require non-empty Version-Bump skip reason in auto-release watchdog (Codex P2 on #1310) ([#1315](https://github.com/danielraffel/pulp/pull/1315))

<a id="v0720"></a>
## [0.72.0] - 2026-05-03

- feat(view): generalize overlay-click routing for React popovers (closes pulp #1148) ([#1297](https://github.com/danielraffel/pulp/pull/1297))

<a id="v0710"></a>
## [0.71.0] - 2026-05-03

- feat(import-design): wire @pulp/css-adapt into emitted output (#1039) ([#1060](https://github.com/danielraffel/pulp/pull/1060))
- docs(skill/import-design): record v0.69 audit lessons + #1148 pattern ([#1298](https://github.com/danielraffel/pulp/pull/1298))
- fix(ci): auto-release.yml stranded-fix watchdog must honor Version-Bump: skip (closes #1308) ([#1310](https://github.com/danielraffel/pulp/pull/1310))

<a id="v0700"></a>
## [0.70.0] - 2026-05-03

- fix(view, ci): Codex P2 sweep — 4 follow-ups bundled (closes #1171) ([#1296](https://github.com/danielraffel/pulp/pull/1296))
- fix(pulp-react): externalize react/reconciler/scheduler in dist build (pulp #1292) ([#1295](https://github.com/danielraffel/pulp/pull/1295))
- feat(view): @pulp/react SvgPath intrinsic (pulp #994) ([#1291](https://github.com/danielraffel/pulp/pull/1291))

<a id="v0692"></a>
## [0.69.2] - 2026-05-03

- fix(view): ScrollView::hit_test honors pointer_events (Codex P1, closes #1170) ([#1289](https://github.com/danielraffel/pulp/pull/1289))
- fix(view): parse font-family CSS lists and pick first non-empty family (pulp #1151) ([#1174](https://github.com/danielraffel/pulp/pull/1174))
- feat(cli): versioned import-design detection (pulp #1031) ([#1047](https://github.com/danielraffel/pulp/pull/1047))
- test(midi): cover UMP helper factories ([#1277](https://github.com/danielraffel/pulp/pull/1277))
- test(cli): cover version diag file-reader edges ([#1281](https://github.com/danielraffel/pulp/pull/1281))
- test(cli): cover common config edge paths ([#1268](https://github.com/danielraffel/pulp/pull/1268))
- test(view): cover widget bridge gpu fallbacks ([#1267](https://github.com/danielraffel/pulp/pull/1267))
- test: cover python binding edge paths ([#1264](https://github.com/danielraffel/pulp/pull/1264))
- fix(ship): tolerate malformed appcast lengths ([#1262](https://github.com/danielraffel/pulp/pull/1262))
- test(canvas): cover CPU fallback helpers ([#1265](https://github.com/danielraffel/pulp/pull/1265))
- test: cover scan blacklist edge cases ([#1255](https://github.com/danielraffel/pulp/pull/1255))
- test(cli): cover import-design tool shellout paths ([#1272](https://github.com/danielraffel/pulp/pull/1272))
- test(format): cover remote view session edges ([#1258](https://github.com/danielraffel/pulp/pull/1258))
- test: cover widget paint edge paths ([#1270](https://github.com/danielraffel/pulp/pull/1270))
- test(platform): cover nested permission override clear ([#1249](https://github.com/danielraffel/pulp/pull/1249))
- test: cover i18n parser edge cases ([#1259](https://github.com/danielraffel/pulp/pull/1259))
- test(events): cover action broadcaster missing listener removal ([#1254](https://github.com/danielraffel/pulp/pull/1254))

<a id="v0691"></a>
## [0.69.1] - 2026-05-02

- fix(events): deactivate one-shot timers after firing ([#1253](https://github.com/danielraffel/pulp/pull/1253))
- test(format): cover settings panel interactions ([#1260](https://github.com/danielraffel/pulp/pull/1260))
- test: cover js engine recommendation edges ([#1261](https://github.com/danielraffel/pulp/pull/1261))
- test: cover workflow lint wiring ([#1251](https://github.com/danielraffel/pulp/pull/1251))
- test(view): cover app framework edge paths ([#1247](https://github.com/danielraffel/pulp/pull/1247))
- test(ci): improve local-ci helper coverage ([#1246](https://github.com/danielraffel/pulp/pull/1246))
- test: cover theme contrast helper edges ([#1252](https://github.com/danielraffel/pulp/pull/1252))
- test(coverage): harden Codecov config invariants ([#1250](https://github.com/danielraffel/pulp/pull/1250))
- test(audio): cover coreaudio format registry edges ([#1248](https://github.com/danielraffel/pulp/pull/1248))
- test(view): cover asset manager edge paths ([#1245](https://github.com/danielraffel/pulp/pull/1245))
- test: cover mkdocs hook branches ([#1244](https://github.com/danielraffel/pulp/pull/1244))
- test(tools): cover pulp sandbox helpers ([#1243](https://github.com/danielraffel/pulp/pull/1243))
- test(tools): cover format validation checker ([#1242](https://github.com/danielraffel/pulp/pull/1242))
- test(tools): complete docs generator coverage ([#1241](https://github.com/danielraffel/pulp/pull/1241))
- test(view): cover theme validation and file edges ([#1240](https://github.com/danielraffel/pulp/pull/1240))
- test(ci): cover skill sync helper edges ([#1231](https://github.com/danielraffel/pulp/pull/1231))
- test(runtime): cover websocket frame edge paths ([#1239](https://github.com/danielraffel/pulp/pull/1239))
- test(view): cover eq curve edge interactions ([#1238](https://github.com/danielraffel/pulp/pull/1238))
- test(ci): cover lcov cobertura edges ([#1237](https://github.com/danielraffel/pulp/pull/1237))
- test(ci): cover cobertura merge edges ([#1236](https://github.com/danielraffel/pulp/pull/1236))
- test(packages): cover freshness check edge paths ([#1235](https://github.com/danielraffel/pulp/pull/1235))
- test(ci): cover runs-on resolver edges ([#1234](https://github.com/danielraffel/pulp/pull/1234))
- test(tools): cover cmajor external edges ([#1233](https://github.com/danielraffel/pulp/pull/1233))
- test(packages): cover registry validator edges ([#1232](https://github.com/danielraffel/pulp/pull/1232))
- test: cover build migration index edge paths ([#1230](https://github.com/danielraffel/pulp/pull/1230))
- test: cover validate_hosts tooling ([#1229](https://github.com/danielraffel/pulp/pull/1229))
- test: cover top-level audit tool ([#1228](https://github.com/danielraffel/pulp/pull/1228))
- test(local-ci): cover android target helper ([#1227](https://github.com/danielraffel/pulp/pull/1227))
- test(tools): complete limitations coverage ([#1226](https://github.com/danielraffel/pulp/pull/1226))
- test: cover compat sync edge paths ([#1225](https://github.com/danielraffel/pulp/pull/1225))
- test(ci): cover cli sync checker edges ([#1224](https://github.com/danielraffel/pulp/pull/1224))
- test: cover add-component tool ([#1223](https://github.com/danielraffel/pulp/pull/1223))
- test(ci): cover swift coverage runner edges ([#1222](https://github.com/danielraffel/pulp/pull/1222))
- test(deps): cover dependency audit edges ([#1221](https://github.com/danielraffel/pulp/pull/1221))
- test(ci): cover version bump check edges ([#1220](https://github.com/danielraffel/pulp/pull/1220))
- test: cover run_python_coverage entrypoint ([#1219](https://github.com/danielraffel/pulp/pull/1219))
- test: add docs-sync edge coverage ([#1218](https://github.com/danielraffel/pulp/pull/1218))
- test: cover auto-release decision CLI edges ([#1215](https://github.com/danielraffel/pulp/pull/1215))
- test(tools): cover package cli edges ([#1217](https://github.com/danielraffel/pulp/pull/1217))
- test(tools): cover status ladder edge paths ([#1216](https://github.com/danielraffel/pulp/pull/1216))
- test(tools): cover jsfx subset edges ([#1214](https://github.com/danielraffel/pulp/pull/1214))
- test(ci): cover coverage diff comment edges ([#1213](https://github.com/danielraffel/pulp/pull/1213))
- test(tools): cover iwyu annotate edges ([#1212](https://github.com/danielraffel/pulp/pull/1212))
- test(tools): cover binary data encoder ([#1207](https://github.com/danielraffel/pulp/pull/1207))
- test: cover embed_js generator ([#1211](https://github.com/danielraffel/pulp/pull/1211))
- test: add coverage tier check edge coverage ([#1210](https://github.com/danielraffel/pulp/pull/1210))
- test: cover mkdocs hooks ([#1209](https://github.com/danielraffel/pulp/pull/1209))
- test(tools): cover docs generator paths ([#1208](https://github.com/danielraffel/pulp/pull/1208))
- test(tools): cover docs consistency checks ([#1206](https://github.com/danielraffel/pulp/pull/1206))
- test(canvas): cover fallback drawing edges ([#1201](https://github.com/danielraffel/pulp/pull/1201))
- test(audio): cover system volume command edges ([#1202](https://github.com/danielraffel/pulp/pull/1202))
- test(canvas): cover sdf atlas cache lifecycle edges ([#1198](https://github.com/danielraffel/pulp/pull/1198))
- test(canvas): cover msdf and psdf atlas edges ([#1197](https://github.com/danielraffel/pulp/pull/1197))
- test(canvas): cover sdf atlas api edges ([#1196](https://github.com/danielraffel/pulp/pull/1196))
- test(runtime): cover gzip header rejection paths ([#1078](https://github.com/danielraffel/pulp/pull/1078))
- test(tools): cover bench diff reporting paths ([#1205](https://github.com/danielraffel/pulp/pull/1205))
- test(canvas): cover image convolution edge paths ([#1200](https://github.com/danielraffel/pulp/pull/1200))
- test(render): cover gpu graph helper state paths ([#1096](https://github.com/danielraffel/pulp/pull/1096))
- test(platform): cover registry fallback stubs ([#1083](https://github.com/danielraffel/pulp/pull/1083))
- test(canvas): cover sdf text helper edges ([#1195](https://github.com/danielraffel/pulp/pull/1195))
- test(runtime): cover license and analytics edges ([#1115](https://github.com/danielraffel/pulp/pull/1115))
- test(canvas): cover rectangle list edge no-ops ([#1203](https://github.com/danielraffel/pulp/pull/1203))
- test(events): cover async helper edge paths ([#1088](https://github.com/danielraffel/pulp/pull/1088))
- test(view): cover toolbar and breadcrumb interactions ([#1097](https://github.com/danielraffel/pulp/pull/1097))
- test(cli): cover create fail-fast edge paths ([#1104](https://github.com/danielraffel/pulp/pull/1104))
- test(runtime): cover expression evaluator edge paths ([#1113](https://github.com/danielraffel/pulp/pull/1113))
- test(signal): cover interpolator endpoints and impulses ([#1116](https://github.com/danielraffel/pulp/pull/1116))
- test(view): cover asset manager edge paths ([#1125](https://github.com/danielraffel/pulp/pull/1125))
- test(view): cover widget bridge native event edges ([#1194](https://github.com/danielraffel/pulp/pull/1194))
- test(canvas): cover sdf software renderer edge paths ([#1199](https://github.com/danielraffel/pulp/pull/1199))
- test(canvas): cover attributed string layout edges ([#1204](https://github.com/danielraffel/pulp/pull/1204))
- test(ci): cover lcov cobertura edge paths ([#1117](https://github.com/danielraffel/pulp/pull/1117))
- test(view): cover design import edge paths ([#1072](https://github.com/danielraffel/pulp/pull/1072))
- test(audio): cover buffering reader tail lifecycle paths ([#1121](https://github.com/danielraffel/pulp/pull/1121))
- test(view): cover frame clock lifecycle edges ([#1126](https://github.com/danielraffel/pulp/pull/1126))
- test(runtime): cover file and library helper edges ([#1114](https://github.com/danielraffel/pulp/pull/1114))
- test(render): cover draw batcher merge blockers ([#1122](https://github.com/danielraffel/pulp/pull/1122))
- test(osc): cover malformed bundle edges ([#1118](https://github.com/danielraffel/pulp/pull/1118))
- test(audio): cover workgroup fallback lifecycle ([#1128](https://github.com/danielraffel/pulp/pull/1128))
- test(view): cover svg path parser edge cases ([#1191](https://github.com/danielraffel/pulp/pull/1191))
- test(render): cover texture atlas full-capacity edges ([#1127](https://github.com/danielraffel/pulp/pull/1127))
- test(events): cover volume detector edge paths ([#1079](https://github.com/danielraffel/pulp/pull/1079))
- test(midi): cover factory data byte bounds ([#1062](https://github.com/danielraffel/pulp/pull/1062))
- test(view): cover table model edge bookkeeping ([#1188](https://github.com/danielraffel/pulp/pull/1188))
- test(view): cover file browser edge paths ([#1089](https://github.com/danielraffel/pulp/pull/1089))
- test(state): cover binding gesture edges ([#1185](https://github.com/danielraffel/pulp/pull/1185))
- test(runtime): cover memory message channel edges ([#1184](https://github.com/danielraffel/pulp/pull/1184))
- test(canvas): cover text layout edge paths ([#1186](https://github.com/danielraffel/pulp/pull/1186))
- test(canvas): cover svg edge paths ([#1187](https://github.com/danielraffel/pulp/pull/1187))
- fix(signal): handle degenerate polynomial roots ([#1051](https://github.com/danielraffel/pulp/pull/1051))
- test(canvas): cover path to sdf guard edges ([#1190](https://github.com/danielraffel/pulp/pull/1190))
- fix(signal): clamp meter process channel counts ([#1066](https://github.com/danielraffel/pulp/pull/1066))
- test(midi): cover sysex accumulator tail paths ([#1129](https://github.com/danielraffel/pulp/pull/1129))
- test(signal): cover fast math boundary cutoffs ([#1189](https://github.com/danielraffel/pulp/pull/1189))
- test(audio): cover compressed reader rejection ([#1074](https://github.com/danielraffel/pulp/pull/1074))
- test(audio): cover hotplug callback edges ([#1086](https://github.com/danielraffel/pulp/pull/1086))
- test(audio): cover channel set edge paths ([#1133](https://github.com/danielraffel/pulp/pull/1133))
- test(cli): cover host wrapper edge paths ([#1075](https://github.com/danielraffel/pulp/pull/1075))
- test(render): cover gpu compute pool bookkeeping ([#1143](https://github.com/danielraffel/pulp/pull/1143))
- test(platform): cover progress parser delimiter edges ([#1137](https://github.com/danielraffel/pulp/pull/1137))
- test(events): cover service discovery edge paths ([#1045](https://github.com/danielraffel/pulp/pull/1045))
- test(host): cover background scanner restart after cancel ([#1071](https://github.com/danielraffel/pulp/pull/1071))
- test(packages): cover validation tool edge paths ([#1142](https://github.com/danielraffel/pulp/pull/1142))
- fix(midi): clear stale running status on undefined system bytes ([#1102](https://github.com/danielraffel/pulp/pull/1102))
- test(state): cover undo history edge cases ([#1119](https://github.com/danielraffel/pulp/pull/1119))
- test(signal): cover meter reset and clip edges ([#1134](https://github.com/danielraffel/pulp/pull/1134))
- test(events): cover ipc error state edges ([#1139](https://github.com/danielraffel/pulp/pull/1139))
- test(signal): cover processor chain reset edges ([#1135](https://github.com/danielraffel/pulp/pull/1135))
- test(render): cover render loop edge paths ([#1082](https://github.com/danielraffel/pulp/pull/1082))
- docs: codify coverage no-idle loop ([#1138](https://github.com/danielraffel/pulp/pull/1138))
- fix(audio): guard load measurer invalid timing inputs ([#1085](https://github.com/danielraffel/pulp/pull/1085))
- test(tools): cover status ladder checker ([#1140](https://github.com/danielraffel/pulp/pull/1140))
- test(format): cover descriptor validation edges ([#1120](https://github.com/danielraffel/pulp/pull/1120))
- test(cli): cover package registry cache fallbacks ([#1136](https://github.com/danielraffel/pulp/pull/1136))
- test(midi): cover sysex sidecar lifecycle ([#1132](https://github.com/danielraffel/pulp/pull/1132))
- test(view): cover split and concertina layout widgets ([#1099](https://github.com/danielraffel/pulp/pull/1099))
- test: cover host scan cache fallback paths ([#1123](https://github.com/danielraffel/pulp/pull/1123))
- test(runtime): cover text diff edge paths ([#1084](https://github.com/danielraffel/pulp/pull/1084))
- test(audio): cover excerpt window boundary arithmetic ([#1131](https://github.com/danielraffel/pulp/pull/1131))
- test(ship): cover appcast malformed optional fields ([#1141](https://github.com/danielraffel/pulp/pull/1141))
- test(state): cover state tree listener detach edges ([#1112](https://github.com/danielraffel/pulp/pull/1112))
- test(runtime): cover json-rpc edge paths ([#1111](https://github.com/danielraffel/pulp/pull/1111))
- test(midi): cover MPE voice tracker tail paths ([#1110](https://github.com/danielraffel/pulp/pull/1110))
- test(state): cover preset manager edge paths ([#1109](https://github.com/danielraffel/pulp/pull/1109))
- test(format): cover AAX model validation edges ([#1108](https://github.com/danielraffel/pulp/pull/1108))
- test(host): cover graph serializer edge paths ([#1107](https://github.com/danielraffel/pulp/pull/1107))
- test(runtime): cover URL parser edge paths ([#1106](https://github.com/danielraffel/pulp/pull/1106))
- fix(canvas): normalize wrapped color hues ([#1105](https://github.com/danielraffel/pulp/pull/1105))
- test(view): cover preset browser edge paths ([#1103](https://github.com/danielraffel/pulp/pull/1103))
- test(view): cover accessibility tree edges ([#1101](https://github.com/danielraffel/pulp/pull/1101))
- test(view): cover code editor edge paths ([#1100](https://github.com/danielraffel/pulp/pull/1100))
- test(view): cover table list edge paths ([#1098](https://github.com/danielraffel/pulp/pull/1098))
- fix(view): cover property list edge paths ([#1095](https://github.com/danielraffel/pulp/pull/1095))
- test(local-ci): cover deterministic helper edges ([#1094](https://github.com/danielraffel/pulp/pull/1094))
- test(view): cover graph editor edge paths ([#1093](https://github.com/danielraffel/pulp/pull/1093))
- test(cli): cover inspect command edge paths ([#1092](https://github.com/danielraffel/pulp/pull/1092))
- test(view): cover ui component edge paths ([#1091](https://github.com/danielraffel/pulp/pull/1091))
- fix(view): harden color picker edge paths ([#1090](https://github.com/danielraffel/pulp/pull/1090))
- test(runtime): cover ip address helpers ([#1087](https://github.com/danielraffel/pulp/pull/1087))
- test(midi): cover midi file edge paths ([#1081](https://github.com/danielraffel/pulp/pull/1081))
- test(runtime): cover base64 decode edge paths ([#1080](https://github.com/danielraffel/pulp/pull/1080))
- test(view): cover widget bridge raf cancellation ([#1065](https://github.com/danielraffel/pulp/pull/1065))

<a id="v0690"></a>
## [0.69.0] - 2026-04-30

- feat(canvas): public font-registration API for plugins (pulp #1150) ([#1175](https://github.com/danielraffel/pulp/pull/1175))
- fix(view): preserve unset border attributes when setting just one (audit PR #1166) ([#1169](https://github.com/danielraffel/pulp/pull/1169))
- fix(view): wire prop-applier to call registerHover for hover handlers (pulp #1149) ([#1173](https://github.com/danielraffel/pulp/pull/1173))
- fix(view): default display:flex to flex-direction:row to match CSS web compat (#1147) ([#1167](https://github.com/danielraffel/pulp/pull/1167))
- audit(compat): populate compat.json from Yoga + RN + MDN spec walk (#1027) ([#1166](https://github.com/danielraffel/pulp/pull/1166))

<a id="v0681"></a>
## [0.68.1] - 2026-04-30

- feat(view): RN style-prop bridge primitives (pulp #1026) ([#1044](https://github.com/danielraffel/pulp/pull/1044))
- test(cli): cover mcp server protocol paths ([#1076](https://github.com/danielraffel/pulp/pull/1076))
- test(view): cover button widget edge paths ([#1069](https://github.com/danielraffel/pulp/pull/1069))
- fix(audio): make empty focus subscriptions inert ([#1032](https://github.com/danielraffel/pulp/pull/1032))
- ci(compat): seed compat-sync gate (#1029) ([#1068](https://github.com/danielraffel/pulp/pull/1068))
- test(render): cover gpu surface state edges ([#1046](https://github.com/danielraffel/pulp/pull/1046))
- test(platform): cover child process edge paths ([#1021](https://github.com/danielraffel/pulp/pull/1021))
- test(signal): cover biquad edge paths ([#1077](https://github.com/danielraffel/pulp/pull/1077))
- test(cli): cover projects registry edge paths ([#1025](https://github.com/danielraffel/pulp/pull/1025))
- test(render): cover atlas cache edge paths ([#1064](https://github.com/danielraffel/pulp/pull/1064))
- test(cli): cover docs search edge paths ([#1063](https://github.com/danielraffel/pulp/pull/1063))
- test(audio): cover subsection reader edges ([#1061](https://github.com/danielraffel/pulp/pull/1061))
- test(runtime): cover range edge paths ([#1034](https://github.com/danielraffel/pulp/pull/1034))

<a id="v0680"></a>
## [0.68.0] - 2026-04-30

- fix(view): bubble click up to ancestor on_click handler (#1067) ([#1073](https://github.com/danielraffel/pulp/pull/1073))
- test(view): cover text editor edge paths ([#1023](https://github.com/danielraffel/pulp/pull/1023))
- docs: codify phase 3 coverage operating loop ([#1030](https://github.com/danielraffel/pulp/pull/1030))
- test(audio): cover BufferView clear accessors ([#1033](https://github.com/danielraffel/pulp/pull/1033))

<a id="v0670"></a>
## [0.67.0] - 2026-04-30

- feat(cli): emit classnames.json from import-design --from claude (#1035) ([#1048](https://github.com/danielraffel/pulp/pull/1048))
- framework: import @pulp/react package source into monorepo (#1042) ([#1050](https://github.com/danielraffel/pulp/pull/1050))
- test(host): cover LV2 plugin slot happy path ([#1028](https://github.com/danielraffel/pulp/pull/1028))
- test(view): cover plugin manager panel edges ([#1024](https://github.com/danielraffel/pulp/pull/1024))
- test(events): cover IPC socket edge paths ([#1022](https://github.com/danielraffel/pulp/pull/1022))
- test(cli): cover tooling edge paths ([#1016](https://github.com/danielraffel/pulp/pull/1016))
- ci(release): three-layer guard against stranded fix/feat merges (#1009) ([#1013](https://github.com/danielraffel/pulp/pull/1013))

<a id="v0660"></a>
## [0.66.0] - 2026-04-30

- fix(view): register canvasFillRect bridge name (pulp #964) ([#1012](https://github.com/danielraffel/pulp/pull/1012))
- test(host): cover graph serializer edge paths ([#1017](https://github.com/danielraffel/pulp/pull/1017))
- test(cli): cover package cmake lifecycle ([#1019](https://github.com/danielraffel/pulp/pull/1019))
- test(midi): cover mpe voice tracker tail cases ([#1018](https://github.com/danielraffel/pulp/pull/1018))
- test(cli): cover update check parser edges ([#1014](https://github.com/danielraffel/pulp/pull/1014))
- fix(cli): reject partial run frame counts ([#1000](https://github.com/danielraffel/pulp/pull/1000))
- test(cli): cover project command edge paths ([#999](https://github.com/danielraffel/pulp/pull/999))
- test(cli): cover package registry cache edges ([#997](https://github.com/danielraffel/pulp/pull/997))
- fix(view): on(id,'click',fn) auto-wires View::on_click (pulp #1006) ([#1008](https://github.com/danielraffel/pulp/pull/1008))

<a id="v0650"></a>
## [0.65.0] - 2026-04-29

- fix(view): canvasRect/canvasStrokeRect honour active fillStyle when no color arg (pulp #968) ([#1003](https://github.com/danielraffel/pulp/pull/1003))

<a id="v0640"></a>
## [0.64.0] - 2026-04-29

- feat(view): RangeSlider widget for HTML <input type="range"> (#966) ([#1004](https://github.com/danielraffel/pulp/pull/1004))

<a id="v0630"></a>
## [0.63.0] - 2026-04-29

- feat(view): CSS-style typography inheritance for Label (pulp #969) ([#1002](https://github.com/danielraffel/pulp/pull/1002))

<a id="v0620"></a>
## [0.62.0] - 2026-04-29

- fix(view): honor z_index() and default overflow to visible (#972) ([#996](https://github.com/danielraffel/pulp/pull/996))
- fix(view): validate _dragTarget against live tree before deref (#992) ([#1001](https://github.com/danielraffel/pulp/pull/1001))
- test(ci): cover local ci pure helper paths ([#989](https://github.com/danielraffel/pulp/pull/989))

<a id="v0610"></a>
## [0.61.0] - 2026-04-29

- framework/spectr parity 965 ([#991](https://github.com/danielraffel/pulp/pull/991))
- test(cli): cover cached cli common helper paths ([#988](https://github.com/danielraffel/pulp/pull/988))
- test(view): lock in transparent View+CanvasWidget contract (#964 / #967) ([#973](https://github.com/danielraffel/pulp/pull/973))
- test(view): cover widget bridge canvas edge APIs ([#987](https://github.com/danielraffel/pulp/pull/987))
- test(view): cover view core edge paths ([#985](https://github.com/danielraffel/pulp/pull/985))
- test(view): cover widget bridge asset loading edges ([#986](https://github.com/danielraffel/pulp/pull/986))
- test(view): cover audio widget meter edges ([#984](https://github.com/danielraffel/pulp/pull/984))
- test(view): cover ui component interaction edges ([#983](https://github.com/danielraffel/pulp/pull/983))
- test(view): cover widget bridge runtime helpers ([#982](https://github.com/danielraffel/pulp/pull/982))
- test(view): cover design import codegen edges ([#981](https://github.com/danielraffel/pulp/pull/981))
- test(view): cover widget schema paint edges ([#980](https://github.com/danielraffel/pulp/pull/980))
- test(view): cover widget bridge event and style APIs ([#979](https://github.com/danielraffel/pulp/pull/979))
- test(cli): cover validate command edge paths ([#978](https://github.com/danielraffel/pulp/pull/978))
- test(cli): cover tool registry edge paths ([#977](https://github.com/danielraffel/pulp/pull/977))
- test(cli): cover package command edge paths ([#976](https://github.com/danielraffel/pulp/pull/976))
- test(cli): cover cli common edge helpers ([#975](https://github.com/danielraffel/pulp/pull/975))
- test(inspect): cover inspector window and domain edges ([#974](https://github.com/danielraffel/pulp/pull/974))
- docs: refresh coverage compliance handoff ([#774](https://github.com/danielraffel/pulp/pull/774))
- test(signal): cover meter channel-count guards ([#851](https://github.com/danielraffel/pulp/pull/851))
- test(audio): cover frame-fill invalid dimensions ([#853](https://github.com/danielraffel/pulp/pull/853))
- test(ios): cover audio session event labels ([#886](https://github.com/danielraffel/pulp/pull/886))
- test(tools): cover version bump helper paths ([#971](https://github.com/danielraffel/pulp/pull/971))
- test(tools): cover runner resolver edge paths ([#850](https://github.com/danielraffel/pulp/pull/850))
- test(bindings): cover embedded python state paths ([#867](https://github.com/danielraffel/pulp/pull/867))
- test(view): cover label paint branches ([#849](https://github.com/danielraffel/pulp/pull/849))
- test(cli): cover JSFX subset validation errors ([#871](https://github.com/danielraffel/pulp/pull/871))
- test(format): cover host type policies ([#870](https://github.com/danielraffel/pulp/pull/870))
- test(render): cover KTX2 decoder header edges ([#869](https://github.com/danielraffel/pulp/pull/869))
- test(signal): cover processor duplicator edge paths ([#868](https://github.com/danielraffel/pulp/pull/868))
- test(audio): cover excerpt window exact tails ([#866](https://github.com/danielraffel/pulp/pull/866))
- test(android): cover audio engine default config ([#865](https://github.com/danielraffel/pulp/pull/865))
- test(swift): cover bridge parameter edge paths ([#864](https://github.com/danielraffel/pulp/pull/864))
- test(state): cover StateStore helper paths ([#863](https://github.com/danielraffel/pulp/pull/863))
- test(tools): cover lcov cobertura utility ([#862](https://github.com/danielraffel/pulp/pull/862))
- test(format): cover descriptor validation edges ([#861](https://github.com/danielraffel/pulp/pull/861))
- test(midi): cover sysex accumulator abort edges ([#860](https://github.com/danielraffel/pulp/pull/860))
- test(platform): cover progress parser edge cases ([#859](https://github.com/danielraffel/pulp/pull/859))
- test(render): cover draw batcher edge cases ([#858](https://github.com/danielraffel/pulp/pull/858))
- test(events): cover async helper edge cases ([#857](https://github.com/danielraffel/pulp/pull/857))
- test(tools): cover lcov cobertura helper edges ([#856](https://github.com/danielraffel/pulp/pull/856))
- test(signal): cover convolver edge paths ([#855](https://github.com/danielraffel/pulp/pull/855))
- test(format): cover ViewBridge helper edges ([#854](https://github.com/danielraffel/pulp/pull/854))
- test(canvas): cover rectangle list helper edges ([#852](https://github.com/danielraffel/pulp/pull/852))
- test(runtime): cover base64 padding validation ([#848](https://github.com/danielraffel/pulp/pull/848))
- test(events): cover NSD and async helper edges ([#847](https://github.com/danielraffel/pulp/pull/847))
- test(audio): cover audio focus token edge paths ([#846](https://github.com/danielraffel/pulp/pull/846))
- test(host): cover parameter event queue contracts ([#845](https://github.com/danielraffel/pulp/pull/845))
- test(midi): cover MPE tracker UMP edge paths ([#843](https://github.com/danielraffel/pulp/pull/843))
- test(events): cover child process manager edges ([#840](https://github.com/danielraffel/pulp/pull/840))
- test(canvas): cover text layout cursor helpers ([#873](https://github.com/danielraffel/pulp/pull/873))
- test(ship): cover NSIS uninstall metadata ([#875](https://github.com/danielraffel/pulp/pull/875))
- test(osc): cover malformed codec argument guards ([#876](https://github.com/danielraffel/pulp/pull/876))
- test(runtime): cover scope guard and string copy edges ([#877](https://github.com/danielraffel/pulp/pull/877))
- test(inspect): cover audio inspector domain paths ([#878](https://github.com/danielraffel/pulp/pull/878))
- test(tools): cover limitations helper ([#879](https://github.com/danielraffel/pulp/pull/879))
- test: cover audio model missing checkpoint state ([#880](https://github.com/danielraffel/pulp/pull/880))
- test(runtime): cover XML and gzip helper edges ([#881](https://github.com/danielraffel/pulp/pull/881))
- test(platform): cover environment edge helpers ([#882](https://github.com/danielraffel/pulp/pull/882))
- test(runtime): cover stream edge paths ([#883](https://github.com/danielraffel/pulp/pull/883))
- test(web-compat): cover selector helper matching ([#884](https://github.com/danielraffel/pulp/pull/884))
- test(state): cover properties file helper paths ([#885](https://github.com/danielraffel/pulp/pull/885))
- test(ship): cover appcast helper edges ([#887](https://github.com/danielraffel/pulp/pull/887))
- test(tools): cover skill sync helper paths ([#888](https://github.com/danielraffel/pulp/pull/888))
- test(midi): cover UMP packet helper masking ([#889](https://github.com/danielraffel/pulp/pull/889))
- test(view): cover accessibility tree range snapshots ([#890](https://github.com/danielraffel/pulp/pull/890))
- test(android): cover accessibility delegate fallback ([#891](https://github.com/danielraffel/pulp/pull/891))
- test(view): cover image cache failure edges ([#892](https://github.com/danielraffel/pulp/pull/892))
- test(audio): cover buffering reader no-callback path ([#893](https://github.com/danielraffel/pulp/pull/893))
- test(runtime): cover crypto helper edge cases ([#894](https://github.com/danielraffel/pulp/pull/894))
- test(host): cover plugin bundle suffix helper ([#895](https://github.com/danielraffel/pulp/pull/895))

<a id="v0600"></a>
## [0.60.0] - 2026-04-28

- fix(canvas): CoreGraphicsCanvas::concat_transform composes onto CTM (Codex P1 on #933) ([#961](https://github.com/danielraffel/pulp/pull/961))
- fix(coverage): include non-test binaries in local diff-cover object set ([#959](https://github.com/danielraffel/pulp/pull/959))
- fix(view): wire platform font manager into TextShaper (pulp #945) ([#957](https://github.com/danielraffel/pulp/pull/957))
- fix(view): land outset shadow + overflow clip inside the view's saveLayer (#936 P1 / #949) ([#955](https://github.com/danielraffel/pulp/pull/955))

<a id="v0590"></a>
## [0.59.0] - 2026-04-28

- feat(canvas): register external/fonts/*.ttf with SkFontMgr at startup (#932) ([#956](https://github.com/danielraffel/pulp/pull/956))
- test(view): cover canvasSetLineDash JS bridge fn (#952) ([#954](https://github.com/danielraffel/pulp/pull/954))

<a id="v0580"></a>
## [0.58.0] - 2026-04-28

- feat(cli): pulp loop — leveraged-prototype focus mode (Slice 1 of #940) ([#950](https://github.com/danielraffel/pulp/pull/950))

<a id="v0570"></a>
## [0.57.0] - 2026-04-28

- fix(view): canvas widget transparent by default + real clearRect (#929) ([#934](https://github.com/danielraffel/pulp/pull/934))

<a id="v0560"></a>
## [0.56.0] - 2026-04-28

- feat(view): setBoxShadow JS bridge + draw_box_shadow Canvas primitive (#925) ([#936](https://github.com/danielraffel/pulp/pull/936))
- feat(canvas): setBackdropFilter for frosted-glass overlays (#926) ([#937](https://github.com/danielraffel/pulp/pull/937))
- fix(view): Label auto-grows to fit text content (#928) ([#935](https://github.com/danielraffel/pulp/pull/935))

<a id="v0550"></a>
## [0.55.0] - 2026-04-28

- feat(view): Label honors setFontFamily / setFontWeight / setLetterSpacing (#927) ([#938](https://github.com/danielraffel/pulp/pull/938))

<a id="v0540"></a>
## [0.54.0] - 2026-04-28

- feat(view): setTransform(id, a, b, c, d, e, f) on View — full 2D affine matrix (#930) ([#933](https://github.com/danielraffel/pulp/pull/933))
- fix(view): __requestFrame__ calls request_repaint() to drive the paint loop (#921) ([#923](https://github.com/danielraffel/pulp/pull/923))

<a id="v0530"></a>
## [0.53.0] - 2026-04-28

- feat(coverage): local diff-coverage check mirroring CI's gate ([#919](https://github.com/danielraffel/pulp/pull/919))

<a id="v0520"></a>
## [0.52.0] - 2026-04-28

- feat(canvas2d): close API gaps — measureText, drawImage, setLineDash, getImageData, putImageData ([#920](https://github.com/danielraffel/pulp/pull/920))
- feat(cli): plumb --headless / --screenshot / --frames / --watch on `pulp run` (#914) ([#917](https://github.com/danielraffel/pulp/pull/917))

<a id="v0510"></a>
## [0.51.0] - 2026-04-28

- feat(view): register web-API globals natively (#915) ([#918](https://github.com/danielraffel/pulp/pull/918))

<a id="v0500"></a>
## [0.50.0] - 2026-04-28

- fix(view): auto-wire WidgetBridge repaint callback to root.request_repaint() (#899) ([#913](https://github.com/danielraffel/pulp/pull/913))
- feat(build): bridge experimental/pulp-rs into the CMake build graph ([#778](https://github.com/danielraffel/pulp/pull/778))
- fix(view): bound QuickJS pump_message_loop against runaway microtasks (#902) ([#909](https://github.com/danielraffel/pulp/pull/909))
- test(cli): isolate ship shell-out tests from user signing config (#901) ([#910](https://github.com/danielraffel/pulp/pull/910))
- fix(ci): install PyYAML before coverage_tier_check tests (#900) ([#907](https://github.com/danielraffel/pulp/pull/907))
- fix(cmake): install binary-data encoder alongside PulpUtils.cmake (Codex P1 on #905) ([#908](https://github.com/danielraffel/pulp/pull/908))

<a id="v0490"></a>
## [0.49.0] - 2026-04-27

- fix(canvas): compose canvasSetTransform onto parent View transform (Codex P1 on #897) ([#906](https://github.com/danielraffel/pulp/pull/906))
- perf(cmake): rewrite pulp_add_binary_data with Python encoder + add_custom_command (#898) ([#905](https://github.com/danielraffel/pulp/pull/905))

<a id="v0480"></a>
## [0.48.0] - 2026-04-27

- feat(view): add canvasSetTransform / canvasClip / canvasGlobalCompositeOperation (#896) ([#897](https://github.com/danielraffel/pulp/pull/897))
- fix(cli): MSVCRT-correct shell_quote for Windows backslash paths (#776) ([#872](https://github.com/danielraffel/pulp/pull/872))
- fix(view): drain QuickJS jobs to empty (Codex P2 on #769) (#769) ([#874](https://github.com/danielraffel/pulp/pull/874))
- docs(ci): route shipyard pin bumps through shipyard pin bump ([#844](https://github.com/danielraffel/pulp/pull/844))
- test(cli): cover ship command validation paths ([#835](https://github.com/danielraffel/pulp/pull/835))
- fix(coverage): classify inspect Codecov surface ([#842](https://github.com/danielraffel/pulp/pull/842))
- test(platform): cover child process edge paths ([#832](https://github.com/danielraffel/pulp/pull/832))
- fix(render): harden GPU graph data setters ([#839](https://github.com/danielraffel/pulp/pull/839))
- test(tools): cover CLI release packager ([#838](https://github.com/danielraffel/pulp/pull/838))
- test(signal): cover filter design edge paths ([#833](https://github.com/danielraffel/pulp/pull/833))
- test(midi): cover MIDI-CI edge paths ([#829](https://github.com/danielraffel/pulp/pull/829))
- test(events): cover event loop lifecycle edges ([#834](https://github.com/danielraffel/pulp/pull/834))
- test(midi): cover RPN parser edge paths ([#830](https://github.com/danielraffel/pulp/pull/830))
- test(packages): cover registry validation tools ([#837](https://github.com/danielraffel/pulp/pull/837))
- test(tools): cover CLI sync checker ([#836](https://github.com/danielraffel/pulp/pull/836))
- test(signal): cover utility helper edges ([#831](https://github.com/danielraffel/pulp/pull/831))
- fix(midi): reject malformed raw MIDI data bytes ([#828](https://github.com/danielraffel/pulp/pull/828))
- test(audio): cover file helper edges ([#822](https://github.com/danielraffel/pulp/pull/822))
- test(audio): cover OGG reader edges ([#826](https://github.com/danielraffel/pulp/pull/826))
- test(signal): cover multi-channel meter edges ([#827](https://github.com/danielraffel/pulp/pull/827))
- test(signal): cover DSP helper edges ([#825](https://github.com/danielraffel/pulp/pull/825))
- fix(signal): zero-initialize Mat3 multiplication ([#824](https://github.com/danielraffel/pulp/pull/824))
- test(platform): cover helper edge paths ([#823](https://github.com/danielraffel/pulp/pull/823))
- test(signal): cover spectrogram edge paths ([#820](https://github.com/danielraffel/pulp/pull/820))
- test(midi): cover keyboard and sequence edges ([#819](https://github.com/danielraffel/pulp/pull/819))
- test(audio): cover reader helper edges ([#821](https://github.com/danielraffel/pulp/pull/821))
- test(midi): cover UMP conversion edges ([#817](https://github.com/danielraffel/pulp/pull/817))
- test(midi): cover MPE synth voice edges ([#818](https://github.com/danielraffel/pulp/pull/818))
- test(midi): cover running status parser edges ([#816](https://github.com/danielraffel/pulp/pull/816))
- test(midi): cover MPE tracker edge paths ([#815](https://github.com/danielraffel/pulp/pull/815))
- test(cli): cover pr command dispatch ([#814](https://github.com/danielraffel/pulp/pull/814))
- test(signal): cover dynamics helper edges ([#813](https://github.com/danielraffel/pulp/pull/813))
- test(cli): cover project command dispatch ([#811](https://github.com/danielraffel/pulp/pull/811))
- test(platform): cover environment diff edges ([#810](https://github.com/danielraffel/pulp/pull/810))
- test(signal): cover fft helper edges ([#809](https://github.com/danielraffel/pulp/pull/809))
- test(midi): cover MIDI file edge round trips ([#808](https://github.com/danielraffel/pulp/pull/808))
- test(audio): cover channel set helpers ([#807](https://github.com/danielraffel/pulp/pull/807))
- test(audio): cover mapped reader and offline processing ([#806](https://github.com/danielraffel/pulp/pull/806))
- test(cli): cover common helper edges ([#805](https://github.com/danielraffel/pulp/pull/805))
- test(signal): cover oversampling helper edges ([#804](https://github.com/danielraffel/pulp/pull/804))
- fix(render): harden texture atlas edge cases ([#801](https://github.com/danielraffel/pulp/pull/801))
- test(signal): cover matrix helper edges ([#803](https://github.com/danielraffel/pulp/pull/803))
- test(cli): cover create shellout scaffolding ([#802](https://github.com/danielraffel/pulp/pull/802))
- test(events): cover volume detector lifecycle edges ([#795](https://github.com/danielraffel/pulp/pull/795))
- test(tools): cover version and skill gate helpers ([#800](https://github.com/danielraffel/pulp/pull/800))
- test(cli): cover docs shellout reader paths ([#799](https://github.com/danielraffel/pulp/pull/799))
- test(audio): cover format registry dispatch paths ([#798](https://github.com/danielraffel/pulp/pull/798))
- test(audio): cover streaming WAV writer edges ([#797](https://github.com/danielraffel/pulp/pull/797))
- test(cli): cover tool registry local paths ([#796](https://github.com/danielraffel/pulp/pull/796))
- test(audio): cover AIFF reader edge paths ([#794](https://github.com/danielraffel/pulp/pull/794))
- test(cli): cover package registry helpers ([#793](https://github.com/danielraffel/pulp/pull/793))
- test(events): cover socket IPC message flow ([#788](https://github.com/danielraffel/pulp/pull/788))
- test(ship): cover android package helpers ([#789](https://github.com/danielraffel/pulp/pull/789))

<a id="v0471"></a>
## [0.47.1] - 2026-04-25

- fix(project): close standalone bump edge cases ([#770](https://github.com/danielraffel/pulp/pull/770))
- test(render): cover dirty tracker and render loop edges ([#786](https://github.com/danielraffel/pulp/pull/786))
- test(format): cover VST3 adapter process path ([#782](https://github.com/danielraffel/pulp/pull/782))
- test(host): cover real clap slot paths ([#777](https://github.com/danielraffel/pulp/pull/777))
- test(view): cover widget bridge extended controls ([#771](https://github.com/danielraffel/pulp/pull/771))
- fix(view): consolidate web-compat-dom-ops single source of truth (#745) ([#773](https://github.com/danielraffel/pulp/pull/773))
- fix(view): drain QuickJS microtasks in pump_message_loop (#746) ([#769](https://github.com/danielraffel/pulp/pull/769))

<a id="v0470"></a>
## [0.47.0] - 2026-04-25

- feat: harden project SDK bump flow ([#735](https://github.com/danielraffel/pulp/pull/735))
- fix(import-design): use runtime gzip_decompress for bundle assets — unblocks ReactDOM + Babel-standalone ([#760](https://github.com/danielraffel/pulp/pull/760))

<a id="v0460"></a>
## [0.46.0] - 2026-04-24

- feat(doctor): validator discovery + auto-heal broken-signature pluginval/clap-validator/auval (#743) ([#749](https://github.com/danielraffel/pulp/pull/749))

<a id="v0451"></a>
## [0.45.1] - 2026-04-24

- feat(import-design): execute inline template scripts (Babel-standalone) (#758) ([#759](https://github.com/danielraffel/pulp/pull/759))

<a id="v0450"></a>
## [0.45.0] - 2026-04-24

- feat(doctor): pulp doctor --caches + build/test preflight (#744) ([#753](https://github.com/danielraffel/pulp/pull/753))
- test(host): add audio-thread handshake to second hot-reload race test (#669) ([#754](https://github.com/danielraffel/pulp/pull/754))
- feat(test): sandbox E2E harness for CLI + Claude plugin (#732) ([#736](https://github.com/danielraffel/pulp/pull/736))
- fix(clap): filter non-core event namespaces + loop state_save on short writes ([#748](https://github.com/danielraffel/pulp/pull/748))
- ci/namespace default ([#752](https://github.com/danielraffel/pulp/pull/752))
- fix(runtime): make gzip_compress/decompress actually handle gzip (RFC 1952) ([#747](https://github.com/danielraffel/pulp/pull/747))
- feat(view): --execute-bundle harness for Claude Design import (pulp #468) ([#731](https://github.com/danielraffel/pulp/pull/731))

<a id="v0430"></a>
## [0.43.0] - 2026-04-24

- feat(view): web-compat shims for bundled-React imports (pulp #468 PR 1/2) ([#730](https://github.com/danielraffel/pulp/pull/730))
- chore: bump Shipyard pin v0.40.0 to v0.46.0 ([#719](https://github.com/danielraffel/pulp/pull/719))

<a id="v0421"></a>
## [0.42.1] - 2026-04-24

- test: split state and ship hardening from #647 ([#666](https://github.com/danielraffel/pulp/pull/666))
- fix(release): add contents:write to sign-and-release macOS job (#724) ([#725](https://github.com/danielraffel/pulp/pull/725))
- coverage: lift events async updater support ([#648](https://github.com/danielraffel/pulp/pull/648))

<a id="v0420"></a>
## [0.42.0] - 2026-04-24

- feat(view): add EditorBridge detach_webview ([#728](https://github.com/danielraffel/pulp/pull/728))
- coverage: add local cli package command tests ([#649](https://github.com/danielraffel/pulp/pull/649))
- docs(coverage): rebaseline phase 1 status ([#723](https://github.com/danielraffel/pulp/pull/723))

<a id="v0411"></a>
## [0.41.1] - 2026-04-24

- fix(release): skip validation tests in sign-and-release ctest (#720) ([#721](https://github.com/danielraffel/pulp/pull/721))
- chore: bump Shipyard pin v0.40.0 to v0.46.0 ([#718](https://github.com/danielraffel/pulp/pull/718))
- ci(coverage): normalize Python coverage surface ([#715](https://github.com/danielraffel/pulp/pull/715))

<a id="v0410"></a>
## [0.41.0] - 2026-04-24

- feat(view): add pulp::view::EditorBridge for renderer-agnostic editor↔processor JSON dispatch (#709, unblocks #468) ([#711](https://github.com/danielraffel/pulp/pull/711))
- fix(events): Timer UAF — move mutable state into shared_ptr<Impl> (#716) ([#717](https://github.com/danielraffel/pulp/pull/717))
- test(coverage): surface bindings/python on Codecov ([#680](https://github.com/danielraffel/pulp/pull/680))
- test(coverage): surface PulpBridge in native lane ([#678](https://github.com/danielraffel/pulp/pull/678))
- ci(coverage): widen Python perimeter representation ([#677](https://github.com/danielraffel/pulp/pull/677))
- test: use ASCII hyphens in TEST_CASE names — no em-dashes (#707) ([#710](https://github.com/danielraffel/pulp/pull/710))
- chore: bump Shipyard pin v0.38.0 → v0.40.0 (closes ssh-windows bundle-apply split-brain) ([#702](https://github.com/danielraffel/pulp/pull/702))

<a id="v0402"></a>
## [0.40.2] - 2026-04-23

- fix/release vst3 tag drift ([#701](https://github.com/danielraffel/pulp/pull/701))
- test(render): extend atlas/gpu-graph/render-loop coverage (#646) ([#700](https://github.com/danielraffel/pulp/pull/700))

<a id="v0401"></a>
## [0.40.1] - 2026-04-23

- codex/release 0.40.1 ([#698](https://github.com/danielraffel/pulp/pull/698))
- chore: bump Shipyard pin v0.37.0 → v0.38.0 + revert chcp workaround ([#699](https://github.com/danielraffel/pulp/pull/699))
- codex/issue 695 release sdk webview ([#696](https://github.com/danielraffel/pulp/pull/696))
- fix(test): keep Catch2 failure output in POSIX stress loop (#669) ([#676](https://github.com/danielraffel/pulp/pull/676))
- ci(windows): set UTF-8 code page before ctest on Namespace runners ([#697](https://github.com/danielraffel/pulp/pull/697))
- codex/issue 682 cli sdk compat gate ([#691](https://github.com/danielraffel/pulp/pull/691))

<a id="v0400"></a>
## [0.40.0] - 2026-04-23

- codex/issue 661 windowhost content size ([#670](https://github.com/danielraffel/pulp/pull/670))
- codex/issue 664 app icon pipeline ([#675](https://github.com/danielraffel/pulp/pull/675))
- harden(cli): curl timeouts + PULP_DEBUG phase markers (#682) ([#690](https://github.com/danielraffel/pulp/pull/690))

<a id="v0390"></a>
## [0.39.0] - 2026-04-23

- fix(sanitizer): eliminate Timer TSan race + CLAP UBSan misalign ([#689](https://github.com/danielraffel/pulp/pull/689))
- ci(coverage): add --break-system-packages for Namespace pip install ([#681](https://github.com/danielraffel/pulp/pull/681))
- perf(doctor): parallelize iOS probes to collapse wall time ([#685](https://github.com/danielraffel/pulp/pull/685))
- test(host): SignalGraph hot-reload stress harness + investigation (#669) ([#674](https://github.com/danielraffel/pulp/pull/674))
- docs: rebaseline coverage tracker after #647 ([#672](https://github.com/danielraffel/pulp/pull/672))
- codex/issue 662 webview first paint ([#673](https://github.com/danielraffel/pulp/pull/673))
- chore: bump Shipyard pin v0.26.0 → v0.29.0 ([#668](https://github.com/danielraffel/pulp/pull/668))

<a id="v0380"></a>
## [0.38.0] - 2026-04-22

- fix(view): drop explicit bridge close from AU editor dealloc paths ([#667](https://github.com/danielraffel/pulp/pull/667))
- coverage: start compliance tranche 1 ([#647](https://github.com/danielraffel/pulp/pull/647))

<a id="v0370"></a>
## [0.37.0] - 2026-04-22

- codex/issue 663 standalone editor only ([#665](https://github.com/danielraffel/pulp/pull/665))
- fix: Codex post-merge sweep — 4 P1s + 4 P2s across PRs 627/629/638/654/650 ([#660](https://github.com/danielraffel/pulp/pull/660))

<a id="v0360"></a>
## [0.36.0] - 2026-04-22

- feat(view): finish plugin-host editor embedding rollout ([#653](https://github.com/danielraffel/pulp/pull/653))
- coverage: union per-OS Cobertura XMLs before diff-cover (#635) ([#654](https://github.com/danielraffel/pulp/pull/654))
- skills: post-merge audit + CLAUDE.md table refresh + path-map fixes ([#652](https://github.com/danielraffel/pulp/pull/652))
- skills: add clap, vst3, auv3 format skills + skill_path_map entries ([#650](https://github.com/danielraffel/pulp/pull/650))

<a id="v0350"></a>
## [0.35.0] - 2026-04-22

- au: wire AU v2 effect MIDI input + aufx→aumf for accepts_midi ([#638](https://github.com/danielraffel/pulp/pull/638))
- coverage: add android kotlin jacoco lane ([#637](https://github.com/danielraffel/pulp/pull/637))
- coverage: add apple swift coverage lane ([#636](https://github.com/danielraffel/pulp/pull/636))
- clap: wire CLAP_EVENT_MIDI + CC/NE/choke/UMP events; emit MIDI on out_events ([#627](https://github.com/danielraffel/pulp/pull/627))
- coverage: expand Python tooling lane ([#634](https://github.com/danielraffel/pulp/pull/634))

<a id="v0340"></a>
## [0.34.0] - 2026-04-22

- feat: persist processor-owned plugin state in adapter blobs ([#628](https://github.com/danielraffel/pulp/pull/628))
- coverage: add Python tools/scripts uploads to Codecov ([#631](https://github.com/danielraffel/pulp/pull/631))
- docs(ci-skill): note v0.24.0/v0.25.0 behavior changes at new pin ([#629](https://github.com/danielraffel/pulp/pull/629))
- chore: bump Shipyard pin v0.22.8 → v0.22.9 (auto-PR title+body quality) ([#623](https://github.com/danielraffel/pulp/pull/623))
- Codex sweep wave 6: installer + coverage follow-ups (#617, #616) ([#622](https://github.com/danielraffel/pulp/pull/622))
- chore: bump Shipyard pin v0.22.7 → v0.22.8 (periodic reconcile loop) ([#621](https://github.com/danielraffel/pulp/pull/621))
- chore: bump Shipyard pin v0.22.5 → v0.22.7 (reconcile evidence-snapshot fix) ([#620](https://github.com/danielraffel/pulp/pull/620))
- chore: bump Shipyard pin v0.22.4 → v0.22.5 (daemon status IPC fix) ([#619](https://github.com/danielraffel/pulp/pull/619))
- Ship fix/coverage-lcov-pipeline-full-surface ([#616](https://github.com/danielraffel/pulp/pull/616))
- chore: bump Shipyard pin v0.22.3 → v0.22.4 (daemon idna codec fix) ([#618](https://github.com/danielraffel/pulp/pull/618))
- chore: installer wrapper for shipyard v0.22.3 + hooks.json schema fix ([#617](https://github.com/danielraffel/pulp/pull/617))
- ci(coverage,docs-sync): Codex sweep #611/#612/#613 P1+P2 follow-ups ([#614](https://github.com/danielraffel/pulp/pull/614))
- ci(docs): generalized docs-sync enforcement (#566 Phase 4 / #567) ([#613](https://github.com/danielraffel/pulp/pull/613))
- Ship fix/codex-sweep-2026-04-21-wave-5 ([#610](https://github.com/danielraffel/pulp/pull/610))
- ci(coverage): per-tier diff-coverage floors (#566 Phase 2) ([#612](https://github.com/danielraffel/pulp/pull/612))
- ci(coverage): flip diff-cover from advisory to required (#566 Phase 3) ([#611](https://github.com/danielraffel/pulp/pull/611))
- docs: replace test counts with coverage references; retire count enforcement ([#609](https://github.com/danielraffel/pulp/pull/609))
- ci(coverage): cross-platform matrix on linux/macos/windows (#566 Phase 1 PR 4) ([#595](https://github.com/danielraffel/pulp/pull/595))
- chore(deps,ci): bump Shipyard v0.21.2 → v0.22.0 + iterate-locally skill guidance ([#608](https://github.com/danielraffel/pulp/pull/608))
- chore(deps): bump pinned Shipyard v0.21.1 → v0.21.2 ([#607](https://github.com/danielraffel/pulp/pull/607))
- fix: Codex post-merge sweep (2026-04-21 wave 4) ([#597](https://github.com/danielraffel/pulp/pull/597))

<a id="v0330"></a>
## [0.33.0] - 2026-04-20

- Ship feature/pulp-pr-shipyard-version-guard ([#606](https://github.com/danielraffel/pulp/pull/606))
- chore(deps): bump pinned Shipyard v0.21.0 → v0.21.1 ([#604](https://github.com/danielraffel/pulp/pull/604))

<a id="v0320"></a>
## [0.32.0] - 2026-04-20

- cli: release-discovery Slice 7 pulp project bump + undo (#564) ([#599](https://github.com/danielraffel/pulp/pull/599))
- fix(ci): drop multi-flag Codecov upload — use components instead ([#603](https://github.com/danielraffel/pulp/pull/603))
- fix(state/ci): state_tree.hpp missing <algorithm> + coverage silent-failure ([#593](https://github.com/danielraffel/pulp/pull/593))
- ci(iwyu): advisory include-what-you-use gate (#594 Phase 2) ([#600](https://github.com/danielraffel/pulp/pull/600))

<a id="v0311"></a>
## [0.31.1] - 2026-04-20

- cli: release-discovery Slice 6 plugin ↔ CLI skew detection (#551) ([#598](https://github.com/danielraffel/pulp/pull/598))
- docs+deps: attribute MkDocs Material deps + extend audit completeness gate ([#592](https://github.com/danielraffel/pulp/pull/592))

<a id="v0310"></a>
## [0.31.0] - 2026-04-20

- cli: release-discovery Slice 5 auto/prompt/manual/off enforcement (#550) ([#590](https://github.com/danielraffel/pulp/pull/590))
- docs: retire legacy renderer + API nav + Doxygen version (#577 PR 4) ([#591](https://github.com/danielraffel/pulp/pull/591))
- chore(shipyard): bump pin v0.20.0 → v0.21.0 ([#589](https://github.com/danielraffel/pulp/pull/589))
- docs: switchover to MkDocs Material (#577 PR 3) ([#588](https://github.com/danielraffel/pulp/pull/588))
- docs: MkDocs hooks + Doxygen + flat URLs (#577 PR 2) ([#587](https://github.com/danielraffel/pulp/pull/587))
- chore(shipyard): bump pin v0.8.0→v0.20.0 + drop `pulp pr --native` workaround ([#585](https://github.com/danielraffel/pulp/pull/585))
- fix: Codex post-merge sweep (2026-04-21 wave 3) ([#581](https://github.com/danielraffel/pulp/pull/581))
- cli: release-discovery Slice 4 /upgrade Claude Code skill (#549) ([#583](https://github.com/danielraffel/pulp/pull/583))
- ci(coverage): diff-cover advisory PR-comment gate (#566 Phase 1 PR 3) ([#580](https://github.com/danielraffel/pulp/pull/580))
- docs: add MkDocs Material parallel build (#577 PR 1) ([#582](https://github.com/danielraffel/pulp/pull/582))
- ci(coverage): wire Codecov dashboard + 20-flag carve-up (#566 Phase 1 PR 2) ([#578](https://github.com/danielraffel/pulp/pull/578))
- docs: fix renderer inline-code-inside-link-text shattering ([#575](https://github.com/danielraffel/pulp/pull/575))

<a id="v0300"></a>
## [0.30.0] - 2026-04-20

- cli: release-discovery Slice 3 migration-doc infrastructure (#548) ([#571](https://github.com/danielraffel/pulp/pull/571))
- ci(coverage): fix cache-value silent failure + filter Catch2 spam + add cobertura output ([#573](https://github.com/danielraffel/pulp/pull/573))

<a id="v0291"></a>
## [0.29.1] - 2026-04-20

- fix: Codex post-merge sweep (2026-04-21 wave 2) ([#574](https://github.com/danielraffel/pulp/pull/574))

<a id="v0290"></a>
## [0.29.0] - 2026-04-20

- cli: ~/.pulp/projects.json registry + pulp projects commands (#552 Slice 1b) ([#563](https://github.com/danielraffel/pulp/pull/563))
- deps: full attribution audit across 4 files (DEPENDENCIES, NOTICE, licensing.md, manifest) ([#565](https://github.com/danielraffel/pulp/pull/565))

<a id="v0280"></a>
## [0.28.0] - 2026-04-21

- cli: release-discovery Slice 2 update-check + pulp config (#547) ([#562](https://github.com/danielraffel/pulp/pull/562))
- fix(cli): cmd_host format-specific error messages (#537) ([#557](https://github.com/danielraffel/pulp/pull/557))

<a id="v0270"></a>
## [0.27.0] - 2026-04-21

- fix: Codex post-merge sweep (2026-04-21) ([#560](https://github.com/danielraffel/pulp/pull/560))
- android skill: bump CLI-verified footer to 2026-04-21 spike ([#558](https://github.com/danielraffel/pulp/pull/558))
- android: end-to-end smoke validation script (#337) ([#556](https://github.com/danielraffel/pulp/pull/556))
- tools/scripts: fix fnmatch `**` glob bug in version + skill gates ([#554](https://github.com/danielraffel/pulp/pull/554))
- cli: pulp doctor --versions foundation (#499 Slice 1) ([#546](https://github.com/danielraffel/pulp/pull/546))

<a id="v0260"></a>
## [0.26.0] - 2026-04-20

- bench: Slice 0.5 honest re-eval on Three.js particles — PerfCounters + threejs-native-demo benchmark mode ([#541](https://github.com/danielraffel/pulp/pull/541))
- host: end-to-end regression coverage (#52) ([#545](https://github.com/danielraffel/pulp/pull/545))
- canvas: migrate sdf_atlas to SkSpan SkFont overloads ([#544](https://github.com/danielraffel/pulp/pull/544))

<a id="v0251"></a>
## [0.25.1] - 2026-04-20

- examples(threejs): flush rAF in headless loader loop (fix #542) ([#553](https://github.com/danielraffel/pulp/pull/553))

<a id="v0250"></a>
## [0.25.0] - 2026-04-20

- fix: Codex sweep rollup - sysex, view header, scan AU/AUv3, post-merge limit (#500 #529) ([#540](https://github.com/danielraffel/pulp/pull/540))

<a id="v0240"></a>
## [0.24.0] - 2026-04-20

- view: PluginManagerPanel widget over scanner backend (#494) ([#538](https://github.com/danielraffel/pulp/pull/538))
- platform: cross-platform pulp::platform::Permissions backends (#495) ([#539](https://github.com/danielraffel/pulp/pull/539))
- render: GPU compute staging buffer pool (zero-copy Slice 1) ([#536](https://github.com/danielraffel/pulp/pull/536))
- host/view: P2 rollup — VST3 FUID + LV2 URI identity, placeholder-node pass-through, createComputeBuffer base64 upload (#491) ([#535](https://github.com/danielraffel/pulp/pull/535))
- install-shipyard: defensively reinitialize truncated queue.json (#528) ([#534](https://github.com/danielraffel/pulp/pull/534))
- ci(android): re-gate emulator smoke — HVF unsupported on macos-latest ([#533](https://github.com/danielraffel/pulp/pull/533))
- docs: close #523 remaining gaps — accuracy audit follow-ups ([#531](https://github.com/danielraffel/pulp/pull/531))
- ci(android): re-enable emulator smoke test with correct ABI + intent ([#530](https://github.com/danielraffel/pulp/pull/530))
- ci: release watchdog — three layers against silent release failures ([#512](https://github.com/danielraffel/pulp/pull/512))
- docs: remove stale Intel Mac + AUv3-not-implemented + phase1-snapshot drift ([#521](https://github.com/danielraffel/pulp/pull/521))
- bench: Slice 0 zero-copy perf counters + ui-preview benchmark mode ([#526](https://github.com/danielraffel/pulp/pull/526))
- view(win): UIA shutdown — atomic null-first before disconnect (#514) ([#520](https://github.com/danielraffel/pulp/pull/520))
- docs: document pulp pr + reconcile CLAUDE.md ship guidance (#523) ([#525](https://github.com/danielraffel/pulp/pull/525))
- bench: treat missing memory_bandwidth_fraction as unknown, not zero ([#524](https://github.com/danielraffel/pulp/pull/524))
- bench: tools/scripts/bench_diff.py — zero-copy bench diff harness (#517) ([#518](https://github.com/danielraffel/pulp/pull/518))
- fix(ci): auto-release — trailer-aware bump detection (#513) ([#519](https://github.com/danielraffel/pulp/pull/519))
- fix(ci+android): Codex post-merge sweep — stale vars, sweep scope, focus flag (#500) ([#515](https://github.com/danielraffel/pulp/pull/515))
- fix(ci): auto-release.yml YAML indent — every run since #501 failed silently ([#510](https://github.com/danielraffel/pulp/pull/510))
- lv2: serialize midi_out back to atom output port (#491) ([#511](https://github.com/danielraffel/pulp/pull/511))

<a id="v0231"></a>
## [0.23.1] - 2026-04-20

- chore: bump SDK to v0.23.1 ([#509](https://github.com/danielraffel/pulp/pull/509))
- audio(android): oboe — clean up input stream on output-start failure (#500) ([#507](https://github.com/danielraffel/pulp/pull/507))
- view(win): UIA shutdown UAF — disconnect provider before delete (#500) ([#508](https://github.com/danielraffel/pulp/pull/508))
- android: audio focus follow-ups — transient routing + stop-race (#500) ([#505](https://github.com/danielraffel/pulp/pull/505))
- fix(osc): Receiver::stop() — shutdown before close to avoid FD-reuse UB (#490) ([#503](https://github.com/danielraffel/pulp/pull/503))
- ci: post-merge review sweep workflow — catch delayed Codex comments ([#502](https://github.com/danielraffel/pulp/pull/502))
- ci(auto-release): fix regressions from #498 (skip-trailer + revert) (#500 P1) ([#501](https://github.com/danielraffel/pulp/pull/501))
- ci(auto-release): diff against latest tag, not event.before (self-heal) ([#498](https://github.com/danielraffel/pulp/pull/498))
- docs(policies): add non-goals.md documenting deliberate scope boundaries ([#497](https://github.com/danielraffel/pulp/pull/497))
- view(win): UIA Phase 2 — host provider + event hooks (#247) ([#485](https://github.com/danielraffel/pulp/pull/485))
- midi: SysExAccumulator — shared F0/F7 aggregation state machine (#86) ([#484](https://github.com/danielraffel/pulp/pull/484))
- android: PulpAudioController + intent-driven FGS lifecycle (#333) ([#483](https://github.com/danielraffel/pulp/pull/483))

<a id="v0230"></a>
## [0.23.0] - 2026-04-20

- audio: cross-platform AudioFocusRegistry + Android JNI wiring (#334) ([#482](https://github.com/danielraffel/pulp/pull/482))
- ci: skip close-path on already-closed trackers (#475 Codex P2 × 2) ([#481](https://github.com/danielraffel/pulp/pull/481))
- audio(android): fix input-buffer sizing + race on stream start (#478 Codex P1/P2) ([#480](https://github.com/danielraffel/pulp/pull/480))
- test(runtime): fix SpscQueue hammer shutdown race (6× flake today) ([#479](https://github.com/danielraffel/pulp/pull/479))

<a id="v0220"></a>
## [0.22.0] - 2026-04-20

- audio(android): wire Oboe input-stream frame read (#244) ([#478](https://github.com/danielraffel/pulp/pull/478))
- fix(docs): guard PagefindUI init against missing bundle (local preview) ([#475](https://github.com/danielraffel/pulp/pull/475))
- ci: post-merge fixes to orphan/deps/ruleset drift workflows (P1 + 3×P2) ([#477](https://github.com/danielraffel/pulp/pull/477))
- test(env): static_assert macOS memory-pressure mask includes NORMAL (#466 gap) ([#476](https://github.com/danielraffel/pulp/pull/476))
- ci: skip RTSan selector resolution outside workflow_dispatch (#438 P2 / #424) ([#464](https://github.com/danielraffel/pulp/pull/464))
- ci: ruleset-as-code for main branch protection (#462) ([#471](https://github.com/danielraffel/pulp/pull/471))
- docs: defer pulp report bug guidance until CLI ships (#438 P2 / #435) ([#458](https://github.com/danielraffel/pulp/pull/458))
- fix(env): include NORMAL in macOS memory-pressure source mask (#438 P2 / #404) ([#466](https://github.com/danielraffel/pulp/pull/466))
- test(cli): bump pulp doctor android|ios timeout to 90s (flake fix) ([#474](https://github.com/danielraffel/pulp/pull/474))
- fix(midi): Win MIDI close() guards SysEx re-queue against shutdown race (#438 P1 / #388) ([#457](https://github.com/danielraffel/pulp/pull/457))
- ci: weekly orphan-branch sweep workflow (#463) ([#469](https://github.com/danielraffel/pulp/pull/469))
- ci: weekly dependency drift-check workflow (#467) ([#470](https://github.com/danielraffel/pulp/pull/470))
- fix(docs): Pagefind results drawer as anchored overlay (#472) ([#473](https://github.com/danielraffel/pulp/pull/473))
- docs: keep ALSA limitation scoped to ALSA behavior (#438 P2 / #437) ([#465](https://github.com/danielraffel/pulp/pull/465))
- docs: restore Pagefind search in docs site (recover feature/doc-search) ([#461](https://github.com/danielraffel/pulp/pull/461))
- fix(doctor): quote adb + emulator fallback paths (#438 P2 / #442) ([#460](https://github.com/danielraffel/pulp/pull/460))
- fix(android): convert insets to CSS units + distinguish landscape sides (#438 P2 / #443) ([#459](https://github.com/danielraffel/pulp/pull/459))
- fix(audio): ALSA create_device distinguishes open-fail from 0-channels (#438 P1 / #387) ([#456](https://github.com/danielraffel/pulp/pull/456))
- fix(audio): WASAPI capture uses engine stride, not actual_channels (#438 P1 / #386) ([#455](https://github.com/danielraffel/pulp/pull/455))
- fix(aax): copy sysex in bypass MIDI-thru path (#438 P2 / #408) ([#454](https://github.com/danielraffel/pulp/pull/454))
- fix(audio): ALSA capture drops before join to unblock snd_pcm_readi (#438 P1 / #387) ([#453](https://github.com/danielraffel/pulp/pull/453))
- fix(platform): use TARGET_OS_IPHONE for iOS observer dispatch (#438 P1 / #445) ([#451](https://github.com/danielraffel/pulp/pull/451))
- fix(events): Timer::start() is idempotent when already active (#438 P1 / #428) ([#450](https://github.com/danielraffel/pulp/pull/450))
- fix(release): search real FetchContent roots + fail on missing wgpu (#438 P1 / #397) ([#452](https://github.com/danielraffel/pulp/pull/452))
- fix(platform): Linux env observer stops + joins poll thread on teardown (#438 P1 / #444) ([#449](https://github.com/danielraffel/pulp/pull/449))
- ci: preserve fail-fast semantics in retry composite action (#438 P1 / #432) ([#448](https://github.com/danielraffel/pulp/pull/448))
- fix(platform): prevent Environment listener callback after token reset (#438 P1 / #403) ([#447](https://github.com/danielraffel/pulp/pull/447))
- platform(win): WM_ACTIVATEAPP + registry-watch observers for Environment API (#342) ([#446](https://github.com/danielraffel/pulp/pull/446))
- test(view): pin ComboBox overlay-click contract (regression) ([#402](https://github.com/danielraffel/pulp/pull/402))
- platform(ios): scene + UITraitCollection observers for Environment API (#342) ([#445](https://github.com/danielraffel/pulp/pull/445))
- fix(midi): ALSA sysex accumulator recovers from aborted F0 (#438 P2 / #406) ([#440](https://github.com/danielraffel/pulp/pull/440))
- platform(android): Kotlin plumbing for orientation + safe-area + keyboard env (#342) ([#443](https://github.com/danielraffel/pulp/pull/443))
- platform(linux): X11/gsettings/proc env adapter for Environment API (#342) ([#444](https://github.com/danielraffel/pulp/pull/444))
- fix(doctor): optional checks don't drive exit code + Windows .exe suffixes (#438 P1+P2 / #389) ([#442](https://github.com/danielraffel/pulp/pull/442))
- ci: lift runner resolver, route sanitizers via repo vars (#412 step 6) ([#424](https://github.com/danielraffel/pulp/pull/424))

<a id="v0210"></a>
## [0.21.0] - 2026-04-19

- platform: unified Environment API for display/safe-area/keyboard/scheme/lifecycle/memory (#342) ([#403](https://github.com/danielraffel/pulp/pull/403))
- chore: migrate CHANGELOG regeneration from in-tree script to shipyard ([#417](https://github.com/danielraffel/pulp/pull/417))
- ci: widen windows alias to also gate on MSVC release-path (#438 P1, #416) ([#439](https://github.com/danielraffel/pulp/pull/439))
- ci: TSan hardening steps 4 + 5 — race hammer + path-gated trigger (#412) ([#431](https://github.com/danielraffel/pulp/pull/431))
- fix(setup): retry_git_clone scrubs partial target between attempts (#438 P1 / #425) ([#441](https://github.com/danielraffel/pulp/pull/441))
- docs(aax skill): document the sysex accumulator pattern #408 landed ([#436](https://github.com/danielraffel/pulp/pull/436))
- fix(release): bundle libwgpu_native + rewrite rpath → portable CLI tarball (#391) ([#397](https://github.com/danielraffel/pulp/pull/397))
- ci: portable-binary smoke gate on every release-cli.yml run (#391) ([#395](https://github.com/danielraffel/pulp/pull/395))
- midi(mac): CoreMIDI SysEx7 reassembly (#239) ([#405](https://github.com/danielraffel/pulp/pull/405))
- render: CMake gate for iOS Dawn-only path (#359) ([#433](https://github.com/danielraffel/pulp/pull/433))
- docs(ci): self-hosted runner setup + first-run gotchas (#429) ([#430](https://github.com/danielraffel/pulp/pull/430))
- docs(support-matrix): reflect shipped Win32 + ALSA sysex ([#437](https://github.com/danielraffel/pulp/pull/437))
- ci: reusable retry composite action + wrap apt-llvm-org flake sites (#419) ([#432](https://github.com/danielraffel/pulp/pull/432))
- fix(events): Timer sentinel race — move alive_ reassignment from stop() to start() (#414) ([#428](https://github.com/danielraffel/pulp/pull/428))
- ci: ccache + FetchContent source caching (#420) ([#427](https://github.com/danielraffel/pulp/pull/427))
- ci: concurrency cancel-in-progress on PR workflows (#421) ([#423](https://github.com/danielraffel/pulp/pull/423))
- docs: GitHub issue templates (bug report + feature request) (#413) ([#435](https://github.com/danielraffel/pulp/pull/435))
- cli(doctor): add android + ios mobile dev-env subcommands (#8 / #355) ([#389](https://github.com/danielraffel/pulp/pull/389))
- fix(view): rename app_framework Toolbar → NativeToolbar to remove ODR violation (#411) ([#426](https://github.com/danielraffel/pulp/pull/426))
- midi(linux): ALSA raw-midi sysex accumulator (#239) ([#406](https://github.com/danielraffel/pulp/pull/406))
- format(aax): sysex parity with CLAP/VST3/AU/CoreMIDI/Win/ALSA (#239) ([#408](https://github.com/danielraffel/pulp/pull/408))
- ci: retry transient git ops + capture Android diagnostic on failure (#418) ([#425](https://github.com/danielraffel/pulp/pull/425))
- view: Linux ATK pkg-config integration (#18 phase 1) ([#400](https://github.com/danielraffel/pulp/pull/400))
- ci: stable-name status aliases macos/linux/windows for branch protection ([#416](https://github.com/danielraffel/pulp/pull/416))
- ci(sanitizers): skip private planning submodule (unblock all PRs) ([#401](https://github.com/danielraffel/pulp/pull/401))

<a id="v0200"></a>
## [0.20.0] - 2026-04-18

- midi(win): MIM_LONGDATA SysEx + QPC timestamps (#19 / #245 partial) ([#388](https://github.com/danielraffel/pulp/pull/388))
- audio(linux): ALSA capture endpoint + real device-channel metadata (#20 / #215) ([#387](https://github.com/danielraffel/pulp/pull/387))
- audio(win): WASAPI capture endpoint + flow-aware device routing (#243) ([#386](https://github.com/danielraffel/pulp/pull/386))
- platform(win): move COM base-type includes into win32_sane.hpp (#384) ([#385](https://github.com/danielraffel/pulp/pull/385))
- ci: add Windows MSVC release-path job to the PR gate ([#349](https://github.com/danielraffel/pulp/pull/349))

<a id="v0190"></a>
## [0.19.0] - 2026-04-18

- view(win): fix UIAutomationCore.h C2371/C2146 in MSVC + unblock releases ([#383](https://github.com/danielraffel/pulp/pull/383))
- test + cli: address Codex P2s from PRs #378/#379/#381 ([#382](https://github.com/danielraffel/pulp/pull/382))
- cli(validate): --strict flag + missing-validator advisory (#356) ([#381](https://github.com/danielraffel/pulp/pull/381))
- test(audio): framework-layer edge-case corpus (#356) ([#380](https://github.com/danielraffel/pulp/pull/380))
- test(examples): golden breadth for Compressor/Drums/Synth (#356) ([#379](https://github.com/danielraffel/pulp/pull/379))
- cli(upgrade): fix 404 — drop version from tarball filename (#352) ([#377](https://github.com/danielraffel/pulp/pull/377))
- test(audio): SR × block-size determinism matrix (#356) ([#378](https://github.com/danielraffel/pulp/pull/378))
- cli: delegate 'pulp pr' to 'shipyard pr' by default; --native preserves fallback (#352) ([#376](https://github.com/danielraffel/pulp/pull/376))
- docs: refresh support matrix after v0.17-v0.18 provider contracts (#348) ([#375](https://github.com/danielraffel/pulp/pull/375))
- ios: forward AVAudioSession notifications to native listener (#329) ([#374](https://github.com/danielraffel/pulp/pull/374))
- test(view): harden WidgetBridge clear() + snapshot/restore (+4 cases) ([#373](https://github.com/danielraffel/pulp/pull/373))
- test(view): harden TextEditor input edges (+7 cases) ([#372](https://github.com/danielraffel/pulp/pull/372))
- test(cli-ship): shell-out tests for pulp ship (+7 cases) ([#368](https://github.com/danielraffel/pulp/pull/368))
- test(view): harden screenshot edge cases (+4 cases) ([#371](https://github.com/danielraffel/pulp/pull/371))
- test: delete placeholder test_showcase + test_token_diff (#357) ([#370](https://github.com/danielraffel/pulp/pull/370))
- test(tempo-hook): harden Processor::on_host_tempo_changed (+6 cases) ([#369](https://github.com/danielraffel/pulp/pull/369))
- test(cli): shell-out behaviour tests for the pulp binary (+7 cases) ([#367](https://github.com/danielraffel/pulp/pull/367))
- test(events): harden AsyncUpdater coalescing + EventLoop/Timer edges (+6 cases) ([#366](https://github.com/danielraffel/pulp/pull/366))
- test(platform): I/O failure-path sweep (+10 cases) ([#365](https://github.com/danielraffel/pulp/pull/365))
- test(format): harden transport/memory/ARA hook stubs (+11 cases) ([#363](https://github.com/danielraffel/pulp/pull/363))
- test(runtime): harden network_stream + websocket_channel (+12 cases) ([#362](https://github.com/danielraffel/pulp/pull/362))
- test(host): expand plugin_info_metadata + scan_cache coverage — fix dropped richer-metadata JSON round-trip ([#361](https://github.com/danielraffel/pulp/pull/361))
- test(dsl): first contract tests for FaustProcessor (+18 cases) — fix 0-in instrument leaking default stereo bus ([#360](https://github.com/danielraffel/pulp/pull/360))
- view(ios): wire Skia paint + CADisplayLink for GPU window/plugin hosts ([#358](https://github.com/danielraffel/pulp/pull/358))
- test: harden OSC coverage (+34 cases) — first pass on #351 ([#353](https://github.com/danielraffel/pulp/pull/353))

<a id="v0180"></a>
## [0.18.0] - 2026-04-17

- platform: introduce pulp::platform::Permissions API + desktop backend (#331) ([#354](https://github.com/danielraffel/pulp/pull/354))
- ios: add pulp_add_ios_auv3() CMake helper (#328) ([#350](https://github.com/danielraffel/pulp/pull/350))

<a id="v0170"></a>
## [0.17.0] - 2026-04-17

- audio: fix MSVC C2248 on fire_device_change + add release-guard.yml ([#318](https://github.com/danielraffel/pulp/pull/318))
- platform(linux): preserve clipboard bytes + correct has_text semantics (#309 P2) ([#320](https://github.com/danielraffel/pulp/pull/320))
- view: compile bridge stubs on iOS + invoke providers outside mutex (#313 P1 + P2) ([#323](https://github.com/danielraffel/pulp/pull/323))
- ci(tooling): coverage-fail propagation + C-compiler check + run_ubsan.sh (#317 P2s) ([#322](https://github.com/danielraffel/pulp/pull/322))
- platform: narrow FileDialog::has_backend=true to macOS only (#316 P2) ([#321](https://github.com/danielraffel/pulp/pull/321))
- events: install_backend assigns backend before emitting lost callbacks (#314 P2) ([#319](https://github.com/danielraffel/pulp/pull/319))
- platform: FileDialog::has_backend returns true on Apple (#312 Codex P2) ([#316](https://github.com/danielraffel/pulp/pull/316))
- ci: LLVM coverage instrumentation + enable sanitizers.yml (#290 tooling) ([#317](https://github.com/danielraffel/pulp/pull/317))
- platform: real Linux clipboard + Android bridge registration (#300 P1) ([#309](https://github.com/danielraffel/pulp/pull/309))
- host(vst3): RT-safe pending-edit drain via two-buffer swap (#307 Codex P1) ([#315](https://github.com/danielraffel/pulp/pull/315))
- events: NSD refresh on re-announce + clear on backend swap (#310 Codex P2) ([#314](https://github.com/danielraffel/pulp/pull/314))
- view: host-backend bridges for screenshot/WindowHost/PluginViewHost (#299) ([#313](https://github.com/danielraffel/pulp/pull/313))

<a id="v0160"></a>
## [0.16.0] - 2026-04-17

- platform: native file dialog backend-registration + explicit unsupported (#301) ([#312](https://github.com/danielraffel/pulp/pull/312))
- format: guard compare_screenshots against filesystem exceptions (#308 Codex P1) ([#311](https://github.com/danielraffel/pulp/pull/311))
- events: NetworkServiceDiscovery backend-registration + honest no-op (#302) ([#310](https://github.com/danielraffel/pulp/pull/310))
- host: consume VST3 set_parameter edits via IParameterChanges (#297) ([#307](https://github.com/danielraffel/pulp/pull/307))
- format: ValidationHarness screenshot/inspector via provider callbacks (#298) ([#308](https://github.com/danielraffel/pulp/pull/308))

<a id="v0150"></a>
## [0.15.0] - 2026-04-16

- host: skip blacklisted bundles before opening them (#271 P1 follow-up) ([#287](https://github.com/danielraffel/pulp/pull/287))
- host: wire CLAP PluginSlot::set_parameter end-to-end (#296) ([#306](https://github.com/danielraffel/pulp/pull/306))
- docs: codify 'tests ship with fixes' as non-negotiable rule ([#305](https://github.com/danielraffel/pulp/pull/305))
- au: sysex via AURenderEventMIDIEventList (#288, #239 AU half) ([#292](https://github.com/danielraffel/pulp/pull/292))
- ci(release): auto-regenerate CHANGELOG.md after SDK tag push ([#294](https://github.com/danielraffel/pulp/pull/294))
- ship: hard-fail on empty Ed25519 signatures (#295 P0) ([#303](https://github.com/danielraffel/pulp/pull/303))
- ci(docs): run Docs Consistency on every PR (#273, #289) ([#291](https://github.com/danielraffel/pulp/pull/291))
- ios: SwiftUI host template for AUv3 verification (#250) ([#279](https://github.com/danielraffel/pulp/pull/279))
- fix: add missing skill descriptions ([#286](https://github.com/danielraffel/pulp/pull/286))
- ara: expand ARADocumentControllerInterface to 27 callbacks (#253) ([#278](https://github.com/danielraffel/pulp/pull/278))
- wasapi: IMMNotificationClient hotplug → fire_device_change (#243) ([#281](https://github.com/danielraffel/pulp/pull/281))
- docs: regenerate CHANGELOG.md to include v0.14.0 ([#293](https://github.com/danielraffel/pulp/pull/293))
- changelog: reverse-chron generator + per-release bodies + tag fetch (#262) ([#265](https://github.com/danielraffel/pulp/pull/265))
- audit triad: #275 check-docs grep pipe, #276 LV2 MidiEffect class, #277 CLAP sidechain guard ([#285](https://github.com/danielraffel/pulp/pull/285))
- vst3: route DataEvent kMidiSysEx into MidiBuffer sidecar (#239 VST3 half) ([#274](https://github.com/danielraffel/pulp/pull/274))
- a11y(win): UIA phase 1 — link UIAutomationCore + session bootstrap (#247) ([#283](https://github.com/danielraffel/pulp/pull/283))
- oboe: symmetric input-stream opener (#244) ([#284](https://github.com/danielraffel/pulp/pull/284))
- lv2: atom-port MIDI parsing in run() (#241) ([#270](https://github.com/danielraffel/pulp/pull/270))
- host: ScanOptions::blacklist — parent-side scan-worker crash feedback (#246) ([#271](https://github.com/danielraffel/pulp/pull/271))
- midi(win): WinRT MIDI 2.0 backend skeleton behind PULP_HAS_WINRT_MIDI (#245) ([#282](https://github.com/danielraffel/pulp/pull/282))
- a11y(linux): register AT-SPI bridge when atk-bridge-2.0 present (#248) ([#280](https://github.com/danielraffel/pulp/pull/280))
- clap: wire CLAP_EVENT_MIDI_SYSEX into MidiBuffer sidecar (#239) ([#269](https://github.com/danielraffel/pulp/pull/269))
- au: AUv3 MIDI-out via MIDIOutputEventBlock + conflict-marker cleanup (#242) ([#268](https://github.com/danielraffel/pulp/pull/268))
- vst3: dynamic bus arrangements — honor setBusArrangements (#240) ([#267](https://github.com/danielraffel/pulp/pull/267))
- canvas: Canvas::draw_image(native_handle) + ImageView wire (#255) ([#266](https://github.com/danielraffel/pulp/pull/266))

<a id="v0140"></a>
## [0.14.0] - 2026-04-16

- view: ModulationMatrixWidget depth/curve/remove editing (#256) ([#264](https://github.com/danielraffel/pulp/pull/264))
- ara(au): audioUnitARAFactory KVO property on PulpAudioUnit (#252) ([#263](https://github.com/danielraffel/pulp/pull/263))
- ara(vst3): detect IHostApplication + surface ARA factory (#251) ([#261](https://github.com/danielraffel/pulp/pull/261))

<a id="v0131"></a>
## [0.13.1] - 2026-04-16

- fix(cli): Windows popen/pclose — unblock release-cli.yml builds ([#259](https://github.com/danielraffel/pulp/pull/259))
- docs: Pulp version clarity + plugin surface (D3/D4/D5) ([#257](https://github.com/danielraffel/pulp/pull/257))
- view: ModulationMatrixWidget canvas widget (W07 slice B5) ([#238](https://github.com/danielraffel/pulp/pull/238))
- view: ImageView consumes ImageCache (W07 slice B4) ([#237](https://github.com/danielraffel/pulp/pull/237))
- ara: CLAP live-factory end-to-end test (W06 slice A2a) ([#234](https://github.com/danielraffel/pulp/pull/234))
- docs: host-compatibility matrix (W06 slice A4) ([#236](https://github.com/danielraffel/pulp/pull/236))
- examples: ara-pitch-tracker — minimum viable ARA-aware CLAP plug-in (W06 A3) ([#235](https://github.com/danielraffel/pulp/pull/235))
- a11y: cross-platform accessibility_provider entry point (W04 4.1+4.2) ([#230](https://github.com/danielraffel/pulp/pull/230))

<a id="v0130"></a>
## [0.13.0] - 2026-04-15

- ara: real ARA::ARAFactory construction (W06 A1 — beyond the stub) ([#232](https://github.com/danielraffel/pulp/pull/232))
- Ship chore/bump-shipyard-0.3.0 ([#233](https://github.com/danielraffel/pulp/pull/233))
- host: pulp-scan-worker out-of-process scan binary (W03 slice 3.3b) ([#229](https://github.com/danielraffel/pulp/pull/229))
- midi: MidiBuffer::add_sysex() variable-length sidecar (W01 MIDI vocab) ([#231](https://github.com/danielraffel/pulp/pull/231))
- ara: VST3 + AU companion-factory extension keys (W06 6.3/6.4) ([#228](https://github.com/danielraffel/pulp/pull/228))
- examples: ios-auv3-synth scaffold (workstream 05 slice 5.1) ([#227](https://github.com/danielraffel/pulp/pull/227))

<a id="v0120"></a>
## [0.12.0] - 2026-04-15

- clap: ARA companion-factory extension hook (workstream 06 slice 6.5) ([#226](https://github.com/danielraffel/pulp/pull/226))
- docs: iOS AUv3 & mobile guidance (workstream 05 slice 5.6) ([#225](https://github.com/danielraffel/pulp/pull/225))

<a id="v0110"></a>
## [0.11.0] - 2026-04-15

- iOS review fixes: clipboard + child_process + on_mouse_cancel ([#224](https://github.com/danielraffel/pulp/pull/224))
- Add iOS skill (.agents/skills/ios/) ([#223](https://github.com/danielraffel/pulp/pull/223))
- iOS Simulator build: gate desktop-only code, fix stale view API ([#222](https://github.com/danielraffel/pulp/pull/222))
- matrix(prod-readiness 08): seed ci_test on 5 usable entries (8.4 sweep) ([#169](https://github.com/danielraffel/pulp/pull/169))
- host: persistent plugin scan cache (workstream 03 slice 3.1) ([#182](https://github.com/danielraffel/pulp/pull/182))
- docs(06): ARA integration guide — current state + SDK acquisition + roadmap ([#181](https://github.com/danielraffel/pulp/pull/181))
- format: Processor::on_host_tempo_changed() hook (slice 1.10) ([#212](https://github.com/danielraffel/pulp/pull/212))
- ara: Processor::create_ara_document_controller() hook (slice 6.3a) ([#190](https://github.com/danielraffel/pulp/pull/190))
- Workstream 07 slice 7.5: resizable plugin shell (size + aspect lock) ([#206](https://github.com/danielraffel/pulp/pull/206))
- host: typed HostedEditor API on PluginSlot (slice 3.4) ([#201](https://github.com/danielraffel/pulp/pull/201))
- Workstream 07 slice 7.3: TableModel data + sort layer ([#200](https://github.com/danielraffel/pulp/pull/200))
- Workstream 01 slice 1.9: validate_descriptor() helper ([#211](https://github.com/danielraffel/pulp/pull/211))
- Workstream 04 slice 4.2a: ATK / AT-SPI mapping table ([#204](https://github.com/danielraffel/pulp/pull/204))
- Workstream 07 slice 7.1: ModulationMatrix data model ([#196](https://github.com/danielraffel/pulp/pull/196))
- a11y: cross-platform announce_accessibility() API (slice 4.3) ([#187](https://github.com/danielraffel/pulp/pull/187))
- docs(8.5): capabilities.md Known Limitations section (renderer half) ([#175](https://github.com/danielraffel/pulp/pull/175))
- matrix(8.5): format_limitations section — surface known per-format gaps ([#171](https://github.com/danielraffel/pulp/pull/171))
- Workstream 01 slice 1.8: PluginDescriptor::vendor_url + vendor_email ([#210](https://github.com/danielraffel/pulp/pull/210))
- format: ViewSize::aspect_ratio field (workstream 07 slice 7.5b) ([#209](https://github.com/danielraffel/pulp/pull/209))
- Workstream 02 slice 2.5: MIDI 1.0 running-status parser ([#202](https://github.com/danielraffel/pulp/pull/202))
- Workstream 03 slice 3.7: richer PluginInfo metadata + CLAP extraction ([#198](https://github.com/danielraffel/pulp/pull/198))
- format: Processor::on_memory_pressure() optional hook (slice 5.3) ([#195](https://github.com/danielraffel/pulp/pull/195))
- Workstream 04 slice 4.4: cross-platform accessibility tree snapshot ([#192](https://github.com/danielraffel/pulp/pull/192))
- Workstream 06 slice 6.2: ARA core type layer ([#185](https://github.com/danielraffel/pulp/pull/185))
- Workstream 03 slice 3.2: background plugin scanner ([#194](https://github.com/danielraffel/pulp/pull/194))
- Workstream 03 slice 3.6: AU slot — route MIDI via MusicDeviceMIDIEvent ([#191](https://github.com/danielraffel/pulp/pull/191))
- Workstream 01 slice 1.2: VST3 multi-bus + sidechain routing ([#178](https://github.com/danielraffel/pulp/pull/178))
- midi_ci: echo inquirer MUID in profile reply destination field ([#180](https://github.com/danielraffel/pulp/pull/180))
- matrix(8.4): seed ci_test: on 5 more usable entries (sweep batch 2) ([#172](https://github.com/danielraffel/pulp/pull/172))
- lv2: resolve LV2_URID__map in instantiate() (workstream 01 slice 1.5) ([#174](https://github.com/danielraffel/pulp/pull/174))
- a11y: Windows UIA mapping table (workstream 04 slice 4.1a) ([#203](https://github.com/danielraffel/pulp/pull/203))
- Workstream 01 slice 1.3: AU v2 outbound parameter changes ([#176](https://github.com/danielraffel/pulp/pull/176))
- audio: AudioSystem hotplug callback base plumbing (slice 2.7) ([#199](https://github.com/danielraffel/pulp/pull/199))
- format: ios_requires_background_audio descriptor flag (slice 5.5) ([#208](https://github.com/danielraffel/pulp/pull/208))
- midi: MidiBuffer UMP sidecar accessor (workstream 02 slice 2.6a) ([#193](https://github.com/danielraffel/pulp/pull/193))
- format: Processor::on_host_transport_changed() hook (slice 1.11) ([#213](https://github.com/danielraffel/pulp/pull/213))
- host: scan blacklist store (workstream 03 slice 3.3a) ([#205](https://github.com/danielraffel/pulp/pull/205))
- auv3: sidechain input bus + render-block routing (workstream 01 slice 1.4b) ([#183](https://github.com/danielraffel/pulp/pull/183))
- coremidi: iterate UMP by message size; decode type-0x04 (slice 1.6) ([#184](https://github.com/danielraffel/pulp/pull/184))
- audio: Linux factory now selects JACK when server is running (slice 2.2) ([#188](https://github.com/danielraffel/pulp/pull/188))
- host: VST3 slot — route MIDI and parameter automation (slice 3.5) ([#186](https://github.com/danielraffel/pulp/pull/186))
- view: A/B compare two-slot state snapshot (slice 7.2) ([#189](https://github.com/danielraffel/pulp/pull/189))
- view: ImageCache with LRU + memory-pressure hook (slice 7.4) ([#207](https://github.com/danielraffel/pulp/pull/207))
- format(clap 1.1): route second input bus to Processor::set_sidechain() ([#177](https://github.com/danielraffel/pulp/pull/177))
- matrix(8.1): widget rows — 16 structured entries for existing widgets ([#173](https://github.com/danielraffel/pulp/pull/173))
- matrix(8.1): schema_version: 2 + ARA plugin_extensions entry ([#170](https://github.com/danielraffel/pulp/pull/170))
- matrix: known-limitations schema + renderer (workstream 08 slice 8.5) ([#168](https://github.com/danielraffel/pulp/pull/168))

<a id="v0101"></a>
## [0.10.1] - 2026-04-14

- fix(host): dlfcn.h Windows shim — unblock release-cli builds (row 17) ([#163](https://github.com/danielraffel/pulp/pull/163))
- docs(prod-readiness 08): consistency checker — locks in 8.7 a11y fix ([#166](https://github.com/danielraffel/pulp/pull/166))
- docs: reconcile capability/a11y drift; bump planning for prod-readiness plan ([#164](https://github.com/danielraffel/pulp/pull/164))
- test: GraphSerializer round-trip coverage (row 11) ([#162](https://github.com/danielraffel/pulp/pull/162))
- SignalGraph Phase 3: GraphSerializer (.pulpgraph format) ([#161](https://github.com/danielraffel/pulp/pull/161))

<a id="v0100"></a>
## [0.10.0] - 2026-04-14

- SignalGraph Phase 2: GraphEditorView (canvas-based node editor) ([#160](https://github.com/danielraffel/pulp/pull/160))
- Fix Codex P1 review comments on signal-graph PRs (#143/#153/#158) ([#159](https://github.com/danielraffel/pulp/pull/159))

<a id="v090"></a>
## [0.9.0] - 2026-04-14

- SignalGraph Phase 1: per-format depth (CLAP/VST3/AU/LV2) + connect_automation ([#158](https://github.com/danielraffel/pulp/pull/158))
- Replace indent-fragile version regex with real JSON parser; fix drift ([#157](https://github.com/danielraffel/pulp/pull/157))
- Fix Android: force-keep TalkBack JNI symbols without whole-archiving pulp-view ([#151](https://github.com/danielraffel/pulp/pull/151))
- Fix macOS build break: pin ausdk target to CXX_STANDARD=23 (closes #155) ([#156](https://github.com/danielraffel/pulp/pull/156))
- version: fix marketplace.json plugins[0].version drift + guard against re-drift ([#152](https://github.com/danielraffel/pulp/pull/152))
- SignalGraph Phase 0: host contracts (snapshot publish + event queue + param flags + thread rules) ([#153](https://github.com/danielraffel/pulp/pull/153))

<a id="v080"></a>
## [0.8.0] - 2026-04-14

- ViewBridge Phase 4 MVP: Remote View Protocol + attach_remote_channel ([#154](https://github.com/danielraffel/pulp/pull/154))
- chore(planning): bump submodule to pick up Feature 1 Phase 2/3 checkoffs ([#150](https://github.com/danielraffel/pulp/pull/150))

<a id="v070"></a>
## [0.7.0] - 2026-04-14

- ViewBridge Phase 3: standalone, AU v2 (+ dual-Processor fix), AU v3, cross-format tests ([#146](https://github.com/danielraffel/pulp/pull/146))
- doctor + pulp pr: surface missing RELEASE_BOT_TOKEN; doc one-time setup ([#149](https://github.com/danielraffel/pulp/pull/149))

<a id="v060"></a>
## [0.6.0] - 2026-04-13

- Streams Phase 4: MessageChannel + WebSocket + OSC + JSON-RPC (closes Feature 3) ([#145](https://github.com/danielraffel/pulp/pull/145))
- Fix Codex P1 + P2 review on PR #144 (versioning gates) ([#147](https://github.com/danielraffel/pulp/pull/147))

<a id="v050"></a>
## [0.5.0] - 2026-04-13

- SignalGraph: full DAW hosting engine (Feature 5 Phases 1-3) ([#143](https://github.com/danielraffel/pulp/pull/143))
- skills(mpe): add MPE developer skill ([#142](https://github.com/danielraffel/pulp/pull/142))

<a id="v040"></a>
## [0.4.0] - 2026-04-13

- Versioning & skill-sync gates + pulp pr + auto-release on merge ([#144](https://github.com/danielraffel/pulp/pull/144))
- ViewBridge: Processor editor lifecycle + VST3/CLAP wiring (Feature 1, Phase 1 + core Phase 2) ([#140](https://github.com/danielraffel/pulp/pull/140))
- MPE Phase 4: MIDI 2.0 UMP native path (closes #139) ([#141](https://github.com/danielraffel/pulp/pull/141))
- Advanced SDF text: MSDF + PSDF + effects + runtime atlas (supersedes #133) ([#136](https://github.com/danielraffel/pulp/pull/136))
- Address Codex P2 review on PR #135 (MPE) ([#138](https://github.com/danielraffel/pulp/pull/138))
- Streams: unified Stream / AsyncStream / network backends (Feature 3, Phase 1-3) ([#134](https://github.com/danielraffel/pulp/pull/134))
- Fix Android build: accessibility bridge API mismatch ([#137](https://github.com/danielraffel/pulp/pull/137))
- Add MPE (MIDI Polyphonic Expression) full support — Phases 1-3 ([#135](https://github.com/danielraffel/pulp/pull/135))
- Fix Codex P1 reviews: wire Android accessibility + fix Windows SDF include ([#132](https://github.com/danielraffel/pulp/pull/132))
- Fix SDF atlas font manager for Skia + add --screenshot mode to demo ([#131](https://github.com/danielraffel/pulp/pull/131))
- Add SDF text demo with bloom post-effect (#76 validation) ([#130](https://github.com/danielraffel/pulp/pull/130))
- Fix P1 Codex review issues from PRs #125-128 ([#129](https://github.com/danielraffel/pulp/pull/129))
- Wire Android MIDI FIFO to audio callback (#86) ([#128](https://github.com/danielraffel/pulp/pull/128))
- Implement Android TalkBack accessibility bridge (#87) ([#127](https://github.com/danielraffel/pulp/pull/127))
- Add fill_text_sdf() for GPU-accelerated resolution-independent text (#76) ([#126](https://github.com/danielraffel/pulp/pull/126))
- Enable SkShaper for OpenType kerning + ligatures in fill_text (fixes #75) ([#125](https://github.com/danielraffel/pulp/pull/125))
- CI: bump actions to Node.js 24 compatible versions ([#112](https://github.com/danielraffel/pulp/pull/112))
- Ship feature/shipyard-new-features ([#124](https://github.com/danielraffel/pulp/pull/124))
- Update docs for Shipyard as primary CI tool (post v0.3.0) ([#123](https://github.com/danielraffel/pulp/pull/123))
- Fix flaky Windows tests with proper wait patterns ([#122](https://github.com/danielraffel/pulp/pull/122))
- Ship feature/ci-skill-shipyard-preference ([#121](https://github.com/danielraffel/pulp/pull/121))

<a id="v030"></a>
## [0.3.0] - 2026-04-11

- Ship release/v0.3.0 ([#119](https://github.com/danielraffel/pulp/pull/119))
- Bump Shipyard pin to v0.1.3 (Phase 5 capability parity) ([#109](https://github.com/danielraffel/pulp/pull/109))
- Address Codex review on PR #106: Windows-safe shell + canonical stages ([#108](https://github.com/danielraffel/pulp/pull/108))
- Document contributor expectations + security baseline + Shipyard migration ([#107](https://github.com/danielraffel/pulp/pull/107))
- Add Shipyard adoption: pinned binary + project config + CI skill update ([#106](https://github.com/danielraffel/pulp/pull/106))

<a id="v022"></a>
## [0.2.2] - 2026-04-09

- Fix Windows SDK consumption: implib filename + standalone link ([#104](https://github.com/danielraffel/pulp/pull/104))
- SDF atlas: real SkFont rasterizer (replaces placeholder when Skia present) ([#103](https://github.com/danielraffel/pulp/pull/103))
- Fix #74: Add INSTALL_RPATH to pulp-cli for installed binaries ([#102](https://github.com/danielraffel/pulp/pull/102))

<a id="v021"></a>
## [0.2.1] - 2026-04-09

- SDF glyph atlas: foundation prototype + Felzenszwalb distance transform ([#100](https://github.com/danielraffel/pulp/pull/100))
- Android TalkBack: wire C++ view tree to AccessibilityDelegate ([#98](https://github.com/danielraffel/pulp/pull/98))
- Add regression test for #75 nested-clip text rendering ([#99](https://github.com/danielraffel/pulp/pull/99))
- Android MIDI I/O Phase 1: lock-free FIFO + PulpApplication wiring ([#97](https://github.com/danielraffel/pulp/pull/97))
- Fix #92: Move Windows platform includes outside pulp::view namespace ([#96](https://github.com/danielraffel/pulp/pull/96))
- Fix #94: Ship wgpu_native.lib in Windows SDK install ([#95](https://github.com/danielraffel/pulp/pull/95))

<a id="v020"></a>
## [0.2.0] - 2026-04-09

- release-cli: temporarily disable windows-arm64 matrix entry ([#93](https://github.com/danielraffel/pulp/pull/93))
- Android: support x86_64 Skia builds for emulator dev cycle ([#91](https://github.com/danielraffel/pulp/pull/91))
- Android: load synth_ui.js from APK assets via AAssetManager ([#90](https://github.com/danielraffel/pulp/pull/90))
- Add \`pulp create --targets android\` experimental scaffold ([#89](https://github.com/danielraffel/pulp/pull/89))
- Add zero-crossing trigger to WaveformView for stable scope display ([#88](https://github.com/danielraffel/pulp/pull/88))
- Add notarize, appcast, and Android packaging to ship CLI ([#79](https://github.com/danielraffel/pulp/pull/79))
- Disable android-emulator-test CI job until infrastructure ready (#77) ([#78](https://github.com/danielraffel/pulp/pull/78))
- Android Platform: Full GPU rendering pipeline + interactive Pulp widgets ([#53](https://github.com/danielraffel/pulp/pull/53))
- Fix modules.md code examples to match actual APIs ([#70](https://github.com/danielraffel/pulp/pull/70))
- Add Skill Maintenance Rule to CLAUDE.md ([#72](https://github.com/danielraffel/pulp/pull/72))
- Inspector polish: cursor balance, selection persistence, highlight stability, scrollbar, positioning ([#71](https://github.com/danielraffel/pulp/pull/71))
- Inspector: tabs, smart positioning, cursor types, highlight sync, PR review fixes ([#68](https://github.com/danielraffel/pulp/pull/68))
- Add separate floating inspector window with TreeView and PropertyList ([#64](https://github.com/danielraffel/pulp/pull/64))
- Fix MSVC portability: add missing #include <algorithm> across 16 files ([#62](https://github.com/danielraffel/pulp/pull/62))
- Modularize CLI: split 4,903-line monolith into focused command files ([#61](https://github.com/danielraffel/pulp/pull/61))
- Add plugin inspector: visual overlay, audio monitoring, TCP protocol, CLI client ([#60](https://github.com/danielraffel/pulp/pull/60))
- Fix PR #52 review: IPC handoff ordering + iOS accessibility CMakeLists ([#54](https://github.com/danielraffel/pulp/pull/54))
- Add iOS VoiceOver accessibility via UIAccessibility ([#43](https://github.com/danielraffel/pulp/pull/43))
- Add rendering reference docs and update module descriptions ([#45](https://github.com/danielraffel/pulp/pull/45))
- Rendering capability Phase A: Foundation infrastructure ([#14](https://github.com/danielraffel/pulp/pull/14))
- Add planning as private submodule, codify planning workflow ([#11](https://github.com/danielraffel/pulp/pull/11))
- Add compute pipeline, CI failover, SDK commands ([#12](https://github.com/danielraffel/pulp/pull/12))
- Add pulp sdk install/status/clean commands (closes #8) ([#10](https://github.com/danielraffel/pulp/pull/10))
- Add step-by-step SSH setup guides for Windows and Linux VMs ([#6](https://github.com/danielraffel/pulp/pull/6))
- Complete Phase 1: vDSP FFT, NSIS CLI, glTF texture tests ([#5](https://github.com/danielraffel/pulp/pull/5))
- Phase 1 follow-up: glTF textures, NSIS fixes, issue #3 crash/mirror/run ([#4](https://github.com/danielraffel/pulp/pull/4))
- Phase 1: Commercial readiness — convolver, image rendering, packaging, MSVC fix ([#2](https://github.com/danielraffel/pulp/pull/2))

[0.668.0]: https://github.com/danielraffel/pulp/releases/tag/v0.668.0
[0.665.0]: https://github.com/danielraffel/pulp/releases/tag/v0.665.0
[0.664.0]: https://github.com/danielraffel/pulp/releases/tag/v0.664.0
[0.663.1]: https://github.com/danielraffel/pulp/releases/tag/v0.663.1
[0.663.0]: https://github.com/danielraffel/pulp/releases/tag/v0.663.0
[0.662.0]: https://github.com/danielraffel/pulp/releases/tag/v0.662.0
[0.661.0]: https://github.com/danielraffel/pulp/releases/tag/v0.661.0
[0.660.0]: https://github.com/danielraffel/pulp/releases/tag/v0.660.0
[0.658.1]: https://github.com/danielraffel/pulp/releases/tag/v0.658.1
[0.658.0]: https://github.com/danielraffel/pulp/releases/tag/v0.658.0
[0.657.0]: https://github.com/danielraffel/pulp/releases/tag/v0.657.0
[0.655.0]: https://github.com/danielraffel/pulp/releases/tag/v0.655.0
[0.654.0]: https://github.com/danielraffel/pulp/releases/tag/v0.654.0
[0.653.0]: https://github.com/danielraffel/pulp/releases/tag/v0.653.0
[0.652.0]: https://github.com/danielraffel/pulp/releases/tag/v0.652.0
[0.651.0]: https://github.com/danielraffel/pulp/releases/tag/v0.651.0
[0.650.0]: https://github.com/danielraffel/pulp/releases/tag/v0.650.0
[0.649.0]: https://github.com/danielraffel/pulp/releases/tag/v0.649.0
[0.647.0]: https://github.com/danielraffel/pulp/releases/tag/v0.647.0
[0.645.0]: https://github.com/danielraffel/pulp/releases/tag/v0.645.0
[0.644.0]: https://github.com/danielraffel/pulp/releases/tag/v0.644.0
[0.643.0]: https://github.com/danielraffel/pulp/releases/tag/v0.643.0
[0.641.0]: https://github.com/danielraffel/pulp/releases/tag/v0.641.0
[0.639.0]: https://github.com/danielraffel/pulp/releases/tag/v0.639.0
[0.638.1]: https://github.com/danielraffel/pulp/releases/tag/v0.638.1
[0.638.0]: https://github.com/danielraffel/pulp/releases/tag/v0.638.0
[0.637.0]: https://github.com/danielraffel/pulp/releases/tag/v0.637.0
[0.636.0]: https://github.com/danielraffel/pulp/releases/tag/v0.636.0
[0.635.0]: https://github.com/danielraffel/pulp/releases/tag/v0.635.0
[0.634.0]: https://github.com/danielraffel/pulp/releases/tag/v0.634.0
[0.626.0]: https://github.com/danielraffel/pulp/releases/tag/v0.626.0
[0.624.0]: https://github.com/danielraffel/pulp/releases/tag/v0.624.0
[0.623.1]: https://github.com/danielraffel/pulp/releases/tag/v0.623.1
[0.622.0]: https://github.com/danielraffel/pulp/releases/tag/v0.622.0
[0.619.0]: https://github.com/danielraffel/pulp/releases/tag/v0.619.0
[0.617.0]: https://github.com/danielraffel/pulp/releases/tag/v0.617.0
[0.615.0]: https://github.com/danielraffel/pulp/releases/tag/v0.615.0
[0.614.0]: https://github.com/danielraffel/pulp/releases/tag/v0.614.0
[0.613.0]: https://github.com/danielraffel/pulp/releases/tag/v0.613.0
[0.612.0]: https://github.com/danielraffel/pulp/releases/tag/v0.612.0
[0.611.0]: https://github.com/danielraffel/pulp/releases/tag/v0.611.0
[0.610.0]: https://github.com/danielraffel/pulp/releases/tag/v0.610.0
[0.609.1]: https://github.com/danielraffel/pulp/releases/tag/v0.609.1
[0.609.0]: https://github.com/danielraffel/pulp/releases/tag/v0.609.0
[0.608.0]: https://github.com/danielraffel/pulp/releases/tag/v0.608.0
[0.607.0]: https://github.com/danielraffel/pulp/releases/tag/v0.607.0
[0.606.0]: https://github.com/danielraffel/pulp/releases/tag/v0.606.0
[0.605.0]: https://github.com/danielraffel/pulp/releases/tag/v0.605.0
[0.604.0]: https://github.com/danielraffel/pulp/releases/tag/v0.604.0
[0.603.0]: https://github.com/danielraffel/pulp/releases/tag/v0.603.0
[0.602.0]: https://github.com/danielraffel/pulp/releases/tag/v0.602.0
[0.600.0]: https://github.com/danielraffel/pulp/releases/tag/v0.600.0
[0.599.0]: https://github.com/danielraffel/pulp/releases/tag/v0.599.0
[0.598.0]: https://github.com/danielraffel/pulp/releases/tag/v0.598.0
[0.597.0]: https://github.com/danielraffel/pulp/releases/tag/v0.597.0
[0.596.0]: https://github.com/danielraffel/pulp/releases/tag/v0.596.0
[0.595.0]: https://github.com/danielraffel/pulp/releases/tag/v0.595.0
[0.594.0]: https://github.com/danielraffel/pulp/releases/tag/v0.594.0
[0.593.0]: https://github.com/danielraffel/pulp/releases/tag/v0.593.0
[0.592.0]: https://github.com/danielraffel/pulp/releases/tag/v0.592.0
[0.591.0]: https://github.com/danielraffel/pulp/releases/tag/v0.591.0
[0.590.0]: https://github.com/danielraffel/pulp/releases/tag/v0.590.0
[0.589.0]: https://github.com/danielraffel/pulp/releases/tag/v0.589.0
[0.588.0]: https://github.com/danielraffel/pulp/releases/tag/v0.588.0
[0.587.0]: https://github.com/danielraffel/pulp/releases/tag/v0.587.0
[0.586.0]: https://github.com/danielraffel/pulp/releases/tag/v0.586.0
[0.584.0]: https://github.com/danielraffel/pulp/releases/tag/v0.584.0
[0.583.0]: https://github.com/danielraffel/pulp/releases/tag/v0.583.0
[0.582.0]: https://github.com/danielraffel/pulp/releases/tag/v0.582.0
[0.580.0]: https://github.com/danielraffel/pulp/releases/tag/v0.580.0
[0.579.0]: https://github.com/danielraffel/pulp/releases/tag/v0.579.0
[0.578.2]: https://github.com/danielraffel/pulp/releases/tag/v0.578.2
[0.578.0]: https://github.com/danielraffel/pulp/releases/tag/v0.578.0
[0.577.0]: https://github.com/danielraffel/pulp/releases/tag/v0.577.0
[0.576.0]: https://github.com/danielraffel/pulp/releases/tag/v0.576.0
[0.575.0]: https://github.com/danielraffel/pulp/releases/tag/v0.575.0
[0.574.0]: https://github.com/danielraffel/pulp/releases/tag/v0.574.0
[0.573.0]: https://github.com/danielraffel/pulp/releases/tag/v0.573.0
[0.572.0]: https://github.com/danielraffel/pulp/releases/tag/v0.572.0
[0.571.0]: https://github.com/danielraffel/pulp/releases/tag/v0.571.0
[0.570.0]: https://github.com/danielraffel/pulp/releases/tag/v0.570.0
[0.569.0]: https://github.com/danielraffel/pulp/releases/tag/v0.569.0
[0.568.0]: https://github.com/danielraffel/pulp/releases/tag/v0.568.0
[0.567.0]: https://github.com/danielraffel/pulp/releases/tag/v0.567.0
[0.566.0]: https://github.com/danielraffel/pulp/releases/tag/v0.566.0
[0.563.0]: https://github.com/danielraffel/pulp/releases/tag/v0.563.0
[0.562.0]: https://github.com/danielraffel/pulp/releases/tag/v0.562.0
[0.561.0]: https://github.com/danielraffel/pulp/releases/tag/v0.561.0
[0.560.0]: https://github.com/danielraffel/pulp/releases/tag/v0.560.0
[0.559.0]: https://github.com/danielraffel/pulp/releases/tag/v0.559.0
[0.558.0]: https://github.com/danielraffel/pulp/releases/tag/v0.558.0
[0.557.0]: https://github.com/danielraffel/pulp/releases/tag/v0.557.0
[0.556.0]: https://github.com/danielraffel/pulp/releases/tag/v0.556.0
[0.554.0]: https://github.com/danielraffel/pulp/releases/tag/v0.554.0
[0.553.0]: https://github.com/danielraffel/pulp/releases/tag/v0.553.0
[0.552.0]: https://github.com/danielraffel/pulp/releases/tag/v0.552.0
[0.551.0]: https://github.com/danielraffel/pulp/releases/tag/v0.551.0
[0.550.0]: https://github.com/danielraffel/pulp/releases/tag/v0.550.0
[0.549.3]: https://github.com/danielraffel/pulp/releases/tag/v0.549.3
[0.549.2]: https://github.com/danielraffel/pulp/releases/tag/v0.549.2
[0.549.1]: https://github.com/danielraffel/pulp/releases/tag/v0.549.1
[0.549.0]: https://github.com/danielraffel/pulp/releases/tag/v0.549.0
[0.548.0]: https://github.com/danielraffel/pulp/releases/tag/v0.548.0
[0.547.0]: https://github.com/danielraffel/pulp/releases/tag/v0.547.0
[0.546.0]: https://github.com/danielraffel/pulp/releases/tag/v0.546.0
[0.545.0]: https://github.com/danielraffel/pulp/releases/tag/v0.545.0
[0.544.0]: https://github.com/danielraffel/pulp/releases/tag/v0.544.0
[0.543.0]: https://github.com/danielraffel/pulp/releases/tag/v0.543.0
[0.542.0]: https://github.com/danielraffel/pulp/releases/tag/v0.542.0
[0.541.0]: https://github.com/danielraffel/pulp/releases/tag/v0.541.0
[0.540.0]: https://github.com/danielraffel/pulp/releases/tag/v0.540.0
[0.539.0]: https://github.com/danielraffel/pulp/releases/tag/v0.539.0
[0.538.0]: https://github.com/danielraffel/pulp/releases/tag/v0.538.0
[0.537.0]: https://github.com/danielraffel/pulp/releases/tag/v0.537.0
[0.536.0]: https://github.com/danielraffel/pulp/releases/tag/v0.536.0
[0.535.0]: https://github.com/danielraffel/pulp/releases/tag/v0.535.0
[0.534.3]: https://github.com/danielraffel/pulp/releases/tag/v0.534.3
[0.534.2]: https://github.com/danielraffel/pulp/releases/tag/v0.534.2
[0.534.1]: https://github.com/danielraffel/pulp/releases/tag/v0.534.1
[0.534.0]: https://github.com/danielraffel/pulp/releases/tag/v0.534.0
[0.533.1]: https://github.com/danielraffel/pulp/releases/tag/v0.533.1
[0.533.0]: https://github.com/danielraffel/pulp/releases/tag/v0.533.0
[0.532.1]: https://github.com/danielraffel/pulp/releases/tag/v0.532.1
[0.532.0]: https://github.com/danielraffel/pulp/releases/tag/v0.532.0
[0.531.1]: https://github.com/danielraffel/pulp/releases/tag/v0.531.1
[0.531.0]: https://github.com/danielraffel/pulp/releases/tag/v0.531.0
[0.530.0]: https://github.com/danielraffel/pulp/releases/tag/v0.530.0
[0.529.0]: https://github.com/danielraffel/pulp/releases/tag/v0.529.0
[0.528.2]: https://github.com/danielraffel/pulp/releases/tag/v0.528.2
[0.528.1]: https://github.com/danielraffel/pulp/releases/tag/v0.528.1
[0.528.0]: https://github.com/danielraffel/pulp/releases/tag/v0.528.0
[0.527.0]: https://github.com/danielraffel/pulp/releases/tag/v0.527.0
[0.526.0]: https://github.com/danielraffel/pulp/releases/tag/v0.526.0
[0.524.0]: https://github.com/danielraffel/pulp/releases/tag/v0.524.0
[0.523.0]: https://github.com/danielraffel/pulp/releases/tag/v0.523.0
[0.522.0]: https://github.com/danielraffel/pulp/releases/tag/v0.522.0
[0.521.1]: https://github.com/danielraffel/pulp/releases/tag/v0.521.1
[0.521.0]: https://github.com/danielraffel/pulp/releases/tag/v0.521.0
[0.520.0]: https://github.com/danielraffel/pulp/releases/tag/v0.520.0
[0.519.0]: https://github.com/danielraffel/pulp/releases/tag/v0.519.0
[0.518.0]: https://github.com/danielraffel/pulp/releases/tag/v0.518.0
[0.517.3]: https://github.com/danielraffel/pulp/releases/tag/v0.517.3
[0.517.2]: https://github.com/danielraffel/pulp/releases/tag/v0.517.2
[0.517.1]: https://github.com/danielraffel/pulp/releases/tag/v0.517.1
[0.517.0]: https://github.com/danielraffel/pulp/releases/tag/v0.517.0
[0.516.0]: https://github.com/danielraffel/pulp/releases/tag/v0.516.0
[0.515.0]: https://github.com/danielraffel/pulp/releases/tag/v0.515.0
[0.514.0]: https://github.com/danielraffel/pulp/releases/tag/v0.514.0
[0.513.0]: https://github.com/danielraffel/pulp/releases/tag/v0.513.0
[0.512.0]: https://github.com/danielraffel/pulp/releases/tag/v0.512.0
[0.511.1]: https://github.com/danielraffel/pulp/releases/tag/v0.511.1
[0.511.0]: https://github.com/danielraffel/pulp/releases/tag/v0.511.0
[0.510.0]: https://github.com/danielraffel/pulp/releases/tag/v0.510.0
[0.509.3]: https://github.com/danielraffel/pulp/releases/tag/v0.509.3
[0.509.2]: https://github.com/danielraffel/pulp/releases/tag/v0.509.2
[0.509.1]: https://github.com/danielraffel/pulp/releases/tag/v0.509.1
[0.509.0]: https://github.com/danielraffel/pulp/releases/tag/v0.509.0
[0.508.1]: https://github.com/danielraffel/pulp/releases/tag/v0.508.1
[0.508.0]: https://github.com/danielraffel/pulp/releases/tag/v0.508.0
[0.507.0]: https://github.com/danielraffel/pulp/releases/tag/v0.507.0
[0.506.0]: https://github.com/danielraffel/pulp/releases/tag/v0.506.0
[0.505.0]: https://github.com/danielraffel/pulp/releases/tag/v0.505.0
[0.504.0]: https://github.com/danielraffel/pulp/releases/tag/v0.504.0
[0.503.0]: https://github.com/danielraffel/pulp/releases/tag/v0.503.0
[0.502.0]: https://github.com/danielraffel/pulp/releases/tag/v0.502.0
[0.501.1]: https://github.com/danielraffel/pulp/releases/tag/v0.501.1
[0.501.0]: https://github.com/danielraffel/pulp/releases/tag/v0.501.0
[0.500.0]: https://github.com/danielraffel/pulp/releases/tag/v0.500.0
[0.499.1]: https://github.com/danielraffel/pulp/releases/tag/v0.499.1
[0.499.0]: https://github.com/danielraffel/pulp/releases/tag/v0.499.0
[0.498.0]: https://github.com/danielraffel/pulp/releases/tag/v0.498.0
[0.497.0]: https://github.com/danielraffel/pulp/releases/tag/v0.497.0
[0.496.1]: https://github.com/danielraffel/pulp/releases/tag/v0.496.1
[0.496.0]: https://github.com/danielraffel/pulp/releases/tag/v0.496.0
[0.495.3]: https://github.com/danielraffel/pulp/releases/tag/v0.495.3
[0.495.2]: https://github.com/danielraffel/pulp/releases/tag/v0.495.2
[0.495.1]: https://github.com/danielraffel/pulp/releases/tag/v0.495.1
[0.495.0]: https://github.com/danielraffel/pulp/releases/tag/v0.495.0
[0.494.0]: https://github.com/danielraffel/pulp/releases/tag/v0.494.0
[0.493.1]: https://github.com/danielraffel/pulp/releases/tag/v0.493.1
[0.493.0]: https://github.com/danielraffel/pulp/releases/tag/v0.493.0
[0.492.0]: https://github.com/danielraffel/pulp/releases/tag/v0.492.0
[0.491.0]: https://github.com/danielraffel/pulp/releases/tag/v0.491.0
[0.490.0]: https://github.com/danielraffel/pulp/releases/tag/v0.490.0
[0.489.0]: https://github.com/danielraffel/pulp/releases/tag/v0.489.0
[0.488.0]: https://github.com/danielraffel/pulp/releases/tag/v0.488.0
[0.487.0]: https://github.com/danielraffel/pulp/releases/tag/v0.487.0
[0.486.0]: https://github.com/danielraffel/pulp/releases/tag/v0.486.0
[0.485.0]: https://github.com/danielraffel/pulp/releases/tag/v0.485.0
[0.484.0]: https://github.com/danielraffel/pulp/releases/tag/v0.484.0
[0.483.0]: https://github.com/danielraffel/pulp/releases/tag/v0.483.0
[0.482.0]: https://github.com/danielraffel/pulp/releases/tag/v0.482.0
[0.481.0]: https://github.com/danielraffel/pulp/releases/tag/v0.481.0
[0.480.0]: https://github.com/danielraffel/pulp/releases/tag/v0.480.0
[0.479.0]: https://github.com/danielraffel/pulp/releases/tag/v0.479.0
[0.478.0]: https://github.com/danielraffel/pulp/releases/tag/v0.478.0
[0.477.0]: https://github.com/danielraffel/pulp/releases/tag/v0.477.0
[0.476.0]: https://github.com/danielraffel/pulp/releases/tag/v0.476.0
[0.475.0]: https://github.com/danielraffel/pulp/releases/tag/v0.475.0
[0.474.0]: https://github.com/danielraffel/pulp/releases/tag/v0.474.0
[0.473.0]: https://github.com/danielraffel/pulp/releases/tag/v0.473.0
[0.472.2]: https://github.com/danielraffel/pulp/releases/tag/v0.472.2
[0.472.1]: https://github.com/danielraffel/pulp/releases/tag/v0.472.1
[0.472.0]: https://github.com/danielraffel/pulp/releases/tag/v0.472.0
[0.471.0]: https://github.com/danielraffel/pulp/releases/tag/v0.471.0
[0.470.2]: https://github.com/danielraffel/pulp/releases/tag/v0.470.2
[0.470.1]: https://github.com/danielraffel/pulp/releases/tag/v0.470.1
[0.470.0]: https://github.com/danielraffel/pulp/releases/tag/v0.470.0
[0.469.1]: https://github.com/danielraffel/pulp/releases/tag/v0.469.1
[0.469.0]: https://github.com/danielraffel/pulp/releases/tag/v0.469.0
[0.468.0]: https://github.com/danielraffel/pulp/releases/tag/v0.468.0
[0.467.0]: https://github.com/danielraffel/pulp/releases/tag/v0.467.0
[0.466.3]: https://github.com/danielraffel/pulp/releases/tag/v0.466.3
[0.466.2]: https://github.com/danielraffel/pulp/releases/tag/v0.466.2
[0.466.1]: https://github.com/danielraffel/pulp/releases/tag/v0.466.1
[0.466.0]: https://github.com/danielraffel/pulp/releases/tag/v0.466.0
[0.465.0]: https://github.com/danielraffel/pulp/releases/tag/v0.465.0
[0.464.0]: https://github.com/danielraffel/pulp/releases/tag/v0.464.0
[0.463.9]: https://github.com/danielraffel/pulp/releases/tag/v0.463.9
[0.463.8]: https://github.com/danielraffel/pulp/releases/tag/v0.463.8
[0.463.7]: https://github.com/danielraffel/pulp/releases/tag/v0.463.7
[0.463.6]: https://github.com/danielraffel/pulp/releases/tag/v0.463.6
[0.463.5]: https://github.com/danielraffel/pulp/releases/tag/v0.463.5
[0.463.4]: https://github.com/danielraffel/pulp/releases/tag/v0.463.4
[0.463.3]: https://github.com/danielraffel/pulp/releases/tag/v0.463.3
[0.463.2]: https://github.com/danielraffel/pulp/releases/tag/v0.463.2
[0.463.1]: https://github.com/danielraffel/pulp/releases/tag/v0.463.1
[0.463.0]: https://github.com/danielraffel/pulp/releases/tag/v0.463.0
[0.462.1]: https://github.com/danielraffel/pulp/releases/tag/v0.462.1
[0.462.0]: https://github.com/danielraffel/pulp/releases/tag/v0.462.0
[0.461.0]: https://github.com/danielraffel/pulp/releases/tag/v0.461.0
[0.460.0]: https://github.com/danielraffel/pulp/releases/tag/v0.460.0
[0.459.7]: https://github.com/danielraffel/pulp/releases/tag/v0.459.7
[0.459.6]: https://github.com/danielraffel/pulp/releases/tag/v0.459.6
[0.459.5]: https://github.com/danielraffel/pulp/releases/tag/v0.459.5
[0.459.4]: https://github.com/danielraffel/pulp/releases/tag/v0.459.4
[0.459.3]: https://github.com/danielraffel/pulp/releases/tag/v0.459.3
[0.459.2]: https://github.com/danielraffel/pulp/releases/tag/v0.459.2
[0.459.1]: https://github.com/danielraffel/pulp/releases/tag/v0.459.1
[0.459.0]: https://github.com/danielraffel/pulp/releases/tag/v0.459.0
[0.458.0]: https://github.com/danielraffel/pulp/releases/tag/v0.458.0
[0.457.0]: https://github.com/danielraffel/pulp/releases/tag/v0.457.0
[0.456.2]: https://github.com/danielraffel/pulp/releases/tag/v0.456.2
[0.456.1]: https://github.com/danielraffel/pulp/releases/tag/v0.456.1
[0.456.0]: https://github.com/danielraffel/pulp/releases/tag/v0.456.0
[0.455.0]: https://github.com/danielraffel/pulp/releases/tag/v0.455.0
[0.454.0]: https://github.com/danielraffel/pulp/releases/tag/v0.454.0
[0.453.0]: https://github.com/danielraffel/pulp/releases/tag/v0.453.0
[0.452.0]: https://github.com/danielraffel/pulp/releases/tag/v0.452.0
[0.451.0]: https://github.com/danielraffel/pulp/releases/tag/v0.451.0
[0.450.0]: https://github.com/danielraffel/pulp/releases/tag/v0.450.0
[0.449.0]: https://github.com/danielraffel/pulp/releases/tag/v0.449.0
[0.448.0]: https://github.com/danielraffel/pulp/releases/tag/v0.448.0
[0.447.0]: https://github.com/danielraffel/pulp/releases/tag/v0.447.0
[0.446.1]: https://github.com/danielraffel/pulp/releases/tag/v0.446.1
[0.446.0]: https://github.com/danielraffel/pulp/releases/tag/v0.446.0
[0.445.0]: https://github.com/danielraffel/pulp/releases/tag/v0.445.0
[0.444.2]: https://github.com/danielraffel/pulp/releases/tag/v0.444.2
[0.444.1]: https://github.com/danielraffel/pulp/releases/tag/v0.444.1
[0.444.0]: https://github.com/danielraffel/pulp/releases/tag/v0.444.0
[0.443.0]: https://github.com/danielraffel/pulp/releases/tag/v0.443.0
[0.442.0]: https://github.com/danielraffel/pulp/releases/tag/v0.442.0
[0.441.1]: https://github.com/danielraffel/pulp/releases/tag/v0.441.1
[0.441.0]: https://github.com/danielraffel/pulp/releases/tag/v0.441.0
[0.440.1]: https://github.com/danielraffel/pulp/releases/tag/v0.440.1
[0.440.0]: https://github.com/danielraffel/pulp/releases/tag/v0.440.0
[0.439.0]: https://github.com/danielraffel/pulp/releases/tag/v0.439.0
[0.438.0]: https://github.com/danielraffel/pulp/releases/tag/v0.438.0
[0.437.1]: https://github.com/danielraffel/pulp/releases/tag/v0.437.1
[0.437.0]: https://github.com/danielraffel/pulp/releases/tag/v0.437.0
[0.436.0]: https://github.com/danielraffel/pulp/releases/tag/v0.436.0
[0.435.0]: https://github.com/danielraffel/pulp/releases/tag/v0.435.0
[0.434.0]: https://github.com/danielraffel/pulp/releases/tag/v0.434.0
[0.433.0]: https://github.com/danielraffel/pulp/releases/tag/v0.433.0
[0.432.1]: https://github.com/danielraffel/pulp/releases/tag/v0.432.1
[0.432.0]: https://github.com/danielraffel/pulp/releases/tag/v0.432.0
[0.431.2]: https://github.com/danielraffel/pulp/releases/tag/v0.431.2
[0.431.1]: https://github.com/danielraffel/pulp/releases/tag/v0.431.1
[0.431.0]: https://github.com/danielraffel/pulp/releases/tag/v0.431.0
[0.430.0]: https://github.com/danielraffel/pulp/releases/tag/v0.430.0
[0.429.0]: https://github.com/danielraffel/pulp/releases/tag/v0.429.0
[0.428.0]: https://github.com/danielraffel/pulp/releases/tag/v0.428.0
[0.427.0]: https://github.com/danielraffel/pulp/releases/tag/v0.427.0
[0.426.1]: https://github.com/danielraffel/pulp/releases/tag/v0.426.1
[0.426.0]: https://github.com/danielraffel/pulp/releases/tag/v0.426.0
[0.425.0]: https://github.com/danielraffel/pulp/releases/tag/v0.425.0
[0.424.0]: https://github.com/danielraffel/pulp/releases/tag/v0.424.0
[0.423.0]: https://github.com/danielraffel/pulp/releases/tag/v0.423.0
[0.422.0]: https://github.com/danielraffel/pulp/releases/tag/v0.422.0
[0.421.0]: https://github.com/danielraffel/pulp/releases/tag/v0.421.0
[0.420.0]: https://github.com/danielraffel/pulp/releases/tag/v0.420.0
[0.419.0]: https://github.com/danielraffel/pulp/releases/tag/v0.419.0
[0.418.0]: https://github.com/danielraffel/pulp/releases/tag/v0.418.0
[0.417.0]: https://github.com/danielraffel/pulp/releases/tag/v0.417.0
[0.416.0]: https://github.com/danielraffel/pulp/releases/tag/v0.416.0
[0.415.0]: https://github.com/danielraffel/pulp/releases/tag/v0.415.0
[0.414.0]: https://github.com/danielraffel/pulp/releases/tag/v0.414.0
[0.413.0]: https://github.com/danielraffel/pulp/releases/tag/v0.413.0
[0.412.2]: https://github.com/danielraffel/pulp/releases/tag/v0.412.2
[0.412.0]: https://github.com/danielraffel/pulp/releases/tag/v0.412.0
[0.411.0]: https://github.com/danielraffel/pulp/releases/tag/v0.411.0
[0.410.0]: https://github.com/danielraffel/pulp/releases/tag/v0.410.0
[0.409.0]: https://github.com/danielraffel/pulp/releases/tag/v0.409.0
[0.408.0]: https://github.com/danielraffel/pulp/releases/tag/v0.408.0
[0.407.0]: https://github.com/danielraffel/pulp/releases/tag/v0.407.0
[0.406.0]: https://github.com/danielraffel/pulp/releases/tag/v0.406.0
[0.405.0]: https://github.com/danielraffel/pulp/releases/tag/v0.405.0
[0.404.0]: https://github.com/danielraffel/pulp/releases/tag/v0.404.0
[0.403.0]: https://github.com/danielraffel/pulp/releases/tag/v0.403.0
[0.402.0]: https://github.com/danielraffel/pulp/releases/tag/v0.402.0
[0.401.0]: https://github.com/danielraffel/pulp/releases/tag/v0.401.0
[0.400.0]: https://github.com/danielraffel/pulp/releases/tag/v0.400.0
[0.399.0]: https://github.com/danielraffel/pulp/releases/tag/v0.399.0
[0.398.0]: https://github.com/danielraffel/pulp/releases/tag/v0.398.0
[0.397.0]: https://github.com/danielraffel/pulp/releases/tag/v0.397.0
[0.396.0]: https://github.com/danielraffel/pulp/releases/tag/v0.396.0
[0.395.0]: https://github.com/danielraffel/pulp/releases/tag/v0.395.0
[0.394.0]: https://github.com/danielraffel/pulp/releases/tag/v0.394.0
[0.393.0]: https://github.com/danielraffel/pulp/releases/tag/v0.393.0
[0.392.2]: https://github.com/danielraffel/pulp/releases/tag/v0.392.2
[0.392.1]: https://github.com/danielraffel/pulp/releases/tag/v0.392.1
[0.392.0]: https://github.com/danielraffel/pulp/releases/tag/v0.392.0
[0.391.0]: https://github.com/danielraffel/pulp/releases/tag/v0.391.0
[0.390.0]: https://github.com/danielraffel/pulp/releases/tag/v0.390.0
[0.389.0]: https://github.com/danielraffel/pulp/releases/tag/v0.389.0
[0.388.0]: https://github.com/danielraffel/pulp/releases/tag/v0.388.0
[0.387.4]: https://github.com/danielraffel/pulp/releases/tag/v0.387.4
[0.387.3]: https://github.com/danielraffel/pulp/releases/tag/v0.387.3
[0.386.1]: https://github.com/danielraffel/pulp/releases/tag/v0.386.1
[0.386.0]: https://github.com/danielraffel/pulp/releases/tag/v0.386.0
[0.385.0]: https://github.com/danielraffel/pulp/releases/tag/v0.385.0
[0.384.0]: https://github.com/danielraffel/pulp/releases/tag/v0.384.0
[0.383.0]: https://github.com/danielraffel/pulp/releases/tag/v0.383.0
[0.382.1]: https://github.com/danielraffel/pulp/releases/tag/v0.382.1
[0.382.0]: https://github.com/danielraffel/pulp/releases/tag/v0.382.0
[0.381.0]: https://github.com/danielraffel/pulp/releases/tag/v0.381.0
[0.380.0]: https://github.com/danielraffel/pulp/releases/tag/v0.380.0
[0.379.0]: https://github.com/danielraffel/pulp/releases/tag/v0.379.0
[0.378.0]: https://github.com/danielraffel/pulp/releases/tag/v0.378.0
[0.371.1]: https://github.com/danielraffel/pulp/releases/tag/v0.371.1
[0.371.0]: https://github.com/danielraffel/pulp/releases/tag/v0.371.0
[0.370.1]: https://github.com/danielraffel/pulp/releases/tag/v0.370.1
[0.370.0]: https://github.com/danielraffel/pulp/releases/tag/v0.370.0
[0.369.0]: https://github.com/danielraffel/pulp/releases/tag/v0.369.0
[0.368.0]: https://github.com/danielraffel/pulp/releases/tag/v0.368.0
[0.367.0]: https://github.com/danielraffel/pulp/releases/tag/v0.367.0
[0.366.0]: https://github.com/danielraffel/pulp/releases/tag/v0.366.0
[0.365.0]: https://github.com/danielraffel/pulp/releases/tag/v0.365.0
[0.364.0]: https://github.com/danielraffel/pulp/releases/tag/v0.364.0
[0.363.0]: https://github.com/danielraffel/pulp/releases/tag/v0.363.0
[0.362.0]: https://github.com/danielraffel/pulp/releases/tag/v0.362.0
[0.361.0]: https://github.com/danielraffel/pulp/releases/tag/v0.361.0
[0.360.2]: https://github.com/danielraffel/pulp/releases/tag/v0.360.2
[0.360.1]: https://github.com/danielraffel/pulp/releases/tag/v0.360.1
[0.360.0]: https://github.com/danielraffel/pulp/releases/tag/v0.360.0
[0.359.1]: https://github.com/danielraffel/pulp/releases/tag/v0.359.1
[0.359.0]: https://github.com/danielraffel/pulp/releases/tag/v0.359.0
[0.358.1]: https://github.com/danielraffel/pulp/releases/tag/v0.358.1
[0.358.0]: https://github.com/danielraffel/pulp/releases/tag/v0.358.0
[0.357.0]: https://github.com/danielraffel/pulp/releases/tag/v0.357.0
[0.356.0]: https://github.com/danielraffel/pulp/releases/tag/v0.356.0
[0.355.1]: https://github.com/danielraffel/pulp/releases/tag/v0.355.1
[0.355.0]: https://github.com/danielraffel/pulp/releases/tag/v0.355.0
[0.354.0]: https://github.com/danielraffel/pulp/releases/tag/v0.354.0
[0.353.0]: https://github.com/danielraffel/pulp/releases/tag/v0.353.0
[0.352.0]: https://github.com/danielraffel/pulp/releases/tag/v0.352.0
[0.351.0]: https://github.com/danielraffel/pulp/releases/tag/v0.351.0
[0.350.0]: https://github.com/danielraffel/pulp/releases/tag/v0.350.0
[0.349.0]: https://github.com/danielraffel/pulp/releases/tag/v0.349.0
[0.348.0]: https://github.com/danielraffel/pulp/releases/tag/v0.348.0
[0.347.0]: https://github.com/danielraffel/pulp/releases/tag/v0.347.0
[0.346.0]: https://github.com/danielraffel/pulp/releases/tag/v0.346.0
[0.345.0]: https://github.com/danielraffel/pulp/releases/tag/v0.345.0
[0.344.0]: https://github.com/danielraffel/pulp/releases/tag/v0.344.0
[0.343.0]: https://github.com/danielraffel/pulp/releases/tag/v0.343.0
[0.342.0]: https://github.com/danielraffel/pulp/releases/tag/v0.342.0
[0.341.0]: https://github.com/danielraffel/pulp/releases/tag/v0.341.0
[0.340.0]: https://github.com/danielraffel/pulp/releases/tag/v0.340.0
[0.339.0]: https://github.com/danielraffel/pulp/releases/tag/v0.339.0
[0.338.0]: https://github.com/danielraffel/pulp/releases/tag/v0.338.0
[0.337.0]: https://github.com/danielraffel/pulp/releases/tag/v0.337.0
[0.336.0]: https://github.com/danielraffel/pulp/releases/tag/v0.336.0
[0.335.0]: https://github.com/danielraffel/pulp/releases/tag/v0.335.0
[0.334.0]: https://github.com/danielraffel/pulp/releases/tag/v0.334.0
[0.333.0]: https://github.com/danielraffel/pulp/releases/tag/v0.333.0
[0.332.1]: https://github.com/danielraffel/pulp/releases/tag/v0.332.1
[0.332.0]: https://github.com/danielraffel/pulp/releases/tag/v0.332.0
[0.331.0]: https://github.com/danielraffel/pulp/releases/tag/v0.331.0
[0.330.0]: https://github.com/danielraffel/pulp/releases/tag/v0.330.0
[0.329.0]: https://github.com/danielraffel/pulp/releases/tag/v0.329.0
[0.328.0]: https://github.com/danielraffel/pulp/releases/tag/v0.328.0
[0.327.0]: https://github.com/danielraffel/pulp/releases/tag/v0.327.0
[0.326.0]: https://github.com/danielraffel/pulp/releases/tag/v0.326.0
[0.325.0]: https://github.com/danielraffel/pulp/releases/tag/v0.325.0
[0.324.0]: https://github.com/danielraffel/pulp/releases/tag/v0.324.0
[0.323.0]: https://github.com/danielraffel/pulp/releases/tag/v0.323.0
[0.322.0]: https://github.com/danielraffel/pulp/releases/tag/v0.322.0
[0.321.0]: https://github.com/danielraffel/pulp/releases/tag/v0.321.0
[0.320.0]: https://github.com/danielraffel/pulp/releases/tag/v0.320.0
[0.319.0]: https://github.com/danielraffel/pulp/releases/tag/v0.319.0
[0.318.0]: https://github.com/danielraffel/pulp/releases/tag/v0.318.0
[0.317.4]: https://github.com/danielraffel/pulp/releases/tag/v0.317.4
[0.317.3]: https://github.com/danielraffel/pulp/releases/tag/v0.317.3
[0.317.2]: https://github.com/danielraffel/pulp/releases/tag/v0.317.2
[0.317.1]: https://github.com/danielraffel/pulp/releases/tag/v0.317.1
[0.317.0]: https://github.com/danielraffel/pulp/releases/tag/v0.317.0
[0.316.0]: https://github.com/danielraffel/pulp/releases/tag/v0.316.0
[0.315.0]: https://github.com/danielraffel/pulp/releases/tag/v0.315.0
[0.314.0]: https://github.com/danielraffel/pulp/releases/tag/v0.314.0
[0.313.0]: https://github.com/danielraffel/pulp/releases/tag/v0.313.0
[0.312.1]: https://github.com/danielraffel/pulp/releases/tag/v0.312.1
[0.312.0]: https://github.com/danielraffel/pulp/releases/tag/v0.312.0
[0.311.0]: https://github.com/danielraffel/pulp/releases/tag/v0.311.0
[0.310.0]: https://github.com/danielraffel/pulp/releases/tag/v0.310.0
[0.309.0]: https://github.com/danielraffel/pulp/releases/tag/v0.309.0
[0.308.0]: https://github.com/danielraffel/pulp/releases/tag/v0.308.0
[0.307.0]: https://github.com/danielraffel/pulp/releases/tag/v0.307.0
[0.306.0]: https://github.com/danielraffel/pulp/releases/tag/v0.306.0
[0.305.0]: https://github.com/danielraffel/pulp/releases/tag/v0.305.0
[0.304.0]: https://github.com/danielraffel/pulp/releases/tag/v0.304.0
[0.303.0]: https://github.com/danielraffel/pulp/releases/tag/v0.303.0
[0.302.0]: https://github.com/danielraffel/pulp/releases/tag/v0.302.0
[0.301.0]: https://github.com/danielraffel/pulp/releases/tag/v0.301.0
[0.300.0]: https://github.com/danielraffel/pulp/releases/tag/v0.300.0
[0.299.0]: https://github.com/danielraffel/pulp/releases/tag/v0.299.0
[0.298.0]: https://github.com/danielraffel/pulp/releases/tag/v0.298.0
[0.297.0]: https://github.com/danielraffel/pulp/releases/tag/v0.297.0
[0.296.0]: https://github.com/danielraffel/pulp/releases/tag/v0.296.0
[0.295.1]: https://github.com/danielraffel/pulp/releases/tag/v0.295.1
[0.295.0]: https://github.com/danielraffel/pulp/releases/tag/v0.295.0
[0.294.0]: https://github.com/danielraffel/pulp/releases/tag/v0.294.0
[0.293.1]: https://github.com/danielraffel/pulp/releases/tag/v0.293.1
[0.293.0]: https://github.com/danielraffel/pulp/releases/tag/v0.293.0
[0.292.0]: https://github.com/danielraffel/pulp/releases/tag/v0.292.0
[0.291.0]: https://github.com/danielraffel/pulp/releases/tag/v0.291.0
[0.290.0]: https://github.com/danielraffel/pulp/releases/tag/v0.290.0
[0.289.0]: https://github.com/danielraffel/pulp/releases/tag/v0.289.0
[0.288.0]: https://github.com/danielraffel/pulp/releases/tag/v0.288.0
[0.287.0]: https://github.com/danielraffel/pulp/releases/tag/v0.287.0
[0.286.1]: https://github.com/danielraffel/pulp/releases/tag/v0.286.1
[0.286.0]: https://github.com/danielraffel/pulp/releases/tag/v0.286.0
[0.285.1]: https://github.com/danielraffel/pulp/releases/tag/v0.285.1
[0.285.0]: https://github.com/danielraffel/pulp/releases/tag/v0.285.0
[0.284.2]: https://github.com/danielraffel/pulp/releases/tag/v0.284.2
[0.284.1]: https://github.com/danielraffel/pulp/releases/tag/v0.284.1
[0.284.0]: https://github.com/danielraffel/pulp/releases/tag/v0.284.0
[0.283.1]: https://github.com/danielraffel/pulp/releases/tag/v0.283.1
[0.283.0]: https://github.com/danielraffel/pulp/releases/tag/v0.283.0
[0.282.2]: https://github.com/danielraffel/pulp/releases/tag/v0.282.2
[0.282.1]: https://github.com/danielraffel/pulp/releases/tag/v0.282.1
[0.282.0]: https://github.com/danielraffel/pulp/releases/tag/v0.282.0
[0.281.0]: https://github.com/danielraffel/pulp/releases/tag/v0.281.0
[0.280.0]: https://github.com/danielraffel/pulp/releases/tag/v0.280.0
[0.279.0]: https://github.com/danielraffel/pulp/releases/tag/v0.279.0
[0.278.0]: https://github.com/danielraffel/pulp/releases/tag/v0.278.0
[0.277.0]: https://github.com/danielraffel/pulp/releases/tag/v0.277.0
[0.276.0]: https://github.com/danielraffel/pulp/releases/tag/v0.276.0
[0.275.0]: https://github.com/danielraffel/pulp/releases/tag/v0.275.0
[0.274.0]: https://github.com/danielraffel/pulp/releases/tag/v0.274.0
[0.273.0]: https://github.com/danielraffel/pulp/releases/tag/v0.273.0
[0.272.1]: https://github.com/danielraffel/pulp/releases/tag/v0.272.1
[0.272.0]: https://github.com/danielraffel/pulp/releases/tag/v0.272.0
[0.271.0]: https://github.com/danielraffel/pulp/releases/tag/v0.271.0
[0.270.4]: https://github.com/danielraffel/pulp/releases/tag/v0.270.4
[0.270.2]: https://github.com/danielraffel/pulp/releases/tag/v0.270.2
[0.270.1]: https://github.com/danielraffel/pulp/releases/tag/v0.270.1
[0.270.0]: https://github.com/danielraffel/pulp/releases/tag/v0.270.0
[0.269.1]: https://github.com/danielraffel/pulp/releases/tag/v0.269.1
[0.268.1]: https://github.com/danielraffel/pulp/releases/tag/v0.268.1
[0.268.0]: https://github.com/danielraffel/pulp/releases/tag/v0.268.0
[0.266.0]: https://github.com/danielraffel/pulp/releases/tag/v0.266.0
[0.264.1]: https://github.com/danielraffel/pulp/releases/tag/v0.264.1
[0.264.0]: https://github.com/danielraffel/pulp/releases/tag/v0.264.0
[0.263.0]: https://github.com/danielraffel/pulp/releases/tag/v0.263.0
[0.262.0]: https://github.com/danielraffel/pulp/releases/tag/v0.262.0
[0.261.0]: https://github.com/danielraffel/pulp/releases/tag/v0.261.0
[0.260.0]: https://github.com/danielraffel/pulp/releases/tag/v0.260.0
[0.259.0]: https://github.com/danielraffel/pulp/releases/tag/v0.259.0
[0.258.0]: https://github.com/danielraffel/pulp/releases/tag/v0.258.0
[0.257.0]: https://github.com/danielraffel/pulp/releases/tag/v0.257.0
[0.256.0]: https://github.com/danielraffel/pulp/releases/tag/v0.256.0
[0.255.3]: https://github.com/danielraffel/pulp/releases/tag/v0.255.3
[0.255.2]: https://github.com/danielraffel/pulp/releases/tag/v0.255.2
[0.255.1]: https://github.com/danielraffel/pulp/releases/tag/v0.255.1
[0.255.0]: https://github.com/danielraffel/pulp/releases/tag/v0.255.0
[0.254.1]: https://github.com/danielraffel/pulp/releases/tag/v0.254.1
[0.254.0]: https://github.com/danielraffel/pulp/releases/tag/v0.254.0
[0.253.0]: https://github.com/danielraffel/pulp/releases/tag/v0.253.0
[0.252.0]: https://github.com/danielraffel/pulp/releases/tag/v0.252.0
[0.250.0]: https://github.com/danielraffel/pulp/releases/tag/v0.250.0
[0.249.0]: https://github.com/danielraffel/pulp/releases/tag/v0.249.0
[0.248.0]: https://github.com/danielraffel/pulp/releases/tag/v0.248.0
[0.247.0]: https://github.com/danielraffel/pulp/releases/tag/v0.247.0
[0.246.0]: https://github.com/danielraffel/pulp/releases/tag/v0.246.0
[0.245.0]: https://github.com/danielraffel/pulp/releases/tag/v0.245.0
[0.244.0]: https://github.com/danielraffel/pulp/releases/tag/v0.244.0
[0.243.0]: https://github.com/danielraffel/pulp/releases/tag/v0.243.0
[0.242.0]: https://github.com/danielraffel/pulp/releases/tag/v0.242.0
[0.241.0]: https://github.com/danielraffel/pulp/releases/tag/v0.241.0
[0.240.0]: https://github.com/danielraffel/pulp/releases/tag/v0.240.0
[0.239.0]: https://github.com/danielraffel/pulp/releases/tag/v0.239.0
[0.238.0]: https://github.com/danielraffel/pulp/releases/tag/v0.238.0
[0.237.0]: https://github.com/danielraffel/pulp/releases/tag/v0.237.0
[0.235.0]: https://github.com/danielraffel/pulp/releases/tag/v0.235.0
[0.234.0]: https://github.com/danielraffel/pulp/releases/tag/v0.234.0
[0.230.0]: https://github.com/danielraffel/pulp/releases/tag/v0.230.0
[0.228.0]: https://github.com/danielraffel/pulp/releases/tag/v0.228.0
[0.226.0]: https://github.com/danielraffel/pulp/releases/tag/v0.226.0
[0.225.0]: https://github.com/danielraffel/pulp/releases/tag/v0.225.0
[0.224.1]: https://github.com/danielraffel/pulp/releases/tag/v0.224.1
[0.224.0]: https://github.com/danielraffel/pulp/releases/tag/v0.224.0
[0.223.0]: https://github.com/danielraffel/pulp/releases/tag/v0.223.0
[0.222.1]: https://github.com/danielraffel/pulp/releases/tag/v0.222.1
[0.222.0]: https://github.com/danielraffel/pulp/releases/tag/v0.222.0
[0.221.0]: https://github.com/danielraffel/pulp/releases/tag/v0.221.0
[0.220.0]: https://github.com/danielraffel/pulp/releases/tag/v0.220.0
[0.219.0]: https://github.com/danielraffel/pulp/releases/tag/v0.219.0
[0.218.0]: https://github.com/danielraffel/pulp/releases/tag/v0.218.0
[0.217.0]: https://github.com/danielraffel/pulp/releases/tag/v0.217.0
[0.216.0]: https://github.com/danielraffel/pulp/releases/tag/v0.216.0
[0.215.0]: https://github.com/danielraffel/pulp/releases/tag/v0.215.0
[0.214.0]: https://github.com/danielraffel/pulp/releases/tag/v0.214.0
[0.213.0]: https://github.com/danielraffel/pulp/releases/tag/v0.213.0
[0.212.0]: https://github.com/danielraffel/pulp/releases/tag/v0.212.0
[0.211.0]: https://github.com/danielraffel/pulp/releases/tag/v0.211.0
[0.210.0]: https://github.com/danielraffel/pulp/releases/tag/v0.210.0
[0.209.0]: https://github.com/danielraffel/pulp/releases/tag/v0.209.0
[0.208.0]: https://github.com/danielraffel/pulp/releases/tag/v0.208.0
[0.207.0]: https://github.com/danielraffel/pulp/releases/tag/v0.207.0
[0.206.0]: https://github.com/danielraffel/pulp/releases/tag/v0.206.0
[0.205.0]: https://github.com/danielraffel/pulp/releases/tag/v0.205.0
[0.204.0]: https://github.com/danielraffel/pulp/releases/tag/v0.204.0
[0.203.0]: https://github.com/danielraffel/pulp/releases/tag/v0.203.0
[0.202.0]: https://github.com/danielraffel/pulp/releases/tag/v0.202.0
[0.201.0]: https://github.com/danielraffel/pulp/releases/tag/v0.201.0
[0.200.0]: https://github.com/danielraffel/pulp/releases/tag/v0.200.0
[0.199.0]: https://github.com/danielraffel/pulp/releases/tag/v0.199.0
[0.198.0]: https://github.com/danielraffel/pulp/releases/tag/v0.198.0
[0.197.4]: https://github.com/danielraffel/pulp/releases/tag/v0.197.4
[0.197.3]: https://github.com/danielraffel/pulp/releases/tag/v0.197.3
[0.197.1]: https://github.com/danielraffel/pulp/releases/tag/v0.197.1
[0.197.0]: https://github.com/danielraffel/pulp/releases/tag/v0.197.0
[0.196.0]: https://github.com/danielraffel/pulp/releases/tag/v0.196.0
[0.195.3]: https://github.com/danielraffel/pulp/releases/tag/v0.195.3
[0.195.2]: https://github.com/danielraffel/pulp/releases/tag/v0.195.2
[0.195.1]: https://github.com/danielraffel/pulp/releases/tag/v0.195.1
[0.194.1]: https://github.com/danielraffel/pulp/releases/tag/v0.194.1
[0.194.0]: https://github.com/danielraffel/pulp/releases/tag/v0.194.0
[0.193.1]: https://github.com/danielraffel/pulp/releases/tag/v0.193.1
[0.193.0]: https://github.com/danielraffel/pulp/releases/tag/v0.193.0
[0.192.2]: https://github.com/danielraffel/pulp/releases/tag/v0.192.2
[0.192.1]: https://github.com/danielraffel/pulp/releases/tag/v0.192.1
[0.192.0]: https://github.com/danielraffel/pulp/releases/tag/v0.192.0
[0.191.0]: https://github.com/danielraffel/pulp/releases/tag/v0.191.0
[0.190.0]: https://github.com/danielraffel/pulp/releases/tag/v0.190.0
[0.189.0]: https://github.com/danielraffel/pulp/releases/tag/v0.189.0
[0.188.0]: https://github.com/danielraffel/pulp/releases/tag/v0.188.0
[0.187.0]: https://github.com/danielraffel/pulp/releases/tag/v0.187.0
[0.186.0]: https://github.com/danielraffel/pulp/releases/tag/v0.186.0
[0.185.0]: https://github.com/danielraffel/pulp/releases/tag/v0.185.0
[0.184.1]: https://github.com/danielraffel/pulp/releases/tag/v0.184.1
[0.184.0]: https://github.com/danielraffel/pulp/releases/tag/v0.184.0
[0.183.6]: https://github.com/danielraffel/pulp/releases/tag/v0.183.6
[0.183.5]: https://github.com/danielraffel/pulp/releases/tag/v0.183.5
[0.183.4]: https://github.com/danielraffel/pulp/releases/tag/v0.183.4
[0.183.1]: https://github.com/danielraffel/pulp/releases/tag/v0.183.1
[0.183.0]: https://github.com/danielraffel/pulp/releases/tag/v0.183.0
[0.182.0]: https://github.com/danielraffel/pulp/releases/tag/v0.182.0
[0.181.0]: https://github.com/danielraffel/pulp/releases/tag/v0.181.0
[0.180.4]: https://github.com/danielraffel/pulp/releases/tag/v0.180.4
[0.180.0]: https://github.com/danielraffel/pulp/releases/tag/v0.180.0
[0.179.0]: https://github.com/danielraffel/pulp/releases/tag/v0.179.0
[0.178.2]: https://github.com/danielraffel/pulp/releases/tag/v0.178.2
[0.178.1]: https://github.com/danielraffel/pulp/releases/tag/v0.178.1
[0.178.0]: https://github.com/danielraffel/pulp/releases/tag/v0.178.0
[0.177.1]: https://github.com/danielraffel/pulp/releases/tag/v0.177.1
[0.177.0]: https://github.com/danielraffel/pulp/releases/tag/v0.177.0
[0.176.0]: https://github.com/danielraffel/pulp/releases/tag/v0.176.0
[0.175.0]: https://github.com/danielraffel/pulp/releases/tag/v0.175.0
[0.174.0]: https://github.com/danielraffel/pulp/releases/tag/v0.174.0
[0.173.13]: https://github.com/danielraffel/pulp/releases/tag/v0.173.13
[0.173.12]: https://github.com/danielraffel/pulp/releases/tag/v0.173.12
[0.173.11]: https://github.com/danielraffel/pulp/releases/tag/v0.173.11
[0.173.8]: https://github.com/danielraffel/pulp/releases/tag/v0.173.8
[0.173.7]: https://github.com/danielraffel/pulp/releases/tag/v0.173.7
[0.173.1]: https://github.com/danielraffel/pulp/releases/tag/v0.173.1
[0.173.0]: https://github.com/danielraffel/pulp/releases/tag/v0.173.0
[0.172.6]: https://github.com/danielraffel/pulp/releases/tag/v0.172.6
[0.172.2]: https://github.com/danielraffel/pulp/releases/tag/v0.172.2
[0.172.0]: https://github.com/danielraffel/pulp/releases/tag/v0.172.0
[0.171.0]: https://github.com/danielraffel/pulp/releases/tag/v0.171.0
[0.169.0]: https://github.com/danielraffel/pulp/releases/tag/v0.169.0
[0.167.0]: https://github.com/danielraffel/pulp/releases/tag/v0.167.0
[0.166.0]: https://github.com/danielraffel/pulp/releases/tag/v0.166.0
[0.161.0]: https://github.com/danielraffel/pulp/releases/tag/v0.161.0
[0.158.1]: https://github.com/danielraffel/pulp/releases/tag/v0.158.1
[0.158.0]: https://github.com/danielraffel/pulp/releases/tag/v0.158.0
[0.155.0]: https://github.com/danielraffel/pulp/releases/tag/v0.155.0
[0.154.0]: https://github.com/danielraffel/pulp/releases/tag/v0.154.0
[0.151.0]: https://github.com/danielraffel/pulp/releases/tag/v0.151.0
[0.150.0]: https://github.com/danielraffel/pulp/releases/tag/v0.150.0
[0.148.0]: https://github.com/danielraffel/pulp/releases/tag/v0.148.0
[0.147.0]: https://github.com/danielraffel/pulp/releases/tag/v0.147.0
[0.146.1]: https://github.com/danielraffel/pulp/releases/tag/v0.146.1
[0.146.0]: https://github.com/danielraffel/pulp/releases/tag/v0.146.0
[0.143.0]: https://github.com/danielraffel/pulp/releases/tag/v0.143.0
[0.142.0]: https://github.com/danielraffel/pulp/releases/tag/v0.142.0
[0.139.0]: https://github.com/danielraffel/pulp/releases/tag/v0.139.0
[0.137.2]: https://github.com/danielraffel/pulp/releases/tag/v0.137.2
[0.137.1]: https://github.com/danielraffel/pulp/releases/tag/v0.137.1
[0.137.0]: https://github.com/danielraffel/pulp/releases/tag/v0.137.0
[0.136.0]: https://github.com/danielraffel/pulp/releases/tag/v0.136.0
[0.135.2]: https://github.com/danielraffel/pulp/releases/tag/v0.135.2
[0.135.1]: https://github.com/danielraffel/pulp/releases/tag/v0.135.1
[0.135.0]: https://github.com/danielraffel/pulp/releases/tag/v0.135.0
[0.134.0]: https://github.com/danielraffel/pulp/releases/tag/v0.134.0
[0.133.0]: https://github.com/danielraffel/pulp/releases/tag/v0.133.0
[0.132.0]: https://github.com/danielraffel/pulp/releases/tag/v0.132.0
[0.128.0]: https://github.com/danielraffel/pulp/releases/tag/v0.128.0
[0.127.0]: https://github.com/danielraffel/pulp/releases/tag/v0.127.0
[0.126.0]: https://github.com/danielraffel/pulp/releases/tag/v0.126.0
[0.125.0]: https://github.com/danielraffel/pulp/releases/tag/v0.125.0
[0.121.0]: https://github.com/danielraffel/pulp/releases/tag/v0.121.0
[0.116.0]: https://github.com/danielraffel/pulp/releases/tag/v0.116.0
[0.115.0]: https://github.com/danielraffel/pulp/releases/tag/v0.115.0
[0.114.0]: https://github.com/danielraffel/pulp/releases/tag/v0.114.0
[0.113.9]: https://github.com/danielraffel/pulp/releases/tag/v0.113.9
[0.113.8]: https://github.com/danielraffel/pulp/releases/tag/v0.113.8
[0.113.5]: https://github.com/danielraffel/pulp/releases/tag/v0.113.5
[0.113.3]: https://github.com/danielraffel/pulp/releases/tag/v0.113.3
[0.113.2]: https://github.com/danielraffel/pulp/releases/tag/v0.113.2
[0.113.1]: https://github.com/danielraffel/pulp/releases/tag/v0.113.1
[0.113.0]: https://github.com/danielraffel/pulp/releases/tag/v0.113.0
[0.112.0]: https://github.com/danielraffel/pulp/releases/tag/v0.112.0
[0.111.0]: https://github.com/danielraffel/pulp/releases/tag/v0.111.0
[0.110.0]: https://github.com/danielraffel/pulp/releases/tag/v0.110.0
[0.109.0]: https://github.com/danielraffel/pulp/releases/tag/v0.109.0
[0.108.0]: https://github.com/danielraffel/pulp/releases/tag/v0.108.0
[0.107.0]: https://github.com/danielraffel/pulp/releases/tag/v0.107.0
[0.106.0]: https://github.com/danielraffel/pulp/releases/tag/v0.106.0
[0.105.0]: https://github.com/danielraffel/pulp/releases/tag/v0.105.0
[0.104.0]: https://github.com/danielraffel/pulp/releases/tag/v0.104.0
[0.103.0]: https://github.com/danielraffel/pulp/releases/tag/v0.103.0
[0.102.1]: https://github.com/danielraffel/pulp/releases/tag/v0.102.1
[0.102.0]: https://github.com/danielraffel/pulp/releases/tag/v0.102.0
[0.101.8]: https://github.com/danielraffel/pulp/releases/tag/v0.101.8
[0.101.7]: https://github.com/danielraffel/pulp/releases/tag/v0.101.7
[0.101.5]: https://github.com/danielraffel/pulp/releases/tag/v0.101.5
[0.101.4]: https://github.com/danielraffel/pulp/releases/tag/v0.101.4
[0.101.3]: https://github.com/danielraffel/pulp/releases/tag/v0.101.3
[0.101.2]: https://github.com/danielraffel/pulp/releases/tag/v0.101.2
[0.101.1]: https://github.com/danielraffel/pulp/releases/tag/v0.101.1
[0.101.0]: https://github.com/danielraffel/pulp/releases/tag/v0.101.0
[0.100.0]: https://github.com/danielraffel/pulp/releases/tag/v0.100.0
[0.99.0]: https://github.com/danielraffel/pulp/releases/tag/v0.99.0
[0.94.0]: https://github.com/danielraffel/pulp/releases/tag/v0.94.0
[0.93.0]: https://github.com/danielraffel/pulp/releases/tag/v0.93.0
[0.92.0]: https://github.com/danielraffel/pulp/releases/tag/v0.92.0
[0.91.0]: https://github.com/danielraffel/pulp/releases/tag/v0.91.0
[0.90.0]: https://github.com/danielraffel/pulp/releases/tag/v0.90.0
[0.89.0]: https://github.com/danielraffel/pulp/releases/tag/v0.89.0
[0.88.0]: https://github.com/danielraffel/pulp/releases/tag/v0.88.0
[0.87.3]: https://github.com/danielraffel/pulp/releases/tag/v0.87.3
[0.87.2]: https://github.com/danielraffel/pulp/releases/tag/v0.87.2
[0.87.1]: https://github.com/danielraffel/pulp/releases/tag/v0.87.1
[0.87.0]: https://github.com/danielraffel/pulp/releases/tag/v0.87.0
[0.86.0]: https://github.com/danielraffel/pulp/releases/tag/v0.86.0
[0.85.0]: https://github.com/danielraffel/pulp/releases/tag/v0.85.0
[0.84.0]: https://github.com/danielraffel/pulp/releases/tag/v0.84.0
[0.83.1]: https://github.com/danielraffel/pulp/releases/tag/v0.83.1
[0.83.0]: https://github.com/danielraffel/pulp/releases/tag/v0.83.0
[0.82.2]: https://github.com/danielraffel/pulp/releases/tag/v0.82.2
[0.82.1]: https://github.com/danielraffel/pulp/releases/tag/v0.82.1
[0.82.0]: https://github.com/danielraffel/pulp/releases/tag/v0.82.0
[0.81.0]: https://github.com/danielraffel/pulp/releases/tag/v0.81.0
[0.80.0]: https://github.com/danielraffel/pulp/releases/tag/v0.80.0
[0.79.25]: https://github.com/danielraffel/pulp/releases/tag/v0.79.25
[0.79.24]: https://github.com/danielraffel/pulp/releases/tag/v0.79.24
[0.79.23]: https://github.com/danielraffel/pulp/releases/tag/v0.79.23
[0.79.22]: https://github.com/danielraffel/pulp/releases/tag/v0.79.22
[0.79.21]: https://github.com/danielraffel/pulp/releases/tag/v0.79.21
[0.79.20]: https://github.com/danielraffel/pulp/releases/tag/v0.79.20
[0.79.19]: https://github.com/danielraffel/pulp/releases/tag/v0.79.19
[0.79.18]: https://github.com/danielraffel/pulp/releases/tag/v0.79.18
[0.79.17]: https://github.com/danielraffel/pulp/releases/tag/v0.79.17
[0.79.16]: https://github.com/danielraffel/pulp/releases/tag/v0.79.16
[0.79.13]: https://github.com/danielraffel/pulp/releases/tag/v0.79.13
[0.79.12]: https://github.com/danielraffel/pulp/releases/tag/v0.79.12
[0.79.11]: https://github.com/danielraffel/pulp/releases/tag/v0.79.11
[0.79.10]: https://github.com/danielraffel/pulp/releases/tag/v0.79.10
[0.79.9]: https://github.com/danielraffel/pulp/releases/tag/v0.79.9
[0.79.8]: https://github.com/danielraffel/pulp/releases/tag/v0.79.8
[0.79.6]: https://github.com/danielraffel/pulp/releases/tag/v0.79.6
[0.79.5]: https://github.com/danielraffel/pulp/releases/tag/v0.79.5
[0.79.4]: https://github.com/danielraffel/pulp/releases/tag/v0.79.4
[0.79.0]: https://github.com/danielraffel/pulp/releases/tag/v0.79.0
[0.78.4]: https://github.com/danielraffel/pulp/releases/tag/v0.78.4
[0.78.3]: https://github.com/danielraffel/pulp/releases/tag/v0.78.3
[0.78.2]: https://github.com/danielraffel/pulp/releases/tag/v0.78.2
[0.78.1]: https://github.com/danielraffel/pulp/releases/tag/v0.78.1
[0.78.0]: https://github.com/danielraffel/pulp/releases/tag/v0.78.0
[0.77.0]: https://github.com/danielraffel/pulp/releases/tag/v0.77.0
[0.76.0]: https://github.com/danielraffel/pulp/releases/tag/v0.76.0
[0.75.6]: https://github.com/danielraffel/pulp/releases/tag/v0.75.6
[0.75.5]: https://github.com/danielraffel/pulp/releases/tag/v0.75.5
[0.75.4]: https://github.com/danielraffel/pulp/releases/tag/v0.75.4
[0.75.2]: https://github.com/danielraffel/pulp/releases/tag/v0.75.2
[0.75.1]: https://github.com/danielraffel/pulp/releases/tag/v0.75.1
[0.75.0]: https://github.com/danielraffel/pulp/releases/tag/v0.75.0
[0.74.1]: https://github.com/danielraffel/pulp/releases/tag/v0.74.1
[0.74.0]: https://github.com/danielraffel/pulp/releases/tag/v0.74.0
[0.73.0]: https://github.com/danielraffel/pulp/releases/tag/v0.73.0
[0.72.6]: https://github.com/danielraffel/pulp/releases/tag/v0.72.6
[0.72.5]: https://github.com/danielraffel/pulp/releases/tag/v0.72.5
[0.72.4]: https://github.com/danielraffel/pulp/releases/tag/v0.72.4
[0.72.3]: https://github.com/danielraffel/pulp/releases/tag/v0.72.3
[0.72.2]: https://github.com/danielraffel/pulp/releases/tag/v0.72.2
[0.72.1]: https://github.com/danielraffel/pulp/releases/tag/v0.72.1
[0.72.0]: https://github.com/danielraffel/pulp/releases/tag/v0.72.0
[0.71.0]: https://github.com/danielraffel/pulp/releases/tag/v0.71.0
[0.70.0]: https://github.com/danielraffel/pulp/releases/tag/v0.70.0
[0.69.2]: https://github.com/danielraffel/pulp/releases/tag/v0.69.2
[0.69.1]: https://github.com/danielraffel/pulp/releases/tag/v0.69.1
[0.69.0]: https://github.com/danielraffel/pulp/releases/tag/v0.69.0
[0.68.1]: https://github.com/danielraffel/pulp/releases/tag/v0.68.1
[0.68.0]: https://github.com/danielraffel/pulp/releases/tag/v0.68.0
[0.67.0]: https://github.com/danielraffel/pulp/releases/tag/v0.67.0
[0.66.0]: https://github.com/danielraffel/pulp/releases/tag/v0.66.0
[0.65.0]: https://github.com/danielraffel/pulp/releases/tag/v0.65.0
[0.64.0]: https://github.com/danielraffel/pulp/releases/tag/v0.64.0
[0.63.0]: https://github.com/danielraffel/pulp/releases/tag/v0.63.0
[0.62.0]: https://github.com/danielraffel/pulp/releases/tag/v0.62.0
[0.61.0]: https://github.com/danielraffel/pulp/releases/tag/v0.61.0
[0.60.0]: https://github.com/danielraffel/pulp/releases/tag/v0.60.0
[0.59.0]: https://github.com/danielraffel/pulp/releases/tag/v0.59.0
[0.58.0]: https://github.com/danielraffel/pulp/releases/tag/v0.58.0
[0.57.0]: https://github.com/danielraffel/pulp/releases/tag/v0.57.0
[0.56.0]: https://github.com/danielraffel/pulp/releases/tag/v0.56.0
[0.55.0]: https://github.com/danielraffel/pulp/releases/tag/v0.55.0
[0.54.0]: https://github.com/danielraffel/pulp/releases/tag/v0.54.0
[0.53.0]: https://github.com/danielraffel/pulp/releases/tag/v0.53.0
[0.52.0]: https://github.com/danielraffel/pulp/releases/tag/v0.52.0
[0.51.0]: https://github.com/danielraffel/pulp/releases/tag/v0.51.0
[0.50.0]: https://github.com/danielraffel/pulp/releases/tag/v0.50.0
[0.49.0]: https://github.com/danielraffel/pulp/releases/tag/v0.49.0
[0.48.0]: https://github.com/danielraffel/pulp/releases/tag/v0.48.0
[0.47.1]: https://github.com/danielraffel/pulp/releases/tag/v0.47.1
[0.47.0]: https://github.com/danielraffel/pulp/releases/tag/v0.47.0
[0.46.0]: https://github.com/danielraffel/pulp/releases/tag/v0.46.0
[0.45.1]: https://github.com/danielraffel/pulp/releases/tag/v0.45.1
[0.45.0]: https://github.com/danielraffel/pulp/releases/tag/v0.45.0
[0.43.0]: https://github.com/danielraffel/pulp/releases/tag/v0.43.0
[0.42.1]: https://github.com/danielraffel/pulp/releases/tag/v0.42.1
[0.42.0]: https://github.com/danielraffel/pulp/releases/tag/v0.42.0
[0.41.1]: https://github.com/danielraffel/pulp/releases/tag/v0.41.1
[0.41.0]: https://github.com/danielraffel/pulp/releases/tag/v0.41.0
[0.40.2]: https://github.com/danielraffel/pulp/releases/tag/v0.40.2
[0.40.1]: https://github.com/danielraffel/pulp/releases/tag/v0.40.1
[0.40.0]: https://github.com/danielraffel/pulp/releases/tag/v0.40.0
[0.39.0]: https://github.com/danielraffel/pulp/releases/tag/v0.39.0
[0.38.0]: https://github.com/danielraffel/pulp/releases/tag/v0.38.0
[0.37.0]: https://github.com/danielraffel/pulp/releases/tag/v0.37.0
[0.36.0]: https://github.com/danielraffel/pulp/releases/tag/v0.36.0
[0.35.0]: https://github.com/danielraffel/pulp/releases/tag/v0.35.0
[0.34.0]: https://github.com/danielraffel/pulp/releases/tag/v0.34.0
[0.33.0]: https://github.com/danielraffel/pulp/releases/tag/v0.33.0
[0.32.0]: https://github.com/danielraffel/pulp/releases/tag/v0.32.0
[0.31.1]: https://github.com/danielraffel/pulp/releases/tag/v0.31.1
[0.31.0]: https://github.com/danielraffel/pulp/releases/tag/v0.31.0
[0.30.0]: https://github.com/danielraffel/pulp/releases/tag/v0.30.0
[0.29.1]: https://github.com/danielraffel/pulp/releases/tag/v0.29.1
[0.29.0]: https://github.com/danielraffel/pulp/releases/tag/v0.29.0
[0.28.0]: https://github.com/danielraffel/pulp/releases/tag/v0.28.0
[0.27.0]: https://github.com/danielraffel/pulp/releases/tag/v0.27.0
[0.26.0]: https://github.com/danielraffel/pulp/releases/tag/v0.26.0
[0.25.1]: https://github.com/danielraffel/pulp/releases/tag/v0.25.1
[0.25.0]: https://github.com/danielraffel/pulp/releases/tag/v0.25.0
[0.24.0]: https://github.com/danielraffel/pulp/releases/tag/v0.24.0
[0.23.1]: https://github.com/danielraffel/pulp/releases/tag/v0.23.1
[0.23.0]: https://github.com/danielraffel/pulp/releases/tag/v0.23.0
[0.22.0]: https://github.com/danielraffel/pulp/releases/tag/v0.22.0
[0.21.0]: https://github.com/danielraffel/pulp/releases/tag/v0.21.0
[0.20.0]: https://github.com/danielraffel/pulp/releases/tag/v0.20.0
[0.19.0]: https://github.com/danielraffel/pulp/releases/tag/v0.19.0
[0.18.0]: https://github.com/danielraffel/pulp/releases/tag/v0.18.0
[0.17.0]: https://github.com/danielraffel/pulp/releases/tag/v0.17.0
[0.16.0]: https://github.com/danielraffel/pulp/releases/tag/v0.16.0
[0.15.0]: https://github.com/danielraffel/pulp/releases/tag/v0.15.0
[0.14.0]: https://github.com/danielraffel/pulp/releases/tag/v0.14.0
[0.13.1]: https://github.com/danielraffel/pulp/releases/tag/v0.13.1
[0.13.0]: https://github.com/danielraffel/pulp/releases/tag/v0.13.0
[0.12.0]: https://github.com/danielraffel/pulp/releases/tag/v0.12.0
[0.11.0]: https://github.com/danielraffel/pulp/releases/tag/v0.11.0
[0.10.1]: https://github.com/danielraffel/pulp/releases/tag/v0.10.1
[0.10.0]: https://github.com/danielraffel/pulp/releases/tag/v0.10.0
[0.9.0]: https://github.com/danielraffel/pulp/releases/tag/v0.9.0
[0.8.0]: https://github.com/danielraffel/pulp/releases/tag/v0.8.0
[0.7.0]: https://github.com/danielraffel/pulp/releases/tag/v0.7.0
[0.6.0]: https://github.com/danielraffel/pulp/releases/tag/v0.6.0
[0.5.0]: https://github.com/danielraffel/pulp/releases/tag/v0.5.0
[0.4.0]: https://github.com/danielraffel/pulp/releases/tag/v0.4.0
[0.3.0]: https://github.com/danielraffel/pulp/releases/tag/v0.3.0
[0.2.2]: https://github.com/danielraffel/pulp/releases/tag/v0.2.2
[0.2.1]: https://github.com/danielraffel/pulp/releases/tag/v0.2.1
[0.2.0]: https://github.com/danielraffel/pulp/releases/tag/v0.2.0
