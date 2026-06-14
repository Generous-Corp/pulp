---
name: video-proof
description: Record, compose, publish, serve, and review short desktop validation video proofs for Pulp UX/test-harness work. Use when the user asks to "show me it working", record a validation video, prove a click/interaction, share a local proof link, attach a video to a review issue, or compare an implemented UI against source material. Covers macOS desktop capture, Remotion proof composition, attachment budgets, local/Tailscale serving, and review closure.
requires:
  scripts:
    - tools/local-ci/local_ci.py
    - tools/local-ci/scripts/compose-video-proof.mjs
  tools:
    - node
    - npm
---

# Validation video proofs

Use this skill when a screenshot is not enough to prove a UX interaction:
clicking a control, opening an inspector, checking a standalone/plugin host flow,
or showing an implementation next to source material. Default to still captures
for static assertions; record video only when motion, timing, audio/visual state,
or reviewer comprehension matters.

## Rules of engagement

- Work on a feature branch or dedicated worktree until humans have reviewed the
  proof workflow. Do not merge generated videos or local config.
- Prefer short clips: 5-12 seconds is the normal range. Keep the recording
  focused on the window or component under test.
- Prefer `video/proof-composed.mp4` for review. Keep `video/proof.mp4` as the
  raw debugging artifact.
- Treat size as a test result. Use `--video-attachment-budget-mb 100` for
  paid/pro GitHub issue attachment planning only when the uploader is eligible
  for videos above 10 MB. Otherwise use a 10 MB budget or publish a local report
  and link the served report instead of attaching the MP4.
- For review issues that may be handled by a non-pro uploader, rerender with
  `desktop compose-video --small-video --small-video-budget-mb 10` so
  `video/proof.small.mp4` is available as a convenience attachment.
- Never commit `tools/local-ci/config.json`, `tools/local-ci/node_modules/`,
  `.remotion/`, or generated desktop artifact bundles.

## First-time setup on a Mac

Start by printing the portable setup checklist. Use the machine label when you
are preparing another Mac, such as blackbook:

```bash
python3 tools/local-ci/local_ci.py desktop video-setup mac --machine blackbook
```

For a JSON handoff artifact that includes current readiness, add
`--check --run-in-terminal` so the readiness check uses Terminal.app's Screen
Recording grant:

```bash
python3 tools/local-ci/local_ci.py desktop video-setup mac --machine blackbook --check --run-in-terminal --json
```

Create the machine-local config on a new checkout:

```bash
cp tools/local-ci/config.example.json tools/local-ci/config.json
```

Install the repo-local developer ffmpeg and Remotion tooling:

```bash
npm --prefix tools/local-ci install
```

Smoke-test Remotion composition without requiring macOS Screen Recording:

```bash
npm --prefix tools/local-ci run smoke-video-proof
```

Enable optional video capture for the mac desktop target:

```bash
python3 tools/local-ci/local_ci.py desktop config set target.mac.video_capture true
```

Prepare the mac desktop target receipt:

```bash
python3 tools/local-ci/local_ci.py desktop install mac
```

Run the doctor before recording:

```bash
python3 tools/local-ci/local_ci.py desktop video-doctor mac
```

Required gates for video:

- `PASS screencapture`
- `PASS video_capture`
- `PASS target.video_capture`
- `PASS remotion_smoke`

`PASS avfoundation_screen` is preferred because it enables the primary
ffmpeg/AVFoundation recorder. If AVFoundation is hidden but `screencapture`
passes, the macOS recorder can still use its screencapture fallback.

Use `--skip-remotion-smoke` for a faster config/tooling check when you do not
need to rerender the synthetic Remotion smoke proof:

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

If `avfoundation_screen` fails, ffmpeg cannot enumerate the primary macOS input
`Capture screen 0`; confirm the configured ffmpeg can run from the invoking
terminal and rerun the doctor. This is a warning, not a hard failure, when
`screencapture` passes.

For audio-bearing proofs, validate the explicit AVFoundation audio device before
recording:

```bash
PULP_VIDEO_AUDIO_DEVICE="BlackHole 2ch" \
  python3 tools/local-ci/local_ci.py desktop video-doctor mac \
    --run-in-terminal \
    --video-audio system
```

Require `PASS avfoundation_audio`; do not guess a microphone or system input.

If `screencapture` fails with `could not create image from display`, macOS has
not granted Screen Recording to the terminal or agent app. Ask the user to grant
Screen Recording, restart that app, then rerun the doctor. You can still update
docs/tests while waiting, but do not claim live capture has been validated.

