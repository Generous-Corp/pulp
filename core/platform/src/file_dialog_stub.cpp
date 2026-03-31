#include <pulp/platform/file_dialog.hpp>

#if !defined(__APPLE__)

namespace pulp::platform {

std::optional<std::string> FileDialog::open_file(const std::string&, const std::vector<FileFilter>&, const std::string&) {
    return std::nullopt;
}

std::vector<std::string> FileDialog::open_files(const std::string&, const std::vector<FileFilter>&, const std::string&) {
    return {};
}

std::optional<std::string> FileDialog::save_file(
    const std::string&, const std::vector<FileFilter>&, const std::string&, const std::string&) {
    return std::nullopt;
}

std::optional<std::string> FileDialog::choose_folder(const std::string&, const std::string&) {
    return std::nullopt;
}

} // namespace pulp::platform

#endif
