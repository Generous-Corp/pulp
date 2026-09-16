#include <vellum/graphics/dawn_bootstrap.hpp>

#include <iostream>
#include <stdexcept>
#include <string>

#ifndef PULP_GPU_AUDIO_VELLUM_D15_PREFIX
#define PULP_GPU_AUDIO_VELLUM_D15_PREFIX ""
#endif

namespace {

using vellum::graphics::DawnBootstrap;
using vellum::graphics::DawnBootstrapRequest;
using vellum::graphics::DawnBootstrapResult;

bool require(bool condition, const char* message) {
    if (condition)
        return true;
    std::cerr << message << '\n';
    return false;
}

DawnBootstrapResult reject_identity(const DawnBootstrapRequest&, void*, std::string* error) {
    if (error != nullptr)
        *error = "test identity mismatch";
    return DawnBootstrapResult::identity_mismatch;
}

DawnBootstrapResult throw_exception(const DawnBootstrapRequest&, void*, std::string*) {
    throw std::runtime_error("test bootstrap exception");
}

struct ReentryState {
    bool rejected_reentry = false;
};

DawnBootstrapResult reject_reentry(const DawnBootstrapRequest& request, void* opaque,
                                   std::string* error) {
    auto& state = *static_cast<ReentryState*>(opaque);
    std::string nested_error;
    state.rejected_reentry = !vellum::graphics::register_dawn_bootstrap(
        {.abi_version = vellum::graphics::kDawnBootstrapAbiVersion,
         .callback = &reject_identity,
         .context = nullptr},
        request.expected_dawn_revision, &nested_error);
    if (!state.rejected_reentry) {
        if (error != nullptr)
            *error = "coordinator allowed re-entry";
        return DawnBootstrapResult::failed;
    }
    return DawnBootstrapResult::ready;
}

} // namespace

int main() {
    if (!require(PULP_GPU_AUDIO_VELLUM_D15_PREFIX[0] != '\0',
                 "D15 test was not compiled from an installed Vellum prefix")) {
        return 1;
    }

    std::string error;
    if (!require(!vellum::graphics::register_dawn_bootstrap(
                     {.abi_version = vellum::graphics::kDawnBootstrapAbiVersion,
                      .callback = &throw_exception,
                      .context = nullptr},
                     "pulp-d15-provider", &error) &&
                     error.find("threw") != std::string::npos,
                 "throwing host bootstrap did not fail closed and reset")) {
        return 1;
    }
    if (!require(!vellum::graphics::register_dawn_bootstrap(
                     {.abi_version = vellum::graphics::kDawnBootstrapAbiVersion,
                      .callback = &reject_identity,
                      .context = nullptr},
                     "pulp-d15-provider", &error) &&
                     error.find("identity") != std::string::npos,
                 "identity mismatch did not fail closed and reset")) {
        return 1;
    }

    ReentryState reentry;
    const DawnBootstrap bootstrap{
        .abi_version = vellum::graphics::kDawnBootstrapAbiVersion,
        .callback = &reject_reentry,
        .context = &reentry,
    };
    if (!require(vellum::graphics::register_dawn_bootstrap(
                     bootstrap, "pulp-d15-provider", &error) && reentry.rejected_reentry,
                 "coordinator did not reject re-entry before completing once-bootstrap")) {
        return 1;
    }
    if (!require(vellum::graphics::dawn_bootstrap_is_registered(&error),
                 "completed bootstrap was not observable")) {
        return 1;
    }
    if (!require(vellum::graphics::register_dawn_bootstrap(bootstrap, "pulp-d15-provider", &error),
                 "same provider registration was not idempotent")) {
        return 1;
    }
    return require(!vellum::graphics::register_dawn_bootstrap(
                       bootstrap, "pulp-d15-provider-mismatch", &error),
                   "different provider revision was accepted after bootstrap")
               ? 0
               : 1;
}
