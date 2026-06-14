# Desktop Video Proofs

Desktop automation can optionally record a short MP4 proof for macOS local
targets. Video is for UX interactions where a still screenshot or before/after
diff does not explain enough.

## Requirements

- The desktop target should enable optional `video_capture`.
- The runner process must have macOS Screen Recording permission.
- The runner needs ffmpeg, either from `PULP_FFMPEG`, `PATH`, or the
  repo-local developer install.
- `pulp ci-local desktop doctor mac` reports Screen Recording, ffmpeg, and
  AVFoundation screen-input state.

## Install Model

The feature-branch setup below still supports direct source-tree commands while
the workflow is being reviewed. The intended user-facing install shape is now an
optional Pulp tool add-on:

```bash
pulp tool install video-proof
pulp tool info video-proof --json
pulp tool doctor video-proof --run
pulp ci-local desktop video-setup mac --check
```

The video-proof tool is not a normal `pulp add` package. `pulp add` is for
project dependencies that update a project's package lock file, generated CMake
wiring, dependency metadata, and notices. Validation video proof capture is
machine-level developer tooling: it needs host permissions, ffmpeg, Node/npm,
Remotion composition, Terminal.app permission handoff on macOS, and optional
host/simulator integrations. Those dependencies should stay outside user
projects and outside the core runtime.

The long-term split should stay narrow:

- Core Pulp/local-CI keeps the command surface, run manifest schema, report
  schema, attachment budget logic, and clear "video-proof tooling is not
  installed" remediation text.
- The optional `video-proof` tool owns ffmpeg/Remotion bootstrap, composition
  templates, renderer scripts, and any large or license-sensitive developer
  dependencies.
- On the feature branch, `pulp tool install video-proof` installs the
  repo-local `tools/local-ci` npm package and writes a managed wrapper under
  `~/.pulp/tools/npm-packages/video-proof/`. That wrapper defaults to the
  Remotion smoke proof when run without arguments.
- `pulp tool info video-proof --json` exposes this as a machine-scoped
  `tool_addon` with `package_format: not_pulp_add` and, on this feature branch,
  `artifact_status: source_tree_iteration`. The same payload includes
  `artifact_pack_command`, `artifact_pack_npm_script`,
  `artifact_verify_command`, and `artifact_manifest_schema` so setup automation
  can discover the artifact review step without scraping this document.
- The source-tree developer path can still use `npm --prefix tools/local-ci
  install` for direct iteration.
- Before mainline release, produce a reviewable source add-on artifact with
  `python3 tools/local-ci/pack_video_proof_tool.py --json`, then verify the
  generated manifest with `--verify`. The artifact is a small zip plus manifest
  that contains only Pulp-owned package source, `package-lock.json`, Remotion
  template source, and wrapper scripts; it explicitly excludes `node_modules`,
  Remotion caches, and generated videos.
- Reusable demo scenarios or source material may later be distributed as
  reviewed Pulp kits/content packs, but the recorder/composer itself should
  remain a tool add-on.

This keeps normal Pulp installs small, avoids installing Remotion or ffmpeg
unless the user asks for video evidence, and gives Remotion the same
developer-supplied dependency treatment as other optional SDK/tool integrations.

Print the portable first-run checklist for another Mac, such as blackbook:

```bash
python3 tools/local-ci/local_ci.py desktop video-setup mac --machine blackbook
```

Use `--json` for a handoff artifact and `--check --run-in-terminal` to include
the current `video-doctor` readiness payload via Terminal.app's Screen Recording
grant:

```bash
python3 tools/local-ci/local_ci.py desktop video-setup mac --machine blackbook --check --run-in-terminal --json
```

On a fresh checkout, add `--init-config` to create the ignored local config from
`tools/local-ci/config.example.json` before setup checks run:

```bash
python3 tools/local-ci/local_ci.py desktop video-setup mac \
  --machine blackbook \
  --init-config \
  --enable-video-capture \
  --check \
  --json
```

Add `--check-tool-addon` when the setup run should also prove the eventual
managed `pulp tool install video-proof` path. If `pulp` on `PATH` is not the
fresh branch CLI, pass `--pulp-command` or set `PULP_CLI`:

```bash
python3 tools/local-ci/local_ci.py desktop video-setup mac \
  --machine blackbook \
  --init-config \
  --enable-video-capture \
  --check \
  --check-tool-addon \
  --pulp-command ./build-video-proof-cli/tools/cli/pulp-cpp \
  --run-in-terminal \
  --json
```

With `--check`, the setup report includes a separate
`setup_prerequisites` block for machine-level setup tools (`pulp`, `npm`,
`node`, and `cmake`) before the `video-doctor` capture readiness payload. This
lets a fresh Mac fail with actionable install remediations even if the later
screen-capture checks are not yet meaningful. `--enable-video-capture` is the
explicit local-config opt-in for recording on that target. With
`--check-tool-addon`, the report also includes a `tool_addon` block for
`pulp tool info video-proof --json` and `pulp tool doctor video-proof --run`.

