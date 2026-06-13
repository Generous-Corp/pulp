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

Create the machine-local config on a new checkout:

```bash
cp tools/local-ci/config.example.json tools/local-ci/config.json
```

Install the repo-local developer ffmpeg on a new Mac:

```bash
npm --prefix tools/local-ci install
```

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

`video-doctor` should show `PASS screencapture`, `PASS video_capture`,
`PASS target.video_capture`, and `PASS remotion_smoke` before `--record-video`
can produce a composed clip. `PASS avfoundation_screen` is preferred because it
uses the primary ffmpeg/AVFoundation path; if it fails while `screencapture`
passes, the recorder can still use the screencapture fallback. If
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
  --command ./build/pulp \
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
  --command ./build/pulp \
  --duration 8
```

```bash
python3 tools/local-ci/local_ci.py desktop video mac \
  --recipe component-zoom \
  --command ./build/pulp \
  --component-id compressor-threshold \
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
provided. It also selects the Remotion `component-zoom` template and records
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
- `video/proof.small.mp4` when `desktop compose-video --small-video` is used
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
sequence. `--video-audio plugin` is still reserved until a plugin-origin audio
source is available.

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

For source/design comparison reviews, pass a reference image and the
`design-parity` template:

```bash
python3 tools/local-ci/local_ci.py desktop compose-video /path/to/run/manifest.json \
  --template design-parity \
  --source-image planning/screenshots/reference.png \
  --source-label "Figma reference" \
  --title "Design parity proof" \
  --note "The imported layout keeps the same primary control grouping." \
  --video-attachment-budget-mb 100 \
  --small-video \
  --small-video-budget-mb 10
```

That composition renders the source/reference image beside the captured proof
and records a `video_proof_composition` block in the run manifest.

```bash
npm --prefix tools/local-ci run compose-video-proof -- \
  --manifest /path/to/run/manifest.json \
  --output /path/to/run/video/proof-composed.mp4
```

## Local Review

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
static server and link that URL in the review issue:

```bash
python3 tools/local-ci/local_ci.py desktop serve --host 0.0.0.0 --port 8765
```

`desktop serve` serves the latest published desktop report unless a report
directory is passed explicitly. When bound to `0.0.0.0`, it prints candidate
watch URLs: localhost, this machine's hostname, any comma-separated
`PULP_DESKTOP_SERVE_HOSTS`, and `tailscale ip -4` results when the Tailscale CLI
is available. Set `PULP_DESKTOP_SERVE_HOSTS=blackbook.tailnet-name.ts.net` when
you already know the friendly Tailnet DNS name you want reviewers to tap.

`desktop publish` also writes `review.md` and `review-package.json` next to
`index.html`. The publish step records the same candidate watch URLs in
`index.json`, `review.md`, and `review-package.json`; start `desktop serve` for
that report directory to make those URLs live. `review.md` is the human issue
body. It includes the local report path, a Tailscale/local serve command, each
run's video artifact, attachment-budget status, and the expected reviewer
response.
`review-package.json` is the machine-readable handoff for future upload
automation: it records each run's primary or small attachment decision, absolute
MP4 path when a file should be attached, size/budget fields, and the served
report fallback. It also preserves review context from the run manifest: exact
launch command when available, source branch/SHA/mode, host, adapter, and copied
manifest path. When Remotion composition metadata is available, it also preserves
the `review_storyboard` steps so issue automation can show what launched, what
action happened, what changed, and what the reviewer should verify. When a small fallback exists, the markdown also lists
`proof.small.mp4`, its 10 MB budget status, and whether that smaller file should
be attached instead of the pro-account issue video. Both files include the
concrete attach/do-not-attach decision and `desktop verdict` commands for
approval or follow-up. GitHub's
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
me`), and a suggested `gh issue create` command. `--check-files` verifies that
every attachable MP4 still exists and fits its recorded attachment budget before
writing the draft; runs that already use the served-report fallback remain
valid.

The intended review loop is:

1. Publish the report.
2. Generate `github-issue.md` / `github-issue.json` with
   `desktop review-issue --check-files`.
3. Open a GitHub issue with `github-issue.md`.
4. Attach the MP4 manually when it fits the configured budget, or use the served
   report link when it does not.
5. Record the review verdict in the run manifest once the reviewer responds.
6. Close the issue once the reviewer comments `looks good to me`.

```bash
python3 tools/local-ci/local_ci.py desktop verdict /path/to/run/manifest.json \
  --approved \
  --issue-url https://github.com/owner/repo/issues/123
```

If the proof needs another iteration, record that state instead:

```bash
python3 tools/local-ci/local_ci.py desktop verdict /path/to/run/manifest.json \
  --needs-work \
  --notes "Zoom starts too late; recapture with the component centered."
```

The manifest `review` block records `approved` or `needs-work`, the review
timestamp, optional reviewer notes, and whether the review issue can be closed.
The command also writes `review-verdict.md` and `review-verdict.json` next to
the run manifest. Use the markdown file as the pasteable issue closeout comment
or same-issue follow-up checklist; the JSON file is the automation handoff.

## Current Scope

This first lane records the target window region on macOS with H.264 video, and
can include AAC audio when `--video-audio system` is paired with an explicit
AVFoundation audio device. It uses ffmpeg/AVFoundation screen capture as the
primary recorder and falls back to a short sequence of trusted
`screencapture -l` window frames when the ffmpeg recorder cannot start and no
audio was requested. If macOS refuses window-ID capture but allows full-screen
capture, the fallback captures full-screen frames and crops them to the target
window bounds during encoding. Final still screenshots use a last-resort
full-screen fallback for the same TCC edge case.

Remotion composition and local review artifacts are implemented in this branch.
Plugin-origin audio capture, iOS Simulator, Android, richer REAPER plugin
interaction automation, and GitHub issue automation are planned follow-on
layers.
