# Static stereo placement through a subgraph

This installed SDK example places a mono signal at fixed left and right levels.
It uses only G-4's public `SubgraphTemplate` API: one mono Input, two Gain nodes,
and one two-channel Output. The template exposes `mono_in`, `left_out`, and
`right_out` port handles plus instance-local `left_gain` and `right_gain`
parameters. A second silent instance checks that restoring the first does not
change the second's gain targets.

From an installed Pulp SDK prefix, configure this directory with
`-DCMAKE_PREFIX_PATH=<prefix>`, build its executable, and run `ctest` in the
example build directory. The executable renders a unit impulse at frame 8. Its
positive oracle requires left/right values `0.75/0.25`, then `0/1` after state
restore, with silence at every other frame. A planted left/right connection
swap must fail the same channel oracle. An out-of-range restore must leave both
gain values unchanged.

This recipe proves fixed mono-to-stereo channel weighting in an offline host.
Named exposed ports are graph handles; they do not negotiate named host buses.
The example does not claim azimuth, HRTF, sample-accurate events, realtime
allocation behavior, baked artifact parity, tuning, or live sampling.