To inspect another Mac without changing its checkout, add `--probe-host` with
an SSH host alias. The command runs read-only `command -v` probes and emits a
`remote_setup_prerequisites` block. Remote probes run through `zsh -lc` with
`$HOME/.local/bin`, `/opt/homebrew/bin`, and `/usr/local/bin` prepended, so
Homebrew or local tool installs are visible even when raw non-interactive SSH
would not find `node` or `npm`:

```bash
python3 tools/local-ci/local_ci.py desktop video-setup mac \
  --machine blackbook \
  --check \
  --probe-host blackbook \
  --skip-remotion-smoke \
  --json
```

When the setup report should also prove that a specific design-parity
comparison is runnable, pass the same concrete inputs used by `video-matrix`:

```bash
python3 tools/local-ci/local_ci.py desktop video-setup mac \
  --machine blackbook \
  --check \
  --design-parity-manifest /path/to/run/manifest.json \
  --design-parity-source-image planning/screenshots/reference.png \
  --json
```

If you prefer to create the machine-local config manually:

```bash
cp tools/local-ci/config.example.json tools/local-ci/config.json
```

Install and validate the optional video-proof tool on a new Mac:

```bash
pulp tool install video-proof
pulp tool info video-proof --json
pulp tool doctor video-proof --run
```

For direct feature-branch iteration, the equivalent source-tree install is
`npm --prefix tools/local-ci install`, and the source-tree smoke check is
`npm --prefix tools/local-ci run smoke-video-proof`.

Pack the current source-tree tool payload for review before changing the
registry from source-tree iteration to a versioned add-on artifact:

```bash
SOURCE_DATE_EPOCH=0 python3 tools/local-ci/pack_video_proof_tool.py \
  --output-dir /tmp/pulp-video-proof-tool-pack \
  --version 0.0.0-local \
  --json

python3 tools/local-ci/pack_video_proof_tool.py \
  --verify /tmp/pulp-video-proof-tool-pack/pulp-video-proof-tool-0.0.0-local.manifest.json \
  --json

pulp tool install video-proof \
  --artifact-manifest /tmp/pulp-video-proof-tool-pack/pulp-video-proof-tool-0.0.0-local.manifest.json \
  --force
```

The generated manifest records the zip path, `sha256`, size, included files,
Remotion/ffmpeg-static npm pins, and the policy flags that keep this machine
tool separate from core Pulp and `pulp add`. The verifier checks the manifest
schema, install policy, archive size and `sha256`, zip entry list, required
payload files, excluded generated/dependency paths, and required npm scripts and
dependency pins.

The `--artifact-manifest` install path is an explicit review lane for verified
local artifacts. The default `pulp tool install video-proof` path remains the
source-tree npm install on this feature branch until humans approve switching
the registry to a versioned add-on source.

Validate the eventual optional tool install path from a fresh source checkout:

```bash
cmake -S . -B build-video-proof-cli \
  -DCMAKE_BUILD_TYPE=Release \
  -DPULP_BUILD_TESTS=ON \
  -DPULP_BUILD_EXAMPLES=OFF
cmake --build build-video-proof-cli --target pulp-cli pulp-test-cli-tool-registry -j$(sysctl -n hw.ncpu)
./build-video-proof-cli/test/pulp-test-cli-tool-registry '~[slow]'

tmp_home=$(mktemp -d /tmp/pulp-video-proof-tool-home.XXXXXX)
PULP_HOME="$tmp_home" ./build-video-proof-cli/tools/cli/pulp-cpp tool install video-proof --force
PULP_HOME="$tmp_home" ./build-video-proof-cli/tools/cli/pulp-cpp tool doctor video-proof
PULP_HOME="$tmp_home" ./build-video-proof-cli/tools/cli/pulp-cpp tool doctor video-proof --run
rm -rf "$tmp_home"
```

`PULP_BUILD_EXAMPLES=OFF` keeps this scoped to the CLI/tool-registry path on
fresh machines that do not have binary Skia installed yet. Do not set
`PULP_ENABLE_GPU=OFF` for this smoke: the C++ CLI target is only generated in the
desktop GPU build graph.

Verify the Remotion/ffmpeg composition stack without Screen Recording
permission:

```bash
npm --prefix tools/local-ci run smoke-video-proof
```

Enable video capture for the mac target:

```bash
python3 tools/local-ci/local_ci.py desktop config set target.mac.video_capture true
```

Prepare the mac desktop target receipt:

```bash
python3 tools/local-ci/local_ci.py desktop install mac
```

Run the video-specific readiness gate:

```bash
python3 tools/local-ci/local_ci.py desktop video-doctor mac
```

For a faster config/tooling check that skips the Remotion smoke render:

```bash
python3 tools/local-ci/local_ci.py desktop video-doctor mac --skip-remotion-smoke
```

For a host/plugin recipe, include the recipe details so setup problems are
reported before recording starts:

```bash
python3 tools/local-ci/local_ci.py desktop video-doctor mac \
  --recipe reaper-plugin-editor \
  --plugin PulpSynth \
  --plugin-format clap
```

`video-doctor` should show `PASS backend.recorder`, `PASS screencapture`,
`PASS video_capture`, `PASS target.video_capture`, and `PASS remotion_smoke`
before `--record-video` can produce a composed clip. `PASS avfoundation_screen`
is preferred because it uses the primary ffmpeg/AVFoundation path; if it fails
while `screencapture` passes, the recorder can still use the screencapture
fallback. If
`screencapture` reports
`could not create image from display`, grant Screen Recording permission to the
terminal/agent app that runs local CI, then restart that app.

