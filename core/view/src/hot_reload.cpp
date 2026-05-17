#include <pulp/view/hot_reload.hpp>
#include <fstream>
#include <sstream>
#include <system_error>

namespace pulp::view {

HotReloader::HotReloader(const std::filesystem::path& js_file, ReloadCallback on_reload)
    : watched_path_(js_file)
    , entry_file_(js_file.filename().string())
    , on_reload_(std::move(on_reload))
{
    seed_observed_write_times();

    auto dir = js_file.parent_path();
    watcher_ = std::make_unique<choc::file::Watcher>(
        dir,
        [this](const choc::file::Watcher::Event& event) {
            on_file_changed(event);
        },
        200 // check every 200ms
    );
}

HotReloader::HotReloader(const std::filesystem::path& directory,
                         const std::string& entry_file,
                         ReloadCallback on_reload)
    : watched_path_(directory)
    , entry_file_(entry_file)
    , on_reload_(std::move(on_reload))
{
    seed_observed_write_times();

    watcher_ = std::make_unique<choc::file::Watcher>(
        directory,
        [this](const choc::file::Watcher::Event& event) {
            on_file_changed(event);
        },
        200
    );
}

HotReloader::~HotReloader() = default;

bool HotReloader::poll_reload() {
    std::string code;
    {
        std::lock_guard lock(pending_mutex_);
        if (!has_pending_) return false;
        code = std::move(pending_code_);
        has_pending_ = false;
    }

    if (on_reload_ && !code.empty()) {
        on_reload_(code);
        reload_count_.fetch_add(1);
    }
    return true;
}

void HotReloader::on_file_changed(const choc::file::Watcher::Event& event) {
    // Only react to .js file modifications
    if (event.eventType != choc::file::Watcher::EventType::modified)
        return;

    auto ext = event.file.extension().string();
    if (ext != ".js" && ext != ".mjs")
        return;

    if (!should_reload_for_modified_file(event.file))
        return;

    // Read the entry file (not necessarily the changed file — could be an import)
    auto entry_path = watched_path_;
    if (std::filesystem::is_directory(watched_path_))
        entry_path = watched_path_ / entry_file_;

    auto code = read_file(entry_path);
    if (code.empty()) return;

    std::lock_guard lock(pending_mutex_);
    pending_code_ = std::move(code);
    has_pending_ = true;
}

std::string HotReloader::read_file(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) return {};
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

void HotReloader::seed_observed_write_times() {
    auto remember = [this](const std::filesystem::path& path) {
        const auto ext = path.extension().string();
        if (ext != ".js" && ext != ".mjs")
            return;

        std::error_code ec;
        const auto write_time = std::filesystem::last_write_time(path, ec);
        if (!ec)
            observed_write_times_[path.lexically_normal().string()] = write_time;
    };

    std::error_code ec;
    if (std::filesystem::is_directory(watched_path_, ec)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(
                 watched_path_,
                 std::filesystem::directory_options::skip_permission_denied,
                 ec)) {
            if (ec)
                break;
            if (entry.is_regular_file(ec))
                remember(entry.path());
            ec.clear();
        }
    } else {
        remember(watched_path_);
    }
}

bool HotReloader::should_reload_for_modified_file(const std::filesystem::path& path) {
    std::error_code ec;
    const auto write_time = std::filesystem::last_write_time(path, ec);
    if (ec)
        return false;

    const auto key = path.lexically_normal().string();
    auto it = observed_write_times_.find(key);
    if (it != observed_write_times_.end() && write_time <= it->second)
        return false;

    observed_write_times_[key] = write_time;
    return true;
}

} // namespace pulp::view
