// Forced into a translation unit that must not name Pulp's view layer. Any
// `namespace pulp::view { ... }` or `pulp::view::X` after this line fails to
// compile, naming the namespace below in the error.
#pragma once

#if defined(PULP_TEST_REQUIRE_NO_VIEW_INCLUDE_PATH) && defined(__has_include)
#if __has_include(<pulp/view/view.hpp>)
#error "pulp/view/view.hpp is on this translation unit's include path"
#endif
#endif

namespace pulp_format_core_must_not_name_view {}
namespace pulp {
namespace view = ::pulp_format_core_must_not_name_view;
}