When Terminal.app has Screen Recording permission but the current agent process
does not, run the readiness check through Terminal:

```bash
python3 tools/local-ci/local_ci.py desktop video-doctor mac --run-in-terminal
```

`--run-in-terminal` re-invokes the same local-ci command inside Terminal.app,
removes the flag for the child process, and relays the child stdout/stderr and
exit code. It also sends a short `caffeinate -u` display-wake pulse before the
child command so idle/remote displays do not produce black captures.

For audio-bearing proofs, validate the explicit AVFoundation audio device too:

```bash
PULP_VIDEO_AUDIO_DEVICE="BlackHole 2ch" \
  python3 tools/local-ci/local_ci.py desktop video-doctor mac \
    --run-in-terminal \
    --video-audio system
```

The audio check should show `PASS avfoundation_audio`. The harness does not
guess an input device; use a loopback device when recording app/system output.

## Capture

The dedicated entry point records video by default and renders the Remotion
composition unless `--compose-video-proof` is already set explicitly:

```bash
python3 tools/local-ci/local_ci.py desktop video mac \
  --run-in-terminal \
  --command ./build/pulp \
  --click 120,80 \
  --video-note "Clicked the bypass control and verified the meter stayed visible." \
  --duration 8 \
  --video-fps 30 \
  --video-audio none \
  --video-attachment-budget-mb 100
```

Use `--action smoke`, `--action click`, or `--action inspect` to choose the
underlying desktop action. The command currently records video on macOS only;
Linux and Windows fail explicitly rather than producing a run bundle without a
video artifact. Use `--run-in-terminal` on macOS when Terminal is the process
with Screen Recording permission.

Use repeatable `--video-note` flags to add short reviewer-facing proof points to
the Remotion composition. Notes are most useful for host/plugin workflows where
the video shows the host window while logs or setup prove what was inserted,
loaded, or compared.

For console-style standalone apps that do not create their own GUI window, ask
the harness to record a titled Terminal.app window instead of waiting for the
child process to expose a window:

```bash
python3 tools/local-ci/local_ci.py desktop video mac \
  --run-in-terminal \
  --action smoke \
  --video-capture-target terminal \
  --command ./build/examples/pulp-tone/PulpTone.app/Contents/MacOS/PulpTone \
  --duration 5
```

Terminal capture is for smoke proofs only. It tees the command output into the
normal run logs, keeps the Terminal window open for the capture duration, then
signals the child process and records the command return code in the manifest.

Named recipes apply reviewer-friendly defaults for common proof scenarios:

```bash
python3 tools/local-ci/local_ci.py desktop video mac \
  --recipe audio-inspector-demo \
  --command ./build-video-nogpu/examples/audio-inspector-demo/pulp-audio-inspector-demo \
  --duration 4 \
  --video-fps 4
```

```bash
python3 tools/local-ci/local_ci.py desktop video mac \
  --recipe standalone-interaction \
  --source-mode exact-sha \
  --command ./build-desktop-automation/examples/ui-preview/pulp-ui-preview \
  --prepare-command 'cmake -S . -B build-desktop-automation -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_TESTS=OFF && cmake --build build-desktop-automation --target pulp-ui-preview -j$(sysctl -n hw.ncpu)' \
  --pulp-app-automation \
  --click 120,80 \
  --duration 8
```

```bash
python3 tools/local-ci/local_ci.py desktop video mac \
  --recipe reaper-plugin-editor \
  --plugin PulpEffect \
  --plugin-format vst3 \
  --duration 10
```

The REAPER recipe generates a temporary wrapper command and ReaScript when no
explicit `--command` is supplied. The wrapper launches a fresh REAPER instance,
adds a track, tries common REAPER plugin-name prefixes for the requested format,
opens the matching plugin editor, and records the REAPER window via
`--capture-bundle-id com.cockos.reaper`. The default action is a smoke proof;
use `--click X,Y` for coordinate clicks in the host window. ViewInspector
selectors such as `--click-view-id` are rejected because REAPER is not a Pulp
ViewInspector target. Pass an explicit `--command` to keep a
prepared-session workflow; in that mode, add `--capture-bundle-id
com.cockos.reaper` yourself if the command is only a wrapper process.
Generated REAPER proofs wait for the `TrackFX_Show floating-editor mode=3`
status marker, then refine capture from the main REAPER window to a visible
secondary window that looks like the floating plugin editor. The run manifest
records this as `capture_window_refinement` so reviewers can see the original
host window and the final captured editor window.

For generated CLAP recipes, build and install the plugin bundle before
recording. The recipe checks that
`~/Library/Audio/Plug-Ins/CLAP/<Plugin>.clap/Contents/MacOS/<Plugin>` exists and
fails early if the bundle is only a partial package:

```bash
cmake --build build-video-nogpu --target PulpSynth_CLAP -j$(sysctl -n hw.ncpu)
mkdir -p "$HOME/Library/Audio/Plug-Ins/CLAP"
ln -sfn "$(pwd)/build-video-nogpu/CLAP/PulpSynth.clap" \
  "$HOME/Library/Audio/Plug-Ins/CLAP/PulpSynth.clap"
```

