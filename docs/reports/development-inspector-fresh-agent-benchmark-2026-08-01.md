# Development Inspector fresh-agent benchmark — 2026-08-01

## Verdict

PASS after one documented guide correction.

A zero-context agent received only the public Claude plugin guide and a scratch
project path. It discovered one live publication, inspected effective
authority, completed a parameter read → typed write → reread loop, captured and
validated compositor-backed PNG evidence, and wrote a credential-free evidence
bundle without operator-supplied selectors, commands, environment overrides, or
corrections.

The passing run used `fork_turns: none`. It completed in 173 seconds with 18
shell commands and zero undocumented interventions.

## Benchmark prompt

The agent was asked to:

1. use only the public guide and project path;
2. discover the exact live Development Inspector publication;
3. explain effective authority;
4. perform one allowed parameter read → typed write → reread loop;
5. capture and verify image evidence;
6. record commands, failures, identity, values, and evidence paths;
7. stop rather than guess if orientation was missing.

It was explicitly forbidden from inspecting implementation source, tests,
build directories, fixture readiness files, git history, prior-agent output,
credentials, or tokens.

Prompt SHA-256:
`0d9cecf11aa88e87bb28c78d0edb9d949e0e9febe265d4f1229681e88a0fba51`.

## First run: useful failure

The first clean agent discovered the exact publication and saved a verified
420×272 PNG, but correctly refused to guess a parameter ID. The public guide
named the typed mutation and MCP reader but did not name the installed CLI
parameter-read command. Its configured MCP reader also referenced a stale
machine-local CLI path.

That run took 378 seconds and made no undocumented intervention. It produced a
blocked-state report instead of fabricating success.

The guide was corrected in `a386c6d85a` to document the complete installed CLI
read/write/reread sequence with `State.getParameters` and typed
`inspect set-parameter`. No message or correction was sent to the running
agent.

## Passing rerun

The second agent independently discovered and pinned:

- session: `session-6cab904caf49a4bafacff5196fde24e8`
- instance: `instance-956a0abcb9ed26618dc1d4cf18e9cf5b`
- publication: `a9fc1161335c84c44fcc485d2ac6a10a`
- plugin: `com.pulp.test.standalone-inspector-workflow`
- profile: `develop`

It reported these effective grants:

- `session.describe`
- `session.control`
- `state.read`
- `ui.read`
- `diagnostics.read`
- `logs.read`
- `capture.image`
- `state.write`
- `authoring.tweaks`

It did not claim `Runtime.evaluate`, test input, telemetry, or trace authority.

The selected live parameter was numeric ID `42017`, `Workflow Gain`, with a
plain range of −24 to +6 dB. The verified transition was:

- before: `-6 dB`
- typed request: `-5.5 dB`, `normalized: false`
- response: `result.ok: true`
- reread: `-5.5 dB`

The same exact session, instance, and publication selectors were used
throughout.

The screenshot response declared `image/png`; strict base64 decoding and the
eight-byte PNG signature passed, and an independent file inspection reported a
420×272 RGBA PNG.

## Evidence identity

Release-package inputs used by the black-box client environment:

- CLI archive:
  `c3cb086177a2a92712c1c7f7f3240be64e75f7dcc6c342bc4f97b93a17e983f1`
- installed `pulp`:
  `0dab6fb74c588c3ed6606a6c5b08c7621bc2fb1ab05ea639eec9e2b7dbd8a6f9`
- installed `pulp-cpp`:
  `2c7e7a97c1aae3623476b3e7e0e7bd9f57e10e148f9d327e093ada934d9bf4ae`
- installed `pulp-mcp`:
  `04653e15830df3e7eb553c1b5498e584db47126b86d42618c970cbd5fe6e15ac`
- corrected public guide:
  `2df036349a1e587de88074e8fe85829eb073e564abc4faf865ed1231d6443bac`

Passing-run artifact hashes:

| Artifact | SHA-256 |
|---|---|
| agent report | `69a3c25a5aa87504852585330d77ca80cf83cc710be4c3deb2a4cf2484f48ef8` |
| transcript | `105467e3e982cac5b2992a1fd365288331108029c249157b6433955b53fb837e` |
| capabilities | `465a9aa726e3a35f41a537b822b6a054b1295d2876323db253d60690e6f7c9e3` |
| parameters before | `b7266f6016735da69c87f632e329b18909e6c3f4c5b3add3be5ae548f2027171` |
| mutation | `bcf029684fa1eccea4a754769bf28859accbdd981a7ddeb125c98362d179af33` |
| parameters after | `d9eeecb181a12b6415d0f75caf14bc6f5309c2168aa09846d2215b26b38f233f` |
| screenshot response | `5738b26abda9145d6150c32909d0d377662c55c448faeb5c346b21d488d87860` |
| decoded PNG | `e185554b7d5235bc12450ed56d893e428d9fb778b13b330ce78bd959199e42ef` |

The detailed local evidence bundle was retained outside the source checkout
during the run. It contained the prompt, credential-free command transcript,
report, capabilities, before/mutation/after JSON, screenshot response, and
decoded PNG.

## Friction removed

The passing agent still found two small orientation ambiguities:

1. `--output` looked general in CLI help but applies only to `--command`
   responses.
2. The public guide described MCP discovery but left the equivalent CLI
   `profiles`/`list`/`capabilities` spelling to installed help.

`1405742cee` removes both: CLI help now states the command-only output
contract, a shell-out assertion pins that wording, and the public guide includes
the explicit installed CLI orientation sequence and effective-authority
interpretation.

Temporary PATH shims and the live benchmark fixture were removed after the
passing run. A final discovery query returned an empty session list.
