#if defined(PULP_VELLUM_HAS_SCRIPT_PREWARM)
#include <pulp/format/editor_prewarm.hpp>

#include <vellum/view/scripted_ui_prewarm.hpp>
#endif

namespace pulp::vellum::detail {
#if defined(PULP_VELLUM_HAS_SCRIPT_PREWARM)
namespace {

void schedule_editor_prewarm(const format::Processor::EditorPrewarm& request) {
    ::vellum::view::prewarm_scripted_ui({
        {request.scripts.begin(), request.scripts.end()},
        {request.materialized_documents.begin(), request.materialized_documents.end()},
    });
}

[[maybe_unused]] const bool scheduler_installed = [] {
    format::set_editor_prewarm_scheduler(&schedule_editor_prewarm);
    return true;
}();

} // namespace
#endif

void link_editor_prewarm() noexcept {}

} // namespace pulp::vellum::detail