If REAPER previously scanned a partial CLAP bundle, its cache can contain a
`[<Plugin>.clap]` stanza without a plugin descriptor such as
`com.pulp.synth=1|PulpSynth (Pulp)`. Generated CLAP recipes fail early in this
state because REAPER will not find the plugin during the proof. Open REAPER's
Preferences > Plug-ins > CLAP and rescan, or remove the stale stanza from
`~/Library/Application Support/REAPER/reaper-clap-macos-aarch64.ini` and relaunch
REAPER.

The same checks are available without recording:

```bash
python3 tools/local-ci/local_ci.py desktop video-doctor mac \
  --recipe reaper-plugin-editor \
  --plugin PulpSynth \
  --plugin-format clap
```

```bash
python3 tools/local-ci/local_ci.py desktop video mac \
  --recipe inspector-workflow \
  --source-mode exact-sha \
  --command ./build-video-nogpu/examples/audio-inspector-demo/pulp-audio-inspector-demo \
  --prepare-command 'cmake -S . -B build-video-nogpu -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_TESTS=OFF -DPULP_ENABLE_GPU=OFF && cmake --build build-video-nogpu --target pulp-audio-inspector-demo -j$(sysctl -n hw.ncpu)' \
  --duration 8
```

```bash
python3 tools/local-ci/local_ci.py desktop video mac \
  --recipe component-zoom \
  --source-mode exact-sha \
  --command ./build-desktop-automation/examples/ui-preview/pulp-ui-preview \
  --prepare-command 'cmake -S . -B build-desktop-automation -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_TESTS=OFF && cmake --build build-desktop-automation --target pulp-ui-preview -j$(sysctl -n hw.ncpu)' \
  --pulp-app-automation \
  --component-id bypass-toggle \
  --duration 8
```

```bash
python3 tools/local-ci/local_ci.py desktop video mac \
  --recipe design-parity \
  --command ./build/pulp \
  --source-image planning/screenshots/reference.png \
  --source-label "Figma reference" \
  --duration 8
```

`component-zoom` enables ViewInspector capture and before/diff capture, then
uses `--component-id` as the click target when no explicit click selector was
provided. The `ui-preview` matrix demo uses the real `bypass-toggle` selector.
It also selects the Remotion `component-zoom` template and records
`video_proof_composition.focus` metadata so the composed proof can show a
component label, focus rectangle, and zoom detail inset. Recipes also write
`video_proof_composition.context`, which the composed video, `index.html`, and
`review.md` display as setup context such as recipe, host, plugin, format,
component, bundle id, or launch mode. `reaper-plugin-editor` selects the
`plugin-host` template, inspector recipes select `inspector-workflow`, and
standalone interaction proofs select `standalone`. `audio-inspector-demo`
records a smoke proof of a built standalone
Audio Inspector demo window without requiring a UI snapshot artifact.
`design-parity` records an inspect proof and composes it with the Remotion
`design-parity` template directly during capture.

For audio-bearing Audio Inspector proofs, build the paired HeadlessHost renderer
and use its WAV as the plugin-origin audio source. The renderer exercises the
same `AudioInspectorDemoProcessor` factory as the standalone window, writes a
deterministic WAV, and records a small metadata JSON next to it:

```bash
cmake --build build-video-nogpu \
  --target pulp-audio-inspector-demo pulp-audio-inspector-demo-render \
  -j$(sysctl -n hw.ncpu)

./build-video-nogpu/examples/audio-inspector-demo/pulp-audio-inspector-demo-render \
  --output /tmp/pulp-audio-inspector-headless-proof.wav \
  --metadata-json /tmp/pulp-audio-inspector-headless-proof.json \
  --duration 4 \
  --frequency 440 \
  --level-db -12
```

The same recorder can be enabled on lower-level desktop actions:

```bash
./tools/local-ci/local_ci.py desktop click mac \
  --command ./build/pulp \
  --click 120,80 \
  --capture-before \
  --record-video \
  --compose-video-proof \
  --video-duration 8 \
  --video-fps 30 \
  --video-attachment-budget-mb 100
```

The run bundle records:

- `video/proof.mp4`
- `video/proof-composed.mp4` when `--compose-video-proof` is set
- `video/proof.issue.mp4` as the configured GitHub/pro-account attachment target
- `video/proof.small.mp4` when `desktop video --small-video` or
  `desktop compose-video --small-video` is used
- `video/metadata.json`
- `video/issue-metadata.json`
- `video/small-metadata.json` when the small fallback is rendered
- screenshot, before/diff screenshot, logs, and `manifest.json`

