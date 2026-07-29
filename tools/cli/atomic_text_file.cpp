#include "atomic_text_file.hpp"

#include <pulp/runtime/detail/durable_file_replacement.hpp>

#include <cstdint>
#include <span>

namespace pulp::cli {

bool write_text_file_atomically(const std::filesystem::path& destination,
                                std::string_view contents,
                                std::string& error) noexcept {
    using pulp::runtime::detail::DurableFileCommitOutcome;
    using pulp::runtime::detail::DurableFileReplacement;

    auto replacement = DurableFileReplacement::create(destination);
    if (!replacement) {
        error = "could not create a temporary sibling for " + destination.string();
        return false;
    }

    const auto bytes = std::span{
        reinterpret_cast<const std::uint8_t*>(contents.data()), contents.size()};
    if (!replacement->write_all(bytes)) {
        replacement->cancel();
        error = "could not write the temporary sibling for " + destination.string();
        return false;
    }

    switch (replacement->commit()) {
    case DurableFileCommitOutcome::ReplacedDurably:
        return true;
    case DurableFileCommitOutcome::ReplacedButDirectorySyncFailed:
        error = "replaced " + destination.string() +
                " but could not durably sync its parent directory";
        return false;
    case DurableFileCommitOutcome::NotReplaced:
        error = "could not atomically replace " + destination.string();
        return false;
    }
    error = "unknown atomic replacement outcome for " + destination.string();
    return false;
}

} // namespace pulp::cli
