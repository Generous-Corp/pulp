#pragma once
// The capability inference lives in runtime, so pack building in the format
// layer can use it without the view layer. The bridge reads it here under the
// view names.
#include <pulp/runtime/reload_autocaps.hpp>
#include <pulp/view/reload_capabilities.hpp>

namespace pulp::view {

using runtime::autocaps_function_table;
using runtime::infer_capabilities_from_js;
using runtime::infer_capability_names_from_js;

namespace detail {
using runtime::detail::contains_call_token;
using runtime::detail::is_js_ident_char;
} // namespace detail

} // namespace pulp::view
