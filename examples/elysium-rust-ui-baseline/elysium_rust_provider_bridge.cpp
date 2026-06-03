#include "elysium_rust_provider_bridge.hpp"

#include <pulp/native_components/native_ui.h>
#include <pulp/runtime/log.hpp>
#include <pulp/view/design_import.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

extern "C" const pulp_native_ui_provider_v1* pulp_native_ui_entry_v1(void);

static_assert(std::is_standard_layout_v<pulp_native_ui_provider_request_v1>);
static_assert(std::is_standard_layout_v<pulp_native_ui_provider_result_v1>);
static_assert(std::is_standard_layout_v<pulp_native_ui_provider_v1>);

namespace pulp::examples {
namespace {

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
        && provider->size >= offsetof(pulp_native_ui_provider_v1, free_result)
                                 + sizeof(provider->free_result)
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

class RustNativeUiProvider final : public view::NativeDesignProvider {
public:
    explicit RustNativeUiProvider(const pulp_native_ui_provider_v1* provider)
        : provider_(provider) {}

    std::string provider_id() const override {
        return provider_ != nullptr ? span_to_string(provider_->provider_id()) : std::string{};
    }

    std::string provider_version() const override {
        return provider_ != nullptr ? span_to_string(provider_->provider_version()) : std::string{};
    }

    view::NativeDesignProviderOutput import_design(
        const view::NativeDesignProviderRequest& request) override {
        view::NativeDesignProviderOutput output;
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

std::unique_ptr<view::View> build_elysium_rust_provider_ui(std::string* failure_reason) {
    const auto source_json = read_text(std::filesystem::path(PULP_ELYSIUM_RUIF_DESIGNIR_JSON));
    if (source_json.empty()) {
        if (failure_reason != nullptr)
            *failure_reason = "failed to read ELYSIUM DesignIR fixture";
        return nullptr;
    }

    auto ir = view::parse_design_ir_json(source_json);

    view::NativeDesignProviderRegistry registry;
    auto rust_provider = std::make_shared<RustNativeUiProvider>(pulp_native_ui_entry_v1());
    if (!registry.register_provider(rust_provider)) {
        if (failure_reason != nullptr)
            *failure_reason = "failed to register Rust native UI provider";
        return nullptr;
    }

    std::vector<view::ImportDiagnostic> diagnostics;
    view::NativeDesignProviderAttempt attempt;
    auto root = view::build_native_view_tree_with_provider(
        ir,
        ir.asset_manifest,
        {.diagnostics_out = &diagnostics},
        {.mode = view::NativeDesignProviderMode::provider_strict,
         .provider_id = "test.rust.elysium",
         .registry = &registry,
         .source_design_hash = "elysium-fixture",
         .attempt_out = &attempt,
         // ELYSIUM currently repeats source stable_anchor_id values. This is a
         // fixture compatibility escape only; provider anchors remain strict.
         .reject_duplicate_source_anchors = false});

    if (!root) {
        if (failure_reason != nullptr) {
            *failure_reason = "failed to build ELYSIUM Rust-provider UI";
            if (!attempt.failure_reason.empty())
                *failure_reason += ": " + attempt.failure_reason;
        }
        return nullptr;
    }
    if (!attempt.provider_attempted || attempt.fallback_used) {
        if (failure_reason != nullptr)
            *failure_reason = "Rust provider was not used strictly";
        return nullptr;
    }

    runtime::log_info(
        "RUIF Rust provider used: provider_id={} provider_strict=true fallback=false",
        attempt.provider_id);

    root->set_requires_gpu_host(true);
    return root;
}

}  // namespace pulp::examples