`metadata.json` includes `size_bytes`, the attachment budget, and
`fits_attachment_budget`. Keep issue-ready clips under the configured budget.
For GitHub issue review, use 100 MB only for paid-plan video uploaders who are
eligible to upload videos above 10 MB; otherwise use 10 MB or fall back to a
local/internal link. GitHub supports `.mp4`, `.mov`, and `.webm` attachments and
recommends H.264 for compatibility.
When the review source is already under budget, `proof.issue.mp4` is a copy of
that source. When it is over budget, the tool runs a bounded H.264 retry ladder:
balanced 720p at 24 fps, compact 720p at 15 fps, then compact 540p at 15 fps.
The transcode ladder maps optional audio and writes AAC when the source has an
audio stream.
`issue-metadata.json` records each attempt, the selected attempt when one fits,
and final `status=transcoded`, `exceeds-budget`, or `transcode-failed`.
For review lanes that should always prepare a 10 MB fallback, rerender with
`--small-video`; this writes `proof.small.mp4` and `small-metadata.json` using
`--small-video-budget-mb` independently from the pro-account issue budget.

`--video-audio none` remains the default. `--video-audio system` records an
explicit macOS AVFoundation audio input into the raw MP4 and resulting
issue-ready clip. Pass `--video-audio-device <index-or-name>` or set
`PULP_VIDEO_AUDIO_DEVICE`; common loopback setups use a device such as
`BlackHole 2ch`. The harness does not guess a microphone/system device, and an
audio-requested capture will not silently fall back to a no-audio frame
sequence.

For Pulp-owned plugin or standalone validation where the harness can render the
expected audio directly, prefer the explicit plugin-audio mux path:

```bash
python3 tools/local-ci/local_ci.py desktop video mac \
  --recipe audio-inspector-demo \
  --command ./build-video-nogpu/examples/audio-inspector-demo/pulp-audio-inspector-demo \
  --video-audio plugin \
  --video-audio-file /tmp/pulp-audio-inspector-headless-proof.wav \
  --compose-video-proof
```

The WAV is copied into the run bundle as `video/audio.wav`, muxed into
`video/proof.mp4` as AAC, and then carried through Remotion composition and the
issue-size retry ladder. This path avoids microphone guessing and host loopback
setup when the proof should use deterministic plugin-rendered audio.

The macOS `screencapture` frame-sequence fallback normalizes encoded video
dimensions to even pixel counts. This avoids libx264 failures when a host
exposes an odd-sized floating editor window.

## Remotion Composition

`--compose-video-proof` uses the repo-local Remotion tooling to render an
annotated proof video with run context, source identity, a launch/action/capture
timeline, attachment status, and the raw recording. The composed clip is what
reviewers should watch first.

You can rerender a composed clip from an existing run bundle:

```bash
python3 tools/local-ci/local_ci.py desktop compose-video /path/to/run/manifest.json
```

That command writes `video/proof-composed.mp4`, `video/composed-metadata.json`,
`video/proof.issue.mp4`, and `video/issue-metadata.json`, then updates the run
manifest. Add repeatable `--note` flags when an existing raw capture needs more
reviewer context in the visible proof steps. The lower-level npm script is still
available for template iteration:

`composed-metadata.json` includes a `review_storyboard` block with the title,
template, launch/action/capture/review steps, selected source identity, capture
details, and issue-variant state. `desktop publish` carries that storyboard into
`index.html`, `review.md`, `review-package.json`, and generated
`github-issue.md`, so reviewers can understand what the clip is proving before
they open the MP4.

For source/design comparison reviews from two still images, use
`desktop design-proof`. This creates a run manifest, generates a source/native
diff, renders the Remotion `design-parity` proof, and writes both the normal
issue MP4 and optional 10 MB fallback:

```bash
python3 tools/local-ci/local_ci.py desktop design-proof \
  --source-image planning/screenshots/reference.png \
  --native-image /tmp/native-render.png \
  --label figma-native-design-proof \
  --source-label "Figma reference" \
  --title "Design parity proof" \
  --note "The source reference was exported from Figma before composition." \
  --context fixture=elysium.pulp.zip \
  --video-attachment-budget-mb 100 \
  --small-video \
  --small-video-budget-mb 10
```

Use the lower-level two-step path when comparing a source image against an
existing recorded run manifest, or when rerendering a proof with a custom diff:

```bash
python3 tools/local-ci/local_ci.py desktop design-diff \
  --source-image planning/screenshots/reference.png \
  --manifest /path/to/run/manifest.json \
  --output planning/screenshots/source-vs-native-diff.png \
  --resized-source-output planning/screenshots/source-resized-to-native.png \
  --json

python3 tools/local-ci/local_ci.py desktop compose-video /path/to/run/manifest.json \
  --template design-parity \
  --source-image planning/screenshots/reference.png \
  --source-label "Figma reference" \
  --diff-image planning/screenshots/source-vs-native-diff.png \
  --diff-label "Source vs native screenshot diff" \
  --title "Design parity proof" \
  --note "The imported layout keeps the same primary control grouping." \
  --video-attachment-budget-mb 100 \
  --small-video \
  --small-video-budget-mb 10
```

`desktop design-proof` and `desktop design-diff` both compare the
source/reference image with a native screenshot, resizing the source image to
the native dimensions when needed. `design-proof` is the repeatable one-shot
route for still-image source/native reviews; `design-diff` writes a visual diff
image and JSON metadata that includes copy-paste `compose_args` for an existing
run. Both use Pillow when available and fall back to a stdlib PNG path for
8-bit RGB/RGBA PNG exports and screenshots. The composition renders the
source/reference image beside the native proof and adds the diff image to the
proof context panel. If the run manifest already has `artifacts.diff_screenshot`,
`compose-video` uses it automatically unless `--diff-image` overrides it. The
commands record a `video_proof_composition` block in the run manifest.

