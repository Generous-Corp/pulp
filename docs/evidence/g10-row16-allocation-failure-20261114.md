# G-10 row 16 allocation failure evidence, 2026-11-14

The dedicated `pulp-test-routed-spectral-allocation-failure` executable replaces
the process global allocation operators with a fixed-table allocator. The
failure window is armed only around control-thread
`PreparedTopologyEdit::prepare`; the SDK exported `rt-allocation-probe` is
unchanged.

The test first runs an unarmed calibration and then sweeps every allocation
ordinal through the candidate spectral prepared-topology edit. The calibration
covered 1,724 allocation points. Each injected failure returned a documented
typed refusal (`PreflightFailed` or `CompileFailed`), left the active graph
rendering bit-identical to an untouched control, and returned the window
allocation balance to its pre-call value after edit cleanup. A subsequent
unarmed prepare succeeded. The ctest has a 120 second timeout.

The test does not claim realtime-thread allocation behavior. It covers the
routed spectral candidate preparation transaction only, with one analysis to
synthesis graph and one identity stage edit. It does not exercise plugin or
custom-node preparation, parallel worker failure, or the SDK exported probe's
counting behavior.

The positive control is the successful unarmed calibration and retry. The
negative control was run with `tools/scripts/confirm_failure.sh` after removing
the `std::bad_alloc` to `PreflightFailed` mapping in
`core/host/src/signal_graph_prepared_topology_edit.cpp`; the allocation-failure
test failed, then passed after exact source restoration.
