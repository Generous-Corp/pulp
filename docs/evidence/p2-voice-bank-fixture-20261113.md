# Voice-bank state round trip and hard-cut swap: fixture evidence (2026-11-13)

Fixture-only extension of the installed-SDK voice-bank consumer
(`examples/g6-voice-bank-consumer`). No `core/` source or public header was
edited. Runtime behaviour is unchanged; the consumer proves what the existing
public surface already does and records where a host has to supply
discipline.

## Source and SDK provenance

- Fixture commit: `fe1f0839e9c9` on `graph/p2-voice-bank-fixture-20261113`,
  branched from `origin/main` `d486ff70c6b0`.
- SDK: the clean Release source install at
  `/Volumes/Workshop/Code/agent-worktrees/a10-sdk-release-prefix-20261112`,
  version `1.120.0`, `sdk_build_type.txt=Release`. Its
  `sdk-provenance.json` (SHA-256
  `6e087db51c35537ffac6816728fcbc337bcbcd1df562cda9c5e8a5b75b2736eb`) reads
  `kind=source-install`, `source_git_sha=4c69e183387017c47d048b477218a6d0a4947bcc`,
  `source_git_dirty=false`, `platform=darwin-arm64`, `build_profile=release`.
- `4c69e18` is an ancestor of `d486ff70c6b0`. Between the two, no file under
  `core/host` changed. The `core/midi` changes are `coremidi_device.mm` and
  `port_identity.cpp`, which this consumer does not use.
- Installed header digests, each equal to `git show origin/main:<path> | shasum -a 256`:

| Header | SHA-256 |
|---|---|
| `pulp/host/voice_bank_binding.hpp` | `d622f444c145ccced476407b97bc62f59912601d4804dc557e4d022882d503a4` |
| `pulp/host/voice_template.hpp` | `a02c5df0f5da8aceebb2b273bff81d96310689f189971e426f31510459fb7e4e` |
| `pulp/host/voice_identity.hpp` | `5165d96c680b0449ff9147de3836b5b17c332a0ec6c72021be0efa5748ad10b5` |
| `pulp/host/graph_event_contract.hpp` | `1364609e77aee4e39fa3c1692e36c715ec9385856515dc3076d1de7ef0b0350a` |
| `pulp/host/signal_graph_subgraph.hpp` | `81103ef7f256181918a1e4b60c254451192f492be5959e10a88e63858b6063c0` |
| `pulp/midi/mpe_synth_voice.hpp` | `d1f0721725aece5c13976d039d131baea39436f4191df8c0de1ec5606c31be5a` |
| `pulp/midi/mpe_voice_tracker.hpp` | `c90be261b57145983c90b269c2d4512b9d3211f492032dbc55885c197cfb6299` |

- The mutation scratch copies come from `origin/main`.
  `core/host/src/voice_bank_binding.cpp` is
  `916e579258198d60f8dfe31a336a039cbd5560fcfcf0d9e441216e64667dfcb6` and
  `core/host/src/voice_template.cpp` is
  `a50c3c8bba495ae9f79d24491d8924af4a062e6195496e6f0f290fee7419a198`. Both are
  identical at `4c69e18`.

## Commands

```sh
P=/Volumes/Workshop/Code/agent-worktrees/a10-sdk-release-prefix-20261112
S=/Volumes/Workshop/Code/agent-worktrees/p2-voice-bank-fixture-20261113-scratch
python3 examples/g6-voice-bank-consumer/generate_a1_vectors.py --check
cmake -S examples/g6-voice-bank-consumer -B "$S/build-g6" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$P" -DPulp_DIR="$P/lib/cmake/Pulp"
PULP_BUILD_JOBS=1 nice tools/ci/governed-build.sh cmake --build "$S/build-g6"
"$S/build-g6/pulp-g6-voice-bank-consumer"
(cd "$S/build-g6" && ctest --output-on-failure)
python3 examples/g6-voice-bank-consumer/prove_mutations.py --sdk-prefix "$P" --scratch "$S/mutations-head"
PULP_MERGE_TREE_CHECK_TIMEOUT=120 tools/scripts/gates.sh origin/main
```

`gates.sh` returned `✓ PASSED WITH 1 NOT CHECKED`. The unchecked item is the
build-gate configure and source-selftest suites, which this gate host refuses
(`TARTCI_ROLE=dedicated-builder`). Diff coverage was also not run on this
host. The `text-encoding-lint`, examples-index and format checks passed.

