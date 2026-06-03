#include <pulp/native_components/native_ui.h>
#include <pulp/view/design_import.hpp>
#include <pulp/view/layout_snapshot.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/window_host.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#if defined(__unix__)
#include <unistd.h>
#endif

extern "C" const pulp_native_ui_provider_v1* pulp_native_ui_entry_v1(void);

static_assert(std::is_standard_layout_v<pulp_native_ui_provider_request_v1>);
static_assert(std::is_standard_layout_v<pulp_native_ui_provider_result_v1>);
static_assert(std::is_standard_layout_v<pulp_native_ui_provider_v1>);

namespace {

std::filesystem::path asset_dir() {
    if (const char* env = std::getenv("PULP_ELYSIUM_RUIF_ASSET_DIR")) {
        if (*env != '\0')
            return env;
    }
    return PULP_ELYSIUM_RUIF_ASSET_DIR;
}

bool set_working_directory(const std::filesystem::path& dir) {
#if defined(__unix__)
    return ::chdir(dir.c_str()) == 0;
#else
    std::error_code ec;
    std::filesystem::current_path(dir, ec);
    return !ec;
#endif
}

bool write_png(const std::filesystem::path& path, const std::vector<uint8_t>& png) {
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open())
        return false;
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    return out.good();
}

bool write_text(const std::filesystem::path& path, std::string_view text) {
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open())
        return false;
    out << text;
    return out.good();
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string span_to_string(pulp_native_ui_byte_span_v1 span) {
    if (span.bytes == nullptr || span.byte_len == 0)
        return {};
    return {reinterpret_cast<const char*>(span.bytes), span.byte_len};
}

bool native_ui_provider_compatible(const pulp_native_ui_provider_v1* provider) {
    return provider != nullptr
        && provider->abi_version == PULP_NATIVE_UI_ABI_VERSION
        && provider->size >= offsetof(pulp_native_ui_provider_v1, free_result) + sizeof(provider->free_result)
        && provider->provider_id != nullptr
        && provider->provider_version != nullptr
        && provider->import_design != nullptr
        && provider->free_result != nullptr;
}

class NativeUiResultGuard {
public:
    explicit NativeUiResultGuard(const pulp_native_ui_provider_v1* provider)
        : provider_(provider) {}

    ~NativeUiResultGuard() {
        if (provider_ != nullptr && provider_->free_result != nullptr)
            provider_->free_result(&result_);
    }

    pulp_native_ui_provider_result_v1* out() { return &result_; }
    const pulp_native_ui_provider_result_v1& result() const { return result_; }

private:
    const pulp_native_ui_provider_v1* provider_ = nullptr;
    pulp_native_ui_provider_result_v1 result_{};
};

class RustNativeUiProvider final : public pulp::view::NativeDesignProvider {
public:
    explicit RustNativeUiProvider(const pulp_native_ui_provider_v1* provider)
        : provider_(provider) {}

    std::string provider_id() const override {
        return provider_ != nullptr ? span_to_string(provider_->provider_id()) : std::string{};
    }

    std::string provider_version() const override {
        return provider_ != nullptr ? span_to_string(provider_->provider_version()) : std::string{};
    }

