// FileChooser — see core/view/include/pulp/view/file_chooser.hpp.
//
// macOS plan item 6.2. Thin facade over
// `pulp::platform::FileDialog` that turns the synchronous
// `optional<string>` / `vector<string>` returns into a callback the
// caller can attach without thinking about the return type, and that
// keeps a single extension point for a future non-modal backend.

#include <pulp/view/file_chooser.hpp>

#include <utility>

namespace pulp::view {

namespace {

void deliver(const FileChooser::ResultCallback& callback,
             std::vector<std::string> paths) {
    if (callback) {
        callback(std::move(paths));
    }
}

} // namespace

FileChooser& FileChooser::set_title(std::string title) {
    title_ = std::move(title);
    return *this;
}

FileChooser& FileChooser::set_initial_directory(std::string path) {
    initial_dir_ = std::move(path);
    return *this;
}

FileChooser& FileChooser::set_default_name(std::string name) {
    default_name_ = std::move(name);
    return *this;
}

FileChooser& FileChooser::add_filter(pulp::platform::FileFilter filter) {
    filters_.push_back(std::move(filter));
    return *this;
}

FileChooser& FileChooser::add_extension_filter(std::string description,
                                               std::string extensions) {
    filters_.push_back({std::move(description), std::move(extensions)});
    return *this;
}

FileChooser& FileChooser::clear_filters() {
    filters_.clear();
    return *this;
}

FileChooser& FileChooser::set_allow_multiple(bool allow) {
    allow_multiple_ = allow;
    return *this;
}

void FileChooser::open(ResultCallback callback) const {
    const std::string& effective_title = title_.empty() ? std::string{"Open"} : title_;

    if (allow_multiple_) {
        auto paths = pulp::platform::FileDialog::open_files(
            effective_title, filters_, initial_dir_);
        deliver(callback, std::move(paths));
        return;
    }

    auto picked = pulp::platform::FileDialog::open_file(
        effective_title, filters_, initial_dir_);
    std::vector<std::string> result;
    if (picked) {
        result.push_back(std::move(*picked));
    }
    deliver(callback, std::move(result));
}

void FileChooser::save(ResultCallback callback) const {
    const std::string& effective_title = title_.empty() ? std::string{"Save"} : title_;

    auto picked = pulp::platform::FileDialog::save_file(
        effective_title, filters_, initial_dir_, default_name_);
    std::vector<std::string> result;
    if (picked) {
        result.push_back(std::move(*picked));
    }
    deliver(callback, std::move(result));
}

void FileChooser::choose_folder(ResultCallback callback) const {
    const std::string& effective_title =
        title_.empty() ? std::string{"Choose Folder"} : title_;

    auto picked = pulp::platform::FileDialog::choose_folder(
        effective_title, initial_dir_);
    std::vector<std::string> result;
    if (picked) {
        result.push_back(std::move(*picked));
    }
    deliver(callback, std::move(result));
}

} // namespace pulp::view
