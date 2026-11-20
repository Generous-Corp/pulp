#pragma once

/// @file
/// `pulp::vellum::EditorEnv`: what a plug-in's root Vellum view receives.
///
/// Kept apart from editor.hpp so a root view, and a headless test or capture
/// of it, compiles without Vellum's plug-in view host.

#include <pulp/format/editor_host.hpp>
#include <pulp/vellum/param_port_model.hpp>

#include <vellum/value/value_feed.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace pulp::vellum {

struct EditorEnv {
    EditorEnv(format::EditorContext& context, ParamPortModel& params,
              ::vellum::value::ValueFeed* visuals = nullptr)
        : context(context), params(params), visuals(visuals) {}

    format::EditorContext& context;
    ParamPortModel& params;
    ::vellum::value::ValueFeed* visuals = nullptr;

    /// Ask the host to resize. The host applies any accepted size via Editor::set_size().
    bool request_resize(const format::EditorSize& size) const {
        return lifetime_ && lifetime_->open && context.request_resize(size);
    }

    using NativeMessageHandler = std::function<std::string(std::string_view)>;

    /// Pass the returned handler to ScriptedUiSession::attach_native_message_handler().
    /// It accepts calls before the first attach and until this attachment closes.
    /// Register again after reattaching. Detach the handler before destroying its captures.
    NativeMessageHandler native_message_handler(NativeMessageHandler handler) const {
        return [lifetime = std::weak_ptr(lifetime_), handler = std::move(handler)](
                   std::string_view json) -> std::string {
            const auto attachment = lifetime.lock();
            if (!attachment || !attachment->open || !handler)
                return R"({"ok":false,"error":"editor message handler unavailable"})";
            return handler(json);
        };
    }

  private:
    template <typename> friend class Editor;

    // Owner-thread callbacks keep a weak identity for this attachment. A
    // reopened editor starts a fresh identity, even if its root is reused.
    struct Attachment {
        bool open = true;
    };
    std::shared_ptr<Attachment> lifetime_ = std::make_shared<Attachment>();
};

} // namespace pulp::vellum
