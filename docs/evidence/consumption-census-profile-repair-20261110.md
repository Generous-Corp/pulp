# Consumption census profile repair

The `darwin-arm64-clap+design-import+inspector+js+lv2+skia` profile had fallen
behind the configured macOS Release+Skia target graph. The baseline helper
regenerated that profile from the official census tool. The G-5 integration
retains its measured profile and adds the two new G-5 public headers to the
shared header map. Neither step changes gate semantics or configure defaults.

## Source and control

- Repair branch base: `35e3c4ce17dfb52dccfbf5c9c8680bef8f49591e`.
- m1 control checkout: `f27efa87d73120d272cef6c4baa7d12258927540`, clean at
  capture; configure reports Release, GPU enabled, Skia present, examples off,
  tests on.
- The control and repair checkout SHA-256 values match for `CMakeLists.txt`
  (`80e04d29af74dd50af9a574b2d888aa31a42ee9f3fb4265fbe1a61f38bf32c58`),
  `tools/scripts/consumption_census.py`
  (`59c5909fccbec1136b0a59b4ebb6005c3b2d98040be1f02bcbb480c42535490f`), and
  `tools/scripts/consumption_census_contract.py`
  (`1b407f3a0a176fdbe6d9455c90f79c7df37512bcf3d3c2192ae661c2c6007532`).
- Since the control commit, the only CMake inputs changed are the g11 test
  registration and installed-consumer fixture. The fixture is not configured
  with examples off; the test addition registers a CTest case and does not add
  or change installed `Pulp::*` targets. No public include paths changed.
- The m1 build tree is the pre-existing `build` directory in the control
  checkout. The checkout, cache, facts, and exported-target file were left
  untouched. Generation used a separate census seed copied from the repair
  branch so the control checkout's historical census was not rewritten.

## Measured drift and proof

The old profile check exited 1. The control reported two exported targets not
recorded by the old profile (`Pulp::inspect-agent-toolkit` and `Pulp::native`)
and link-closure drift including the GPU runtime targets and Apple frameworks.
The official `tools/scripts/consumption_census.py --write` regenerated the
profile from that control. It records 87 targets (previously 85); the shared
public-header map is unchanged, the profile key set remains three, and both
other profile records have identical JSON values to the original document.

Focused proof:

- Regenerated-profile `--check`: passed on m1 (`targets=87`).
- `--validate-only`: passed (`profiles=3`).
- `consumption_census_contract.py`: passed all 14 contract cases, including
  valid-current and the planted drift/schema/facts failures.
- `test_consumption_census_contract_note.py`: passed.
- Local `test_consumption_census.py`: passed 22 tests; two optional SDK header
  roots are not provisioned on this host.
- Local generated-profile schema validation: passed.

The generated census SHA-256 is
`113e9b73a7e4c14e04865c6b462137b281d34718d9299ff4686eacdf941e419e`.
The graph lead directed handoff-only integration by G-5, with no standalone
push, PR, or duplicate gates. The normal gates run belongs to the integrating
G-5 change.

## G-5 integration

The G-5 checkout merged protected `origin/main` before re-deriving. Its own
Release+VST3/AUSDK+Skia configure regenerated the SDK profile at 89 targets;
that profile matched the protected base. The helper's no-SDK Skia profile was
then applied as the only profile-record replacement. The shared header map was
generated from the G-5 tree and added `pulp/host/control_index_nodes.hpp` and
`pulp/signal/control_math.hpp`. Assertions confirmed that the helper had left
the other two profile records and header map identical to its base. The final
combined document passed `consumption_census.py --validate-only` (three
profiles) and `--check` on the G-5 SDK build (89 targets). The no-SDK Skia
profile had independently passed the helper's official `--check` (87 targets).