When Terminal.app has Screen Recording permission but the current agent process
does not, run the same command through Terminal:

```bash
python3 tools/local-ci/local_ci.py desktop video-doctor mac --run-in-terminal
```

Use `--run-in-terminal` on `desktop video`, `desktop smoke --record-video`,
`desktop click --record-video`, or `desktop inspect --record-video` for the same
permission handoff. The wrapper removes the flag for the child command, relays
stdout/stderr and exit code, and sends a short `caffeinate -u` display-wake
pulse first so idle/remote displays do not produce black captures.

The macOS recorder uses ffmpeg/AVFoundation first and falls back to a
`screencapture -l` frame sequence when ffmpeg capture cannot start. If macOS
refuses window-ID capture but allows full-screen capture, the fallback captures
full-screen frames and crops them to the target window bounds during encoding.
Final still screenshots use a last-resort full-screen fallback for the same TCC
edge case. All paths still require Screen Recording permission for the invoking
app.

## Capture a proof

Record a short click proof and render the Remotion composition with the
dedicated video entry point:

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
underlying desktop action. Today, recording is implemented for macOS only;
Linux/Windows video requests fail explicitly until their recorder backends are
wired.

Use repeatable `--video-note` flags for short proof points that should be
visible in the Remotion step list. This is especially useful for host/plugin
proofs where the video shows the host window and logs prove what was loaded.

For a console-style standalone app with no GUI window, record a titled
Terminal.app window instead of waiting for the child process to expose a
capturable window:

```bash
python3 tools/local-ci/local_ci.py desktop video mac \
  --run-in-terminal \
  --action smoke \
  --video-capture-target terminal \
  --command ./build/examples/pulp-tone/PulpTone.app/Contents/MacOS/PulpTone \
  --duration 5
```

Use terminal capture for smoke proofs only. It tees command output into the run
logs, captures the Terminal window, and records the command return code in the
manifest. Use normal app-window capture for clicks, ViewInspector snapshots, and
Pulp app automation.

Use named recipes when the user asks for one of the high-value demos:

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

The REAPER recipe generates a temporary wrapper command and ReaScript when no
explicit `--command` is supplied. The wrapper launches a fresh REAPER instance,
adds a track, tries common REAPER plugin-name prefixes for the requested format,
opens the matching plugin editor, and records the REAPER window via
`--capture-bundle-id com.cockos.reaper`. The default action is a smoke proof;
use `--click X,Y` for coordinate clicks in the host window. ViewInspector
selectors such as `--click-view-id` are rejected because REAPER is not a Pulp
ViewInspector target. Pass an explicit `--command` to keep a
prepared-session workflow; in that mode, add `--capture-bundle-id
com.cockos.reaper` yourself if the command is only a wrapper process. The
`audio-inspector-demo` recipe is a smoke proof for a built standalone demo
binary; pass the command path for the build directory you want to validate.

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

Run the same checks without recording when preparing another machine:

```bash
python3 tools/local-ci/local_ci.py desktop video-doctor mac \
  --recipe reaper-plugin-editor \
  --plugin PulpSynth \
  --plugin-format clap
```

Recipes select Remotion templates and write structured setup context into
`video_proof_composition.context`. The composed video, `index.html`, and
`review.md` display that context, including recipe, host, plugin, format,
component, bundle id, and launch mode when available. `reaper-plugin-editor`
uses the `plugin-host` template, inspector recipes use `inspector-workflow`,
standalone interaction uses `standalone`, component proofs use
`component-zoom`, and source comparisons use `design-parity`.

The `component-zoom` recipe selects the Remotion `component-zoom` template. It
stores `video_proof_composition.focus` from the click selector and resolved
content point, so the composed proof shows a component label, focus rectangle,
and zoom detail inset. Use it when the reviewer should inspect one specific
control instead of hunting through the full app window.

The same recorder can be enabled on lower-level desktop actions:

```bash
python3 tools/local-ci/local_ci.py desktop click mac \
  --command ./build/pulp \
  --click 120,80 \
  --capture-before \
  --record-video \
  --compose-video-proof \
  --video-duration 8 \
  --video-fps 30 \
  --video-attachment-budget-mb 100
```

Adjust `--command`, `--click`, duration, and fps for the actual scenario. Keep
fps at 30 unless there is a motion-specific reason to go higher.