The A1 vector header is unchanged and `generate_a1_vectors.py --check`
passes. The new code does not affect vector generation.

## Positive result

```
cross-bank unguarded retired token on colliding replacement: code=0
g6-voice-bank-public-consumer PASS partitions=1,32,64,127,256 samples=800 sample3=-0.353553 sample6=-0.314663 sample10=0
```

`ctest`: `g6-voice-bank-public-consumer` passes (1/1). The consumer binary's
SHA-256 is `d3dd5848a750e43048ae99b8e3520f64a143d98a5c4db118b2494e9dff355d22`.

### (a) Save and restore on a live binding (`check_state_round_trip`)

- The consumer sets the eight slot levels to `0.25, 0.5, ..., 2.0` with
  `set_slot_parameter`. The binding has no getter, so the consumer keeps its
  own copy (`capture_levels`, `reapply_state` in `fixture.hpp`).
- It renders a reference script: eight notes on member channels 1-8, released
  at sample 48, 96 frames in 32-frame blocks. A control bank at default levels
  renders differently (`restore-levels-audible`), so the comparison depends on
  the restored values.
- A note is left held on the live bank (`active_count()==1`). The levels are
  then encoded against `live->manifest()`.
- A fresh bank is prepared with the same manifest digest. The state is decoded,
  restored with `restore_voice_bank_state(fresh->manifest(), ...)`, and
  reapplied with `set_slot_parameter`.
- **Notes are not persisted.** The restored bank has `active_count()==0`
  (`restore-no-active-voices`), which matches `voice_template.hpp:79-81`.
- The restored state re-encodes to identical bytes and the same SHA-256.
- **Tolerance: bit-exact.** The replayed script is `memcmp`-equal on both
  channels (`restore-audio-bit-exact`).
- Each planted negative below is refused, and the previously restored
  destination stays field- and bit-identical:
  - a manifest digest with one character changed
  - the manifest of template revision 2
  - forged slot 8
  - undeclared target `cutoff`
  - NaN, +inf and -inf values
- The same forgeries in wire form are refused by `decode_voice_bank_state`
  with the destination untouched: slot 8 written into the first record, and
  NaN bits written into the last value. The fixture asserts the wire layout
  before patching it.
- **Observation (not a gap).** `VoiceBindingSpec` has no bounds, so a finite
  declared value of 5.0 (authored max 2.0) passes `restore_voice_bank_state`.
  `set_slot_parameter` refuses it (`voice_bank_binding.cpp:602-613`), and the
  slot keeps its level: a re-render is still bit-exact.
  - `set_slot_parameter` applies one value per call. Replaying a multi-value
    state onto a *live* bank is therefore not transactional.
  - The fixture avoids this by applying only to a freshly prepared bank, which
    it discards on any refusal. A host must do the same.

### (b) Declared hard-cut swap while sounding (`check_hard_cut_swap`, `check_cross_bank_swap`)

Within one bank:
- A note-on renders a 32-frame block with peak > 0.1.
- `activate()` inside an open block returns `false` with
  `VoiceBankBindingCode::InvalidBlock` (`voice_bank_binding.cpp:422-424`). The
  epoch is unchanged, the voice keeps sounding, and the block renders audible.
- Between blocks, `activate()` returns Ok and the epoch becomes old+1. Owner
  `active_count()==0` (`voice_bank_binding.cpp:430-436`), and the next block
  is exactly `0.0f` on both channels.
- Old-epoch `dispatch_deferred` returns `StaleEpoch`.
- An old-epoch `publish` in the new block returns `HostEventResult::StaleEpoch`.
- A new-epoch note-on renders audible. Its generation is greater than the
  retired one, because the tracker generation is monotonic across `activate()`.
  New-epoch deferred NoteOff returns Ok.

Across two prepared banks:
- An idle replacement bank refuses the retired bank's `(epoch, generation)`
  with `StaleGeneration` (`cross-bank-idle-token-not-live`).
