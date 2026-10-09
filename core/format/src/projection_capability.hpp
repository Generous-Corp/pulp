#pragma once

// Source-tree compatibility include. New consumers must include the installed
// public header. Keeping this shim avoids breaking the internal WASM/build
// helpers that historically referenced the source path.
#include <pulp/format/projection_capability.hpp>
