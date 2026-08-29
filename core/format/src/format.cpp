// pulp-format: the Processor vtable anchor.
//
// Processor::create_view() is the first non-inline, non-pure virtual declared
// on Processor, which makes it the class's key function: this translation
// unit is where the compiler emits `vtable for Processor` and its typeinfo.
// Two consequences bind the target layout, and neither is optional:
//
//  1. This TU must live in pulp-format-core. A core library without the
//     Processor vtable cannot link anything that derives from Processor,
//     which is every plugin, plus pulp-sequence.
//  2. The emitted vtable holds slots pointing at Processor's other
//     out-of-line virtuals -- process_f64 (processor_f64.cpp) and
//     create_ara_document_controller (ara.cpp) -- so this object carries
//     undefined references to both. Those two files must therefore stay in
//     pulp-format-core alongside this one; classifying either into
//     pulp-format-view would leave the core vtable unresolvable.
//
// The body returns nullptr and never names a complete view::View, so
// processor.hpp's forward declaration is sufficient and no pulp/view header
// is included here. That is what keeps pulp-format-core free of the view
// layer.
//
// One upstream property this depends on: view::View's destructor is virtual
// (pulp/view/view.hpp). Destroying through unique_ptr<view::View> therefore
// dispatches through the vtable instead of naming ~View, so this TU carries no
// undefined view symbol. If ~View were ever made non-virtual, this file would
// grow a direct reference to it and pulp-format-core would stop linking
// without the view layer.
//
// The SettingsSection special members DO need view::View complete
// (SettingsSection owns a unique_ptr<view::View>), so they live in
// settings_section.cpp on the view side of the split.
#include <pulp/format/format.hpp>

namespace pulp::format {

std::unique_ptr<view::View> Processor::create_view() {
    return nullptr;
}

} // namespace pulp::format