```bash
npm --prefix tools/local-ci run compose-video-proof -- \
  --manifest /path/to/run/manifest.json \
  --output /path/to/run/video/proof-composed.mp4
```

## Local Review

Before recording a demo batch, print the curated proof matrix:

```bash
python3 tools/local-ci/local_ci.py desktop video-matrix --markdown
```

The matrix lists the full-service proof scenarios, current readiness, Remotion
template, doctor command, concrete Release prepare command, recording/compose
command, publish command, review-issue command, background serve/status/stop
commands, and what a reviewer should look for. Use `--target mac`, `--target
ubuntu`, `--target windows`, `--target ios-simulator`, `--scenario
audio-inspector-demo`, `--scenario component-zoom`, `--status ready`, or
`--json` when an agent needs a narrower machine-readable plan. Add `--check`
to include lightweight machine-local readiness checks, such as whether `cmake`,
the in-tree audio-inspector demo source, `adb`, `xcrun`, REAPER, or
`external/skia-build/libskia.a` are available before attempting a demo, and
whether design-parity has a source/reference image plus either an existing run
manifest to recompose or a native screenshot for the one-shot `desktop
design-proof` helper. For design parity, pass concrete inputs when checking a
specific recorded run:

```bash
python3 tools/local-ci/local_ci.py desktop video-matrix \
  --target mac \
  --scenario design-parity \
  --check \
  --design-parity-manifest /path/to/run/manifest.json \
  --design-parity-source-image planning/screenshots/reference.png
```

For a still-image source/native review with no prior recording, pass the native
image instead. The matrix emits a ready `desktop design-proof` command:

```bash
python3 tools/local-ci/local_ci.py desktop video-matrix \
  --target mac \
  --scenario design-parity \
  --check \
  --design-parity-source-image planning/screenshots/reference.png \
  --design-parity-native-image /tmp/native-render.png
```

If `--design-parity-source-image` is omitted, the check first tries
`planning/screenshots/reference.png`. If either design-parity input is still
missing, `video-matrix --check` also searches the configured published report
root for the latest design-parity report that contains both a copied run
manifest and a copied source/reference image. Explicit paths always win. The
failed checks include remediation text with the next setup step. The status
filter uses declared matrix status by default; with `--check`, it uses computed
local readiness, so `--target mac --status ready --check` prints the macOS
proofs this machine can run now. The
`audio-inspector-demo` row is the fast no-GPU macOS proof path and does not
require Skia. The iOS Simulator row uses the working
`simulator video` recorder. The
Android row uses the working `android video` command for adb-connected
emulators/devices with `screenrecord` and timed open-url/deep-link actions. The
Linux/Xvfb and Windows/session-agent rows are marked `planned`; their
`desktop video-doctor` checks fail `backend.recorder` until the ffmpeg
`x11grab` and `ddagrab`/`gdigrab` backends land.

## iOS Simulator Video Proofs

For simulator validation, boot the target simulator first, then run:

```bash
python3 tools/local-ci/local_ci.py simulator video-doctor
```

Build the PulpSineSynth HostApp, then record a bounded MP4 proof of the booted
simulator with app install and launch:

```bash
cmake -S . -B build-ios-sim-video-proof -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphonesimulator \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=16.4 \
  -DCMAKE_BUILD_TYPE=Release \
  -DPULP_ENABLE_GPU=OFF \
  -DPULP_BUILD_TESTS=OFF \
  -DPULP_BUILD_EXAMPLES=ON
cmake --build build-ios-sim-video-proof \
  --target PulpSineSynth_HostApp_Embed \
  --config Release -- -sdk iphonesimulator

python3 tools/local-ci/local_ci.py simulator video \
  --app build-ios-sim-video-proof/AUv3/Release/PulpSineSynth.app \
  --bundle-id com.pulp.examples.sinesynth.host \
  --open-url https://example.com \
  --action-label "open validation URL after HostApp launch" \
  --label ios-pulpsinesynth-hostapp-proof \
  --duration 6 \
  --video-fps 8 \
  --compose-video-proof \
  --video-title "Pulp iOS HostApp proof" \
  --video-note "Installs and launches the PulpSineSynth iOS HostApp, then records the simulator while simctl opens a validation URL." \
  --small-video
```

The command writes `video/proof.mp4` and `manifest.json` under the simulator run
directory. With `--compose-video-proof`, it immediately renders the
`mobile-simulator` Remotion proof and adds `video/proof-composed.mp4`,
`video/proof.issue.mp4`, optional `video/proof.small.mp4`, composition metadata,
and a storyboard to the simulator manifest before publishing. This first
simulator lane proves capture/install/launch plumbing via `xcrun simctl io
screenshot` frames encoded with ffmpeg. When `--open-url` is provided, the URL
opens during capture and the manifest stores a `mobile-simulator` action marker.
Coordinate tap driving still needs a future automation backend because this
Xcode's `simctl` does not expose a tap command.

You can still rerender an existing simulator run with
`desktop compose-video --template mobile-simulator` when only the title, notes,
or size budget need to change.

## Android Emulator Video Proofs

