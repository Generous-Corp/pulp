#pragma once

// Deprecated include path. The type moved to <pulp/events/coalesced_updater.hpp>
// and is now spelled `CoalescedUpdater`, which says what it does. A deprecated
// alias for the old spelling lives there too, so code that included this header
// keeps compiling unchanged. New code should include the new path directly.
//
// NOTE: the virtual hook was renamed too (`handle_async_update()` → `on_update()`).
// That one cannot be aliased — a subclass overriding the old name would silently
// never be called, which is worse than a compile error. If you subclassed this,
// rename your override; the compiler will tell you exactly where.

#include <pulp/events/coalesced_updater.hpp>