    pulp::view::NativeDesignProviderOutput import_design(
        const pulp::view::NativeDesignProviderRequest& request) override {
        pulp::view::NativeDesignProviderOutput output;
        if (!native_ui_provider_compatible(provider_)) {
            output.ok = false;
            output.failure_reason = "Rust native UI provider vtable is incompatible";
            return output;
        }

        pulp_native_ui_provider_request_v1 abi_request{
            .size = sizeof(pulp_native_ui_provider_request_v1),
            .abi_version = PULP_NATIVE_UI_ABI_VERSION,
            .canonical_source_design_ir_json = {
                reinterpret_cast<const uint8_t*>(request.canonical_source_design_ir_json.data()),
                request.canonical_source_design_ir_json.size()},
            .source_design_hash = {
                reinterpret_cast<const uint8_t*>(request.source_design_hash.data()),
                request.source_design_hash.size()},
            .strict_mode = request.strict_mode ? 1u : 0u,
            .reserved = 0};

        NativeUiResultGuard guard(provider_);
        const auto status = provider_->import_design(&abi_request, guard.out());
        const auto& result = guard.result();
        output.ok = status == PULP_NATIVE_UI_OK && result.status == PULP_NATIVE_UI_OK;
        output.canonical_design_ir_json = span_to_string(result.canonical_design_ir_json);
        output.provenance.provider_id = span_to_string(result.provider_id);
        output.provenance.provider_version = span_to_string(result.provider_version);
        output.provenance.source_design_hash = span_to_string(result.source_design_hash);
        output.failure_reason = span_to_string(result.failure_reason);
        if (!output.ok && output.failure_reason.empty())
            output.failure_reason = "Rust native UI provider import failed";
        return output;
    }

private:
    const pulp_native_ui_provider_v1* provider_ = nullptr;
};

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path screenshot_path;
    std::filesystem::path layout_path;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        constexpr std::string_view screenshot_prefix = "--screenshot=";
        constexpr std::string_view layout_prefix = "--layout=";
        if (arg.rfind(screenshot_prefix, 0) == 0) {
            screenshot_path = arg.substr(screenshot_prefix.size());
        } else if (arg.rfind(layout_prefix, 0) == 0) {
            layout_path = arg.substr(layout_prefix.size());
        }
    }
    if (!screenshot_path.empty())
        screenshot_path = std::filesystem::absolute(screenshot_path);
    if (!layout_path.empty())
        layout_path = std::filesystem::absolute(layout_path);

    const auto assets = asset_dir();
    if (!std::filesystem::exists(assets / "assets")) {
        std::cerr << "ELYSIUM assets not found at " << (assets / "assets") << "\n";
        return 1;
    }
    if (!set_working_directory(assets)) {
        std::cerr << "failed to set ELYSIUM asset working directory: " << assets << "\n";
        return 1;
    }

    const auto source_json = read_text(
        std::filesystem::path(PULP_ELYSIUM_RUIF_DESIGNIR_JSON));
    if (source_json.empty()) {
        std::cerr << "failed to read ELYSIUM DesignIR fixture\n";
        return 1;
    }
    auto ir = pulp::view::parse_design_ir_json(source_json);

    pulp::view::NativeDesignProviderRegistry registry;
    auto rust_provider = std::make_shared<RustNativeUiProvider>(pulp_native_ui_entry_v1());
    if (!registry.register_provider(rust_provider)) {
        std::cerr << "failed to register Rust native UI provider\n";
        return 1;
    }

    std::vector<pulp::view::ImportDiagnostic> diagnostics;
    pulp::view::NativeDesignProviderAttempt attempt;
    auto root = pulp::view::build_native_view_tree_with_provider(
        ir,
        ir.asset_manifest,
        {.diagnostics_out = &diagnostics},
        {.mode = pulp::view::NativeDesignProviderMode::provider_strict,
         .provider_id = "test.rust.elysium",
         .registry = &registry,
         .source_design_hash = "elysium-fixture",
         .attempt_out = &attempt,
         // ELYSIUM currently repeats source stable_anchor_id values. This is a
         // fixture compatibility escape only; provider anchors remain strict.
         .reject_duplicate_source_anchors = false});

    if (!root) {
        std::cerr << "failed to build ELYSIUM Rust-provider UI";
        if (!attempt.failure_reason.empty())
            std::cerr << ": " << attempt.failure_reason;
        std::cerr << "\n";
        return 1;
    }
    if (!attempt.provider_attempted || attempt.fallback_used) {
        std::cerr << "Rust provider was not used strictly\n";
        return 1;
    }

    root->set_requires_gpu_host(true);
    root->set_bounds({0.0f, 0.0f, 1000.0f, 600.0f});
    root->layout_children();

    if (!layout_path.empty()) {
        const auto layout = pulp::view::dump_layout_tree(
            *root,
            {.surface = "ruif-standalone",
             .fixture = "elysium-rust-provider",
             .viewport_width = 1000.0f,
             .viewport_height = 600.0f});
        if (!write_text(layout_path, layout)) {
            std::cerr << "failed to write ELYSIUM Rust-provider layout snapshot: "
                      << layout_path << "\n";
            return 1;
        }
    }

    pulp::view::WindowOptions options;
    options.title = "Pulp ELYSIUM RUIF C++ Baseline";
    options.width = 1000.0f;
    options.height = 600.0f;
    options.min_width = 667.0f;
    options.min_height = 400.0f;
    options.resizable = true;
    options.use_gpu = true;
    options.initially_hidden = !screenshot_path.empty();

    auto window = pulp::view::WindowHost::create(*root, options);
    if (!window) {
        std::cerr << "failed to create ELYSIUM Rust-provider GPU window host\n";
        return 1;
    }
    window->set_design_viewport(1000.0f, 600.0f);
    window->set_fixed_aspect_ratio(1000.0f / 600.0f);
    window->set_close_callback([] {});

    if (!screenshot_path.empty()) {
        auto png = window->capture_back_buffer_png();
        if (png.empty() || !write_png(screenshot_path, png)) {
            std::cerr << "failed to capture ELYSIUM Rust-provider GPU screenshot: "
                      << screenshot_path << "\n";
            return 1;
        }
        return 0;
    }

    window->run_event_loop();
    return 0;
}
