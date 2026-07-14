#pragma once

// Deprecated include path. The visitor moved to
// <pulp/host/native_handle_visitor.hpp> and is now spelled
// `pulp::host::NativeHandleVisitor` (its per-format structs are now
// `*NativeHandle`). Deprecated aliases for the old spellings live there, so
// code that included this header keeps compiling unchanged. New code should
// include the new path directly.

#include <pulp/host/native_handle_visitor.hpp>