Keep `--video-audio none` unless the proof genuinely needs audio. Use
`--video-audio system --video-audio-device <index-or-name>` only when a known
macOS AVFoundation audio input or loopback device should be recorded; the same
device can be supplied with `PULP_VIDEO_AUDIO_DEVICE`. Do not guess a
microphone/system device. Audio-requested captures must fail rather than fall
back to a no-audio frame sequence. `--video-audio plugin` is still reserved
until plugin-origin audio capture exists.

Each run bundle should contain:

- `video/proof.mp4`
- `video/proof-composed.mp4` when `--compose-video-proof` is set
- `video/proof.issue.mp4` for GitHub/pro-account attachment review
- `video/proof.small.mp4` when `desktop compose-video --small-video` is used
- `video/metadata.json` with `size_bytes`, budget, and
  `fits_attachment_budget`
- `video/issue-metadata.json` with copy/transcode/budget status
- `video/small-metadata.json` with the 10 MB fallback status when requested
- screenshots, logs, and `manifest.json`

`proof.issue.mp4` is copied from the review source when it already fits the
configured budget. If the source is too large, the tool runs a bounded H.264
retry ladder: balanced 720p at 24 fps, compact 720p at 15 fps, then compact
540p at 15 fps. `issue-metadata.json` records every attempt and the final
`transcoded`, `exceeds-budget`, or `transcode-failed` status; use the served
report link when the issue variant still does not fit.

Published reports should contain both `review.md` and `review-package.json`.
Use `review.md` as the human GitHub issue body. Use `review-package.json` as the
structured source of truth for automation: it records whether to attach the
primary issue MP4, attach the small fallback, or use the served/local report
link, along with absolute attachment paths, size/budget fields, fallback serve
commands, and source/command/manifest context from the run.

To recompose an existing raw proof:

```bash
python3 tools/local-ci/local_ci.py desktop compose-video /path/to/run/manifest.json
```

This rerenders `video/proof-composed.mp4`, refreshes composed metadata, creates
or refreshes `video/proof.issue.mp4`, writes issue metadata, and updates the run
manifest.

`video/composed-metadata.json` includes `review_storyboard`: a concise
launch/action/capture/review step list plus source, capture, and issue-variant
context. Published reports and generated issue drafts surface those steps, so a
reviewer can understand the proof before opening the MP4.

To create both the pro-account issue target and a 10 MB fallback:

```bash
python3 tools/local-ci/local_ci.py desktop compose-video /path/to/run/manifest.json \
  --video-attachment-budget-mb 100 \
  --small-video \
  --small-video-budget-mb 10
```

For a design/source comparison proof, use the Remotion design-parity template:

```bash
python3 tools/local-ci/local_ci.py desktop compose-video /path/to/run/manifest.json \
  --template design-parity \
  --source-image planning/screenshots/reference.png \
  --source-label "Figma reference" \
  --title "Design parity proof" \
  --note "The imported layout keeps the same primary control grouping."
```

This renders the source/reference image beside the captured proof and records a
`video_proof_composition` block in the manifest.

## Publish and serve for review

Before recording a batch of examples, print the demo matrix:

```bash
python3 tools/local-ci/local_ci.py desktop video-matrix --markdown
```

Use it to choose the smallest useful proof scenario. `--target mac` narrows to
the current macOS lane, `--target ios-simulator` narrows to simulator capture,
`--target android-emulator` narrows to Android capture, `--target ubuntu` and
`--target windows` show the planned Linux/Xvfb and Windows/session-agent rows,
`--scenario audio-inspector-demo` selects the fast no-GPU audio-inspector proof,
`--scenario component-zoom` prints one row, and `--json` is suitable for
automation. Add `--check` on a fresh machine or blackbook to include
machine-local readiness checks for obvious blockers such as missing `cmake`,
the in-tree audio-inspector demo source, `adb`, `xcrun`, REAPER, or
`external/skia-build/libskia.a`. Failed checks include remediation text with the
next setup step. The matrix carries
readiness status, Remotion template, doctor command, concrete Release prepare
command, recording/compose command,
publish/review-issue commands, background serve/status/stop commands, and
reviewer watch-points for standalone, audio-inspector demo, REAPER/plugin-host, inspector,
component-zoom, design-parity, iOS Simulator, Android Emulator, Linux, and
Windows proofs. Linux/Windows `desktop video-doctor` must fail
`backend.recorder` until their ffmpeg `x11grab` and `ddagrab`/`gdigrab`
backends land.

## iOS Simulator capture

Boot the target simulator first, then check readiness:

```bash
python3 tools/local-ci/local_ci.py simulator video-doctor
```

Record a short simulator MP4 with optional install/launch context:

```bash
python3 tools/local-ci/local_ci.py simulator video \
  --app build/ios/PulpDemo.app \
  --bundle-id com.pulp.demo \
  --open-url https://example.com \
  --action-label "open validation URL" \
  --label ios-simulator-launch-proof \
  --duration 8 \
  --video-fps 10 \
  --compose-video-proof \
  --video-title "iOS Simulator open URL proof" \
  --video-note "Simulator opens the validation URL during recording." \
  --small-video
```

This writes `video/proof.mp4` and `manifest.json` under the simulator run
directory using `xcrun simctl io screenshot` frames encoded with ffmpeg. With
`--compose-video-proof`, it immediately renders the `mobile-simulator` Remotion
proof and writes `video/proof-composed.mp4`, `video/proof.issue.mp4`, optional
`video/proof.small.mp4`, composition metadata, and a storyboard into the
simulator run manifest. With `--open-url`, the command opens the URL during
capture and stores a `mobile-simulator` action marker. Coordinate tap driving
still needs a future automation backend because this Xcode's `simctl` does not
expose a tap command.

Use `desktop compose-video --template mobile-simulator` only when rerendering an
existing run with different notes, title, or size budgets.

## Android Emulator capture

Start the emulator or connect a device first, then check readiness:

```bash
python3 tools/local-ci/local_ci.py android video-doctor
```

Record a short Android MP4 with optional install/launch context:

```bash
python3 tools/local-ci/local_ci.py android video \
  --apk android/app/build/outputs/apk/debug/app-debug.apk \
  --package com.pulp.demo \
  --activity .MainActivity \
  --open-url pulp-demo://validate \
  --action-label "open validation deep link" \
  --label android-emulator-proof \
  --duration 8 \
  --compose-video-proof \
  --video-title "Android emulator deep-link proof" \
  --video-note "The emulator opens the validation deep link during recording." \
  --small-video
```

This writes `video/proof.mp4` and `manifest.json` under the Android run
directory using `adb shell screenrecord`. With `--compose-video-proof`, it
immediately renders the `mobile-emulator` Remotion proof and writes
`video/proof-composed.mp4`, `video/proof.issue.mp4`, optional
`video/proof.small.mp4`, composition metadata, and a storyboard into the Android
run manifest. With `--open-url`, the command opens the URL/deep link during
capture and stores a `mobile-emulator` action marker. Coordinate tap driving
still needs a future automation backend; use package launch and
open-url/deep-link actions for portable Android proof clips today.

Use `desktop compose-video --template mobile-emulator` only when rerendering an
existing run with different notes, title, or size budgets.

Publish the latest runs:

```bash
python3 tools/local-ci/local_ci.py desktop publish mac --limit 3 --label validation-video-proof
```

Publish a specific run or generated demo manifest when discovery is not enough:

```bash
python3 tools/local-ci/local_ci.py desktop publish \
  --manifest /path/to/run/manifest.json \
  --label validation-video-proof
```

Serve a published report on a Tailscale-visible machine. Use background mode
when the link needs to stay alive after the agent response:

```bash
python3 tools/local-ci/local_ci.py desktop serve /path/to/published-report \
  --host 0.0.0.0 \
  --port 8765 \
  --auto-port \
  --background \
  --label validation-video-proof \
  --json
```

When bound to `0.0.0.0`, `desktop serve` prints candidate watch URLs:
localhost, the machine hostname, any comma-separated `PULP_DESKTOP_SERVE_HOSTS`,
and `tailscale ip -4` results when the Tailscale CLI is installed. If the
reviewer should use a friendly Tailnet DNS name, run with:

```bash
PULP_DESKTOP_SERVE_HOSTS=blackbook.tailnet-name.ts.net \
  python3 tools/local-ci/local_ci.py desktop serve /path/to/published-report \
    --host 0.0.0.0 \
    --port 8765 \
    --auto-port \
    --background \
    --label validation-video-proof \
    --json
```

Check and stop the background server with the same label:

```bash
python3 tools/local-ci/local_ci.py desktop serve --status --label validation-video-proof --json
python3 tools/local-ci/local_ci.py desktop serve --stop --label validation-video-proof --json
```

Use `--auto-port` for review links so a stale server on `8765` does not produce
a dead URL; the JSON response records the actual port and candidate watch URLs.
If background startup returns `status: failed`, treat the watch link as dead.
Inspect `stderr_tail` for bind errors such as `Address already in use`, then
stop the conflicting labeled server or rerun `desktop serve` with a different
port before sharing the URL.

