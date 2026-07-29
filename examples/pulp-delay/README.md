# Pulp Delay

Pulp Delay is a stereo character delay built on `pulp::signal::CharacterDelay`.
Its processor exposes the complete 25-parameter contract used by the authored
1120×740 editor design. The native editor is intentionally kept separate from
the processor and timing policy; until it lands, Pulp's parameter-generated
editor remains available for functional testing.

The effect supports free and tempo-synchronised timing, linked or independent
right-channel timing, mono/stereo/ping-pong routing, four switchable character
engines, in-loop tone shaping, modulation, freeze, reverse, ducking and dry/wet
mixing.