- **Finding.** Every `VoiceBankBinding` starts at epoch 1
  (`core/host/src/voice_bank_binding.cpp:133`), and every tracker starts at
  generation 1 (`core/midi/include/pulp/midi/mpe_voice_tracker.hpp:304`).
  - When the replacement plays the same note on the same channel, it gets the
    same generation (`cross-bank-generation-collides`).
  - The retired token is then **accepted** by the replacement (`code=0`, i.e.
    `Ok`, printed above), so the replacement dispatches that NoteOff to its own
    voice. The fixture prints this result and does not assert it, so a future
    runtime fix does not break the fixture.
  - Tokens carry no binding-instance identity. They are scoped only by the
    `VoiceBankBinding` object the host calls.
- **Host discipline that closes it (proven).** Before routing to the
  replacement, the host calls `activate()` on it until its epoch exceeds the
  retired epoch. The same colliding token then returns `StaleEpoch`
  (`cross-bank-retired-token-stale-epoch`). A host can also discard every
  retired-bank deferred queue at swap.
- **Release tails across a swap (buildable).** The host keeps rendering the
  retired bank after a NoteOff until `active_count()==0`. The tail is audible
  and bounded (under 16 frames), while the replacement takes new notes.

## Mutation proof

The receipt is `$S/mutations-head/receipt.json`
(`schema=pulp.g6-production-mutation-proof.v1`). It covers 28
positive/planted/restored cases at source head `fe1f0839e9c9`. Receipt
SHA-256: `2130af06c31e0de5b60ec46b9ee19608cf4f26f95c0dcdaa1b3e30c39ec6def9`.

The receipt's `source_git_dirty=true` has one cause: this evidence file was
untracked in the worktree while the proof ran. Build and scratch directories
live outside the worktree, and the fixture sources were committed at
`fe1f0839e9c9`.

The 16 existing production mutants still fail with their own markers. The 10
new mutants:

| Mutant | Edited scratch copy | Change | Observed failure |
|---|---|---|---|
| `activate_skips_owner_reset` | binding | drop `impl_->owner->reset()` in `activate()` | `swap-voices-cut` |
| `activate_keeps_epoch` | binding | drop `++impl_->epoch` | `swap-epoch-advanced` |
| `activate_inside_open_block` | binding | `if (impl_->block_open)` -> `if (false)` | `swap-activate-open-block-refusal` |
| `skip_parameter_upper_bound` | binding | drop `value > maximum` in `set_slot_parameter` | `restore-out-of-range-reapply-refusal` |
| `restore_ignores_manifest_identity` | voice_template | drop the candidate digest comparison in restore | `restore-sha-mismatch-refusal` |
| `normalize_accepts_forged_slot` | voice_template | drop `slot_ordinal >= kSlots` | `restore-wire-forged-slot-refusal` |
| `normalize_accepts_non_finite` | voice_template | drop `!std::isfinite` | `restore-non-finite-refusal` |
| `drop_restore_reapply` | consumer `fixture.hpp` | `reapply_state` applies nothing | `restore-audio-bit-exact` |
| `restore_onto_live_bank` | consumer `main.cpp` | restore onto the sounding bank, no fresh prepare | `restore-no-active-voices` |
| `skip_replacement_epoch_advance` | consumer `main.cpp` | no epoch advance before routing to the replacement | `cross-bank-retired-token-stale-epoch` |

Positive and restored cases exit 0 with the numeric oracle. Every planted case
exits 1 with its named rule.

## Conclusion

**FIXTURE-ONLY satisfied.** Every check in (a) and (b) is met through the
public surface, and no API was added.

One candidate P2 runtime gap is recorded for a second consumer to reproduce
before admission:

- **Cross-binding token aliasing.** `VoiceBankBinding` epochs
  (`core/host/src/voice_bank_binding.cpp:133`) and tracker generations
  (`core/midi/include/pulp/midi/mpe_voice_tracker.hpp:304`) both restart at 1
  for every prepared bank. A retired bank's deferred `(epoch, generation)` is
  therefore accepted by a replacement bank that plays the same channel and
  note (`dispatch_deferred`, `voice_bank_binding.cpp:583-600`).
- It is closable host-side, as proven above, so it does not block this
  fixture. A runtime fix would need a binding-instance component in the token,
  or a caller-seeded epoch.

**No voice-preserving swap gap.** `activate()` is a hard cut by design. Tails
can be preserved by rendering the retired bank out (proven). Held notes could
be re-triggered on the replacement with `publish`; that path is not exercised
here. Slot parameters change without
a cut via `set_slot_parameter` between blocks. None of these needs new API.
