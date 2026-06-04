#include <pulp/view/design_import.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>

#include <memory>
#include <string_view>

namespace pulp::test::generated_binding_runtime {

std::unique_ptr<pulp::view::View> build_generated_binding_runtime_ui() {
    auto root = std::make_unique<pulp::view::View>();
    root->set_anchor_id("figma:root");
    root->flex().preferred_width = 120.0f;
    root->flex().preferred_height = 80.0f;

    auto checkbox = std::make_unique<pulp::view::Checkbox>();
    checkbox->set_anchor_id("figma:bypass-checkbox");
    checkbox->flex().preferred_width = 32.0f;
    checkbox->flex().preferred_height = 32.0f;
    checkbox->set_checked(false);
    root->add_child(std::move(checkbox));

    return root;
}

namespace {

pulp::view::View* find_imported_view_by_anchor(pulp::view::View& root,
                                               std::string_view anchor,
                                               int& matches) {
    pulp::view::View* first = nullptr;
    if (root.anchor_id() == anchor) {
        first = &root;
        ++matches;
    }
    for (std::size_t i = 0; i < root.child_count(); ++i) {
        if (auto* found = find_imported_view_by_anchor(*root.child_at(i), anchor, matches);
            first == nullptr) {
            first = found;
        }
    }
    return first;
}

}  // namespace

void bind_generated_binding_runtime_ui(pulp::view::View& root,
                                       pulp::view::NativeImportBindingContext& ctx) {
    int route_0_match_count = 0;
    if (auto* view = find_imported_view_by_anchor(root,
                                                  "figma:bypass-checkbox",
                                                  route_0_match_count);
        view != nullptr && route_0_match_count == 1 &&
        ctx.claim_import_binding(*view, "figma-plugin:bypass")) {
        if (auto* checkbox = dynamic_cast<pulp::view::Checkbox*>(view)) {
            ctx.bind_checkbox(*checkbox,
                              pulp::view::NativeImportBindingDescriptor{
                                  "figma-plugin:bypass",
                                  "filter.bypass",
                                  "filter",
                                  "bypass",
                                  "onChange:set_param:filter.bypass",
                                  "click:toggle"});
        }
    }
}

} // namespace pulp::test::generated_binding_runtime