`desktop publish` writes `review.md` and `review-package.json` next to
`index.html`, including candidate watch URLs from localhost, configured
`PULP_DESKTOP_SERVE_HOSTS`, and Tailscale when available. Start `desktop serve`
for the report directory to make those URLs live. The review package includes
foreground/background serve commands plus status/stop commands for cleanup.
Generate an offline GitHub
issue draft from that package before opening the issue:

```bash
python3 tools/local-ci/local_ci.py desktop review-issue /path/to/published-report \
  --repo owner/repo \
  --check-files
```

The command accepts either the report directory or `review-package.json`, then
writes `github-issue.md` and `github-issue.json` next to the report without
calling GitHub. Use `github-issue.md` as the issue body. Use
`github-issue.json` to see which MP4s should be attached and which runs need the
served fallback link. The draft also includes launch command, source branch/SHA,
host/adapter, copied manifest path, and the Remotion `review_storyboard` steps
when composition metadata recorded them. It includes a per-run
`desktop review-status` command so agents can poll for `looks good to me`
before applying the verdict.
`--check-files` verifies that every attachable MP4 still exists and fits its
recorded attachment budget before writing the draft; fallback-link runs remain
valid. GitHub supports `.mp4`, `.mov`, and `.webm` attachments and currently
recommends H.264 for compatibility; paid-plan eligible uploaders can use the
100 MB video budget, while others should assume 10 MB. Attach
`proof.issue.mp4` manually when it fits the chosen budget; attach
`proof.small.mp4` when the normal issue video is too large but the small
fallback fits. Otherwise include the served report URL. The review issue can be
closed when the reviewer comments `looks good to me`. The generated review body
also records the attach/do-not-attach decision, the `desktop review-status`
command for checking approval, and the `desktop verdict` commands for approval
or follow-up.

When the user wants the issue created by the agent, add `--create` plus any
labels or assignees:

```bash
python3 tools/local-ci/local_ci.py desktop review-issue /path/to/published-report \
  --repo owner/repo \
  --check-files \
  --create \
  --label video-review \
  --assignee @me
```

This still does not upload MP4 attachments. Use `github-issue.json` to decide
whether to attach `proof.issue.mp4`, attach `proof.small.mp4`, or rely on the
served report link.

Check whether the issue has the approval trigger before recording the final
verdict:

```bash
python3 tools/local-ci/local_ci.py desktop review-status \
  https://github.com/owner/repo/issues/123 \
  --manifest /path/to/run/manifest.json \
  --close-issue
```

`desktop review-status` is read-only. It calls `gh issue view`, detects `looks
good to me` in issue comments, and prints a suggested `desktop verdict` command
when approval is present. Add `--json` for automation and `--repo owner/repo`
when passing a bare issue number.

Record that review state back into the run manifest:

```bash
python3 tools/local-ci/local_ci.py desktop verdict /path/to/run/manifest.json \
  --approved \
  --issue-url https://github.com/owner/repo/issues/123
```

If the user asks to close the accepted review issue, add `--close-issue`:

```bash
python3 tools/local-ci/local_ci.py desktop verdict /path/to/run/manifest.json \
  --approved \
  --issue-url https://github.com/owner/repo/issues/123 \
  --close-issue
```

For another iteration, keep the issue open and capture the reason:

```bash
python3 tools/local-ci/local_ci.py desktop verdict /path/to/run/manifest.json \
  --needs-work \
  --notes "Zoom starts too late; recapture with the component centered."
```

`desktop verdict` writes a manifest `review` block with status, timestamp,
optional reviewer notes, and whether the review issue can be closed. It also
writes `review-verdict.md` and `review-verdict.json` next to the manifest; use
the markdown as the pasteable issue closeout comment or same-issue follow-up
checklist, and keep the JSON for automation.

## High-value demo scenarios

- Standalone app: launch a built standalone, perform one visible interaction,
  and show the inspector or normal UI state changing.
- Plugin host: run a Pulp plugin in a host such as REAPER, click a meaningful
  control, and show host/plugin state updating.
- Audio Inspector: open the Audio Inspector during a running app and show live
  meters or probe state after a stimulus.
- Component zoom: focus on a specific implemented component and prove the
  interaction or visual state that was just changed.
- Design validation: show source material, the implemented UI, and a small
  comparison/diff frame so a reviewer can understand alignment without knowing
  the feature.

Use Remotion to make these clips reviewer-friendly: title card, scenario label,
run metadata, source identity, launch/action/capture timeline, size/budget
status, and short explanatory captions. Keep the composition factual and
concise; the video should explain the proof, not become a long demo reel.
