// In-tree tests compile the probe from here, beside their own sources; the
// implementation lives in tools/testing/rt_allocation_probe, which the SDK
// exports as Pulp::rt-allocation-probe. A binary links one copy or the other,
// never both: each replaces the global operator new.
#include "../../tools/testing/rt_allocation_probe/src/rt_allocation_probe.cpp"
