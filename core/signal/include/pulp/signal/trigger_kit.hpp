#pragma once

/// Compatibility spelling for the event-domain toolkit. The canonical
/// definitions live in `trigger.hpp`; keeping this header as a pure include
/// makes include order immaterial and prevents parallel class definitions from
/// drifting apart again.
#include <pulp/signal/trigger.hpp>
