#pragma once

#include <pulp/format/editor_host.hpp>
#include <pulp/vellum/param_port_model.hpp>

#include <vellum/rmlui/assets.hpp>
#include <vellum/rmlui/native_parent_host.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulp::vellum::rmlui {

struct PackageAssets {
    std::string document;
    std::string stylesheet;
    std::vector<std::string> roots;
};

class AssetProvider final : public ::vellum::rmlui::AssetProvider {
  public:
    explicit AssetProvider(PackageAssets assets) : assets_(std::move(assets)) {}

    ::vellum::rmlui::Status load(std::string_view path,
                                 ::vellum::rmlui::Asset& out) override {
        if (path.empty() || path.find("..") != std::string_view::npos ||
            path.front() == '/' || path.find('\\') != std::string_view::npos)
            return ::vellum::rmlui::Status::missing_asset;
        const std::filesystem::path relative(path);
        std::vector<std::filesystem::path> candidates;
        if (relative.filename() == std::filesystem::path(assets_.document).filename())
            candidates.emplace_back(assets_.document);
        if (relative.filename() == std::filesystem::path(assets_.stylesheet).filename())
            candidates.emplace_back(assets_.stylesheet);
        for (const auto& root : assets_.roots)
            candidates.emplace_back(std::filesystem::path(root) / relative);
        for (const auto& candidate : candidates) {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(candidate, ec))
                continue;
            std::ifstream input(candidate, std::ios::binary);
            if (!input)
                return ::vellum::rmlui::Status::missing_asset;
            std::string bytes((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
            out.path = std::string(path);
            out.bytes = std::move(bytes);
            return ::vellum::rmlui::Status::ok;
        }
        return ::vellum::rmlui::Status::missing_asset;
    }

  private:
    PackageAssets assets_;
};

/// The RmlUi data-model field for a parameter: `p` then its decimal id
/// (`p1`). RmlUi variable names must be identifiers, so a bare id cannot be
/// one.
inline std::string param_field(std::uint32_t id) { return "p" + std::to_string(id); }

/// The parameter id a `param_field` names, or the field unchanged when it is
/// not one, so ParamPortModel can still resolve a parameter by name.
inline std::string_view param_key(std::string_view field) {
    if (field.size() > 1 && field.front() == 'p' &&
        field.find_first_not_of("0123456789", 1) == std::string_view::npos)
        return field.substr(1);
    return field;
}

class ModelHost final : public ::vellum::rmlui::ModelHost {
  public:
    ModelHost(format::ParamPort& port, ::vellum::rmlui::NativeParentHost& host)
        : model_(port), host_(host) {}

    ::vellum::rmlui::Status read(std::string_view field,
                                 ::vellum::rmlui::ModelValue& out) override {
        const auto handle = model_.resolve(param_key(field));
        if (!handle.valid())
            return ::vellum::rmlui::Status::rejected;
        out = ::vellum::rmlui::ModelValue::number_value(model_.normalized(handle));
        return ::vellum::rmlui::Status::ok;
    }

    ::vellum::rmlui::Status write(std::string_view field,
                                  const ::vellum::rmlui::ModelValue& value) override {
        if (value.kind != ::vellum::rmlui::ModelValue::Kind::number)
            return ::vellum::rmlui::Status::rejected;
        const auto handle = model_.resolve(param_key(field));
        if (!handle.valid())
            return ::vellum::rmlui::Status::rejected;
        model_.begin(handle);
        model_.set_normalized(handle, value.number);
        model_.end(handle);
        return ::vellum::rmlui::Status::ok;
    }

    ParamPortModel& model() noexcept { return model_; }

  private:
    ParamPortModel model_;
    ::vellum::rmlui::NativeParentHost& host_;
};

template <typename Assets> class Editor final : public format::Editor {
  public:
    Editor(format::EditorContext& context, Assets assets)
        : assets_(std::move(assets)), provider_(assets_), host_(provider_), model_host_(context.params(), host_) {
        model_subscription_ = context.params().add_listener([this](state::ParamID id, float) {
            host_.dirty_model("params", param_field(id));
        });
        host_.open(size_.width, size_.height);
        std::vector<std::string> fields;
        for (const auto& info : context.params().params())
            fields.push_back(param_field(info.id));
        host_.bind_model("params", std::shared_ptr<ModelHost>(&model_host_, [](ModelHost*) {}),
                         std::move(fields));
        host_.load_document(std::filesystem::path(assets_.document).filename().string());
    }

    ~Editor() override { detach(); }

    bool supports_parent(format::NativeParentKind kind) const override {
        return kind == format::NativeParentKind::none || kind == format::NativeParentKind::cocoa_view;
    }

    bool attach(const format::NativeParent& parent) override {
        if (!supports_parent(parent.kind))
            return false;
        if (parent.kind == format::NativeParentKind::none)
            return true;
        return host_.attach({::vellum::rmlui::NativeParentKind::cocoa_view, parent.handle});
    }

    void detach() override { host_.detach(); }
    void invalidate() override { host_.invalidate(); }

    format::EditorSize size() const override { return size_; }

    bool set_size(const format::EditorSize& size) override {
        if (size.width == 0 || size.height == 0 || size.scale <= 0.0)
            return false;
        size_ = size;
        return host_.set_size({size.width, size.height, size.scale});
    }

    format::ViewSize size_hints() const override {
        return {.preferred_width = size_.width, .preferred_height = size_.height,
                .min_width = 1, .min_height = 1};
    }

    void idle() override { host_.idle(); }
    void visibility_changed(bool visible) override { host_.visibility_changed(visible); }
    void focus_changed(bool focused) override { host_.focus_changed(focused); }

    ::vellum::rmlui::Status process_mouse_move(int x, int y) {
        return host_.process_mouse_move(x, y);
    }
    ::vellum::rmlui::Status process_mouse_button_down(int button) {
        return host_.process_mouse_button_down(button);
    }
    ::vellum::rmlui::Status process_mouse_button_up(int button) {
        return host_.process_mouse_button_up(button);
    }
    const ::vellum::rmlui::SoftRenderer* renderer() const noexcept {
        return host_.default_renderer();
    }

  private:
    Assets assets_;
    AssetProvider provider_;
    ::vellum::rmlui::NativeParentHost host_;
    ModelHost model_host_;
    format::ParamSubscription model_subscription_;
    format::EditorSize size_{400, 300, 1.0};
};

template <typename Assets> class EditorFactory final : public format::EditorFactory {
  public:
    explicit EditorFactory(Assets assets) : assets_(std::move(assets)) {}

    bool supports(format::EditorRole role) const override {
        return role == format::EditorRole::editor || role == format::EditorRole::inspector;
    }

    std::unique_ptr<format::Editor> create(format::Processor&, format::EditorContext& context,
                                           format::EditorRole role) override {
        return supports(role) ? std::make_unique<Editor<Assets>>(context, assets_) : nullptr;
    }

  private:
    Assets assets_;
};

template <typename Assets> format::EditorFactory* editor_factory(Assets assets) {
    static EditorFactory<Assets> factory(std::move(assets));
    return &factory;
}

} // namespace pulp::vellum::rmlui