For Android validation, start the emulator or connect a device first, then run:

```bash
python3 tools/local-ci/local_ci.py android video-doctor
```

Record a bounded MP4 proof with optional APK install, package/activity launch,
and a timed tap action:

```bash
python3 tools/local-ci/local_ci.py android video \
  --apk android/app/build/outputs/apk/debug/app-debug.apk \
  --package com.pulp.demo \
  --activity .MainActivity \
  --tap 540,1200 \
  --action-label "tap validation control" \
  --label android-emulator-proof \
  --duration 8 \
  --compose-video-proof \
  --video-title "Android emulator tap proof" \
  --video-note "The emulator receives a timed adb input tap during recording." \
  --small-video
```

The command writes `video/proof.mp4` and `manifest.json` under the Android run
directory using `adb shell screenrecord`. With `--compose-video-proof`, it
immediately renders the `mobile-emulator` Remotion proof and adds
`video/proof-composed.mp4`, `video/proof.issue.mp4`, optional
`video/proof.small.mp4`, composition metadata, and a storyboard to the Android
manifest before publishing. With `--tap x,y`, the command runs `adb shell input
tap` during capture and stores a `mobile-emulator` action marker. Use
`--open-url` instead when a URL/deep link is the right validation action; do not
combine both action types in one proof.

You can still rerender an existing Android run with
`desktop compose-video --template mobile-emulator` when only the title, notes,
or size budget need to change.

Stage a local report after one or more desktop runs:

```bash
./tools/local-ci/local_ci.py desktop publish mac --limit 3 --label validation-video-proof
```

To stage a specific run or generated demo manifest, pass it explicitly:

```bash
python3 tools/local-ci/local_ci.py desktop publish \
  --manifest /path/to/run/manifest.json \
  --label validation-video-proof
```

The generated `index.html` renders videos with native browser controls. To
share from a Tailscale-visible machine, serve the publish directory with a local
static server and link that URL in the review issue. Use background mode when
the link needs to stay alive after the agent response:

```bash
python3 tools/local-ci/local_ci.py desktop serve /path/to/published-report \
  --host 0.0.0.0 \
  --port 8765 \
  --auto-port \
  --background \
  --label validation-video-proof \
  --json
```

`desktop serve` serves the latest published desktop report unless a report
directory is passed explicitly. When bound to `0.0.0.0`, it prints candidate
watch URLs: localhost, this machine's hostname, any comma-separated
`PULP_DESKTOP_SERVE_HOSTS`, and `tailscale ip -4` results when the Tailscale CLI
is available. Set `PULP_DESKTOP_SERVE_HOSTS=blackbook.tailnet-name.ts.net` when
you already know the friendly Tailnet DNS name you want reviewers to tap.
Manage a background server with the same label:

```bash
python3 tools/local-ci/local_ci.py desktop serve --status --label validation-video-proof --json
python3 tools/local-ci/local_ci.py desktop serve --stop --label validation-video-proof --json
```

After reviewers have watched or attached the proof, prune old published reports
without touching run bundles:

```bash
python3 tools/local-ci/local_ci.py desktop cleanup --published --older-than-days 14 --keep-last 3 --json
```

Use `--auto-port` for shared review links so a stale server on `8765` does not
produce a dead URL; the JSON response records the actual port and candidate
watch URLs. If background startup returns `status: failed`, the URL is not live.
Check the JSON `stderr_tail` for bind errors such as `Address already in use`,
then stop the conflicting labeled server or choose another port before sharing
the link.

`desktop publish` also writes `review.md` and `review-package.json` next to
`index.html`. The publish step records the same candidate watch URLs in
`index.json`, `review.md`, and `review-package.json`; start `desktop serve` for
that report directory to make those URLs live. `review.md` is the human issue
body. It includes the local report path, foreground and background
Tailscale/local serve commands, status/stop commands for cleanup, each run's
video artifact, attachment-budget status, and the expected reviewer response.
`review-package.json` is the machine-readable handoff for future upload
automation: it records each run's primary or small attachment decision, absolute
MP4 path when a file should be attached, size/budget fields, and the served
report fallback including background/status/stop commands. It also preserves
review context from the run manifest: exact
launch command when available, source branch/SHA/mode, host, adapter, and copied
manifest path. When Remotion composition metadata is available, it also preserves
the `review_storyboard` steps so issue automation can show what launched, what
action happened, what changed, and what the reviewer should verify. When a small fallback exists, the markdown also lists
`proof.small.mp4`, its 10 MB budget status, and whether that smaller file should
be attached instead of the pro-account issue video. Both files include the
concrete attach/do-not-attach decision and `desktop verdict` commands for
approval or follow-up. They also include a `desktop review-status` command for
checking the issue's `looks good to me` approval trigger before recording the
final verdict. GitHub's
current hosted attachment policy is 100 MB for paid-plan video uploads when the
uploader is eligible; otherwise plan for the 10 MB video cap and use the served
report link.

Generate a local GitHub issue draft from the review package before opening the
issue:

```bash
python3 tools/local-ci/local_ci.py desktop review-issue /path/to/published-report \
  --repo owner/repo \
  --check-files
```

