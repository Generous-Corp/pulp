// pulp-format: the Processor vtable anchor.
//
// The destructor is the first non-inline, non-pure virtual on Processor. Keep
// The default view factory is the first non-inline virtual, so its definition
// anchors the Processor vtable in pulp-format-core without compiling against
// the view include tree. Returning nullptr only needs the forward declaration
// from processor.hpp, and keeping the symbol in core is required by core-only
// consumers whose derived vtables retain the slot.
#include <pulp/format/editor_package.hpp>
#include <pulp/format/format.hpp>
#if PULP_FORMAT_LEGACY_VIEW
#include <pulp/view/view.hpp>
#endif

namespace pulp::format {

#if !PULP_FORMAT_LEGACY_VIEW
} // namespace pulp::format

// See processor.hpp: the marker a macro-0 caller links against.
extern "C" const char pulp_format_core_was_built_with_PULP_FORMAT_LEGACY_VIEW_0 = 0;

namespace pulp::format {

// Completes the opaque stand-in for the reserved create_view() slot, which
// libstdc++ needs to instantiate ~unique_ptr<View> below. No one can construct
// one, so the reserved slot can only ever return null.
namespace view {
class View {
  public:
    View() = delete;
    virtual ~View() = default;
};
} // namespace view
#endif

Processor::~Processor() = default;

EditorFactory* Processor::editor_factory() {
    return plugin_editor_factory();
}

std::unique_ptr<view::View> Processor::PULP_FORMAT_LEGACY_VIEW_NAME(create_view)() {
    return nullptr;
}

} // namespace pulp::format
