#pragma once

#include "dawn_shared_io_provider.hpp"
#include "shared_io_convolution_session.hpp"

#include <cstdint>
#include <memory>
#include <span>

namespace pulp::gpu_audio::detail {

// Private, non-RT construction seam for the real Dawn provider path. It
// creates one provider, obtains that provider's paired prepared program, and
// transfers both to the session as one lifetime transaction. Public transport
// selection remains outside this experimental seam.
struct DawnSharedIoConvolutionSessionOptions {
    DawnSharedIoProvider::Options provider;
    SharedIoConvolutionSession::Config session;
    std::span<const float> normalized_ir_spectrum;
};

struct DawnSharedIoConvolutionSessionCreateResult {
    enum class Reason : std::uint8_t {
        Ready,
        ProviderUnavailable,
        ProgramConstructionFailed,
        SessionPreparationFailed,
        ConstructionException,
    };

    std::unique_ptr<SharedIoConvolutionSession> session;
    DawnSharedIoProvider::Availability availability = DawnSharedIoProvider::Availability::Failed;
    // On SessionPreparationFailed this remains non-null. Its arena may retain
    // live provider storage after a failed cleanup barrier, so the caller owns
    // retrying release() rather than permitting immediate destruction.
    Reason reason = Reason::ProviderUnavailable;
};

DawnSharedIoConvolutionSessionCreateResult
create_dawn_shared_io_convolution_session(const DawnSharedIoConvolutionSessionOptions&) noexcept;

} // namespace pulp::gpu_audio::detail