The command accepts either the report directory or its `review-package.json`.
It writes `github-issue.md` and `github-issue.json` next to the report without
calling GitHub. The JSON draft lists attachable MP4 paths, fallback links,
source/command/manifest context for each run, the close trigger (`looks good to
me`), a read-only `desktop review-status` polling command, and a suggested `gh
issue create` command. `--check-files` verifies that
every attachable MP4 still exists and fits its recorded attachment budget before
writing the draft; runs that already use the served-report fallback remain
valid.

To create the GitHub issue directly after writing the local draft, add
`--create`. Labels and assignees are passed through to `gh issue create`:

```bash
python3 tools/local-ci/local_ci.py desktop review-issue /path/to/published-report \
  --repo owner/repo \
  --check-files \
  --create \
  --label video-review \
  --assignee @me
```

This does not upload MP4 attachments; `gh issue create` accepts the issue body
but not local binary attachments. Use the generated JSON attachment decisions to
attach `proof.issue.mp4` or `proof.small.mp4` manually, or include the served
report link.

Before recording the final verdict, check whether the review issue has the
approval trigger:

```bash
python3 tools/local-ci/local_ci.py desktop review-status \
  https://github.com/owner/repo/issues/123 \
  --manifest /path/to/run/manifest.json \
  --close-issue
```

This command is read-only. It uses `gh issue view` to inspect comments for
`looks good to me`; when found, it reports `approved: true` and prints a
suggested `desktop verdict` command. Add `--json` for automation handoff, and
add `--repo owner/repo` when passing a bare issue number.

The intended review loop is:

1. Publish the report.
2. Generate `github-issue.md` / `github-issue.json` with
   `desktop review-issue --check-files`.
3. Open a GitHub issue with `github-issue.md`.
4. Attach the MP4 manually when it fits the configured budget, or use the served
   report link when it does not.
5. Check the issue for the reviewer approval phrase.
6. Record the review verdict in the run manifest once the reviewer responds.
7. Close the issue once the reviewer comments `looks good to me`.

```bash
python3 tools/local-ci/local_ci.py desktop review-status \
  https://github.com/owner/repo/issues/123 \
  --manifest /path/to/run/manifest.json \
  --close-issue
```

`desktop review-status` is read-only. It calls `gh issue view`, detects the
close trigger (`looks good to me`) in issue comments, and prints or JSON-emits a
suggested `desktop verdict` command when approval is present. Use `--repo
owner/repo` when the issue argument is a bare number or when GitHub context
cannot be inferred.

To converge on all open video-review issues at session start, use
`desktop review-watch`. It first lists open issues with the `video-review`
label, then views only uncached or changed issues when a state file is provided:

```bash
python3 tools/local-ci/local_ci.py desktop review-watch \
  --repo owner/repo \
  --label video-review \
  --state-file /tmp/pulp-video-review-watch.json \
  --manifest-map /tmp/pulp-video-review-manifests.json \
  --close-issue \
  --json
```

The optional manifest map is a JSON object keyed by issue URL, issue number, or
`#number`, with values pointing to run `manifest.json` files. When an approved
issue has a mapped manifest, `review-watch` emits the exact `desktop verdict
... --approved --issue-url ...` command, and includes `--close-issue` when that
flag was passed. Use `--interval N --max-iterations M` only for short
post-ping watch windows; one-shot mode is the default.

```bash
python3 tools/local-ci/local_ci.py desktop verdict /path/to/run/manifest.json \
  --approved \
  --issue-url https://github.com/owner/repo/issues/123
```

To close the review issue through `gh` in the same step after an approved
verdict, add `--close-issue`:

```bash
python3 tools/local-ci/local_ci.py desktop verdict /path/to/run/manifest.json \
  --approved \
  --issue-url https://github.com/owner/repo/issues/123 \
  --close-issue
```

If the proof needs another iteration, record that state instead:

```bash
python3 tools/local-ci/local_ci.py desktop verdict /path/to/run/manifest.json \
  --needs-work \
  --issue-url https://github.com/owner/repo/issues/123 \
  --comment-issue \
  --notes "Zoom starts too late; recapture with the component centered."
```

The manifest `review` block records `approved` or `needs-work`, the review
timestamp, optional reviewer notes, and whether the review issue can be closed.
The command also writes `review-verdict.md` and `review-verdict.json` next to
the run manifest. Use `--comment-issue` when the GitHub issue should receive
the generated summary/checklist immediately; without it, use the markdown file
as the pasteable issue closeout comment or same-issue follow-up checklist. The
JSON file is the automation handoff.

## Current Scope

This first lane records the target window region on macOS with H.264 video. It
can include AAC audio from either explicit AVFoundation system capture or an
explicit plugin-rendered WAV mux. It uses ffmpeg/AVFoundation screen capture as
the primary recorder and falls back to a short sequence of trusted
`screencapture -l` window frames when the ffmpeg recorder cannot start and no
system audio was requested. If macOS refuses window-ID capture but allows
full-screen capture, the fallback captures full-screen frames and crops them to
the target window bounds during encoding. Final still screenshots use a
last-resort full-screen fallback for the same TCC edge case.

Remotion composition and local review artifacts are implemented in this branch.
iOS Simulator, Android, richer REAPER plugin interaction automation, and GitHub
issue automation are planned follow-on layers.
