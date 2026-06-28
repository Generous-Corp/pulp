#include <pulp/view/hot_reload.hpp>
#include <fstream>
#include <sstream>
#include <system_error>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if !defined(TARGET_OS_IPHONE)
#define TARGET_OS_IPHONE 0
#endif

namespace pulp::view {

// iOS: hot reload is a dev-time feature that depends on
// `choc::file::Watcher`, which uses macOS `FSEventStream*` APIs not
// available on iOS. Provide no-op constructors so `scripted_ui.cpp`
// (and any other caller that owns a `HotReloader` member) still links;
// the iOS AUv3 / HostApp paths always pass `enable_hot_reload = false`
// so the object never actually gets constructed there.
#if TARGET_OS_IPHONE

HotReloader::HotReloader(const std::filesystem::path& js_file, ReloadCallback on_reload)
    : watched_path_(js_file)
    , entry_file_(js_file.filename().string())
    , on_reload_(std::move(on_reload))
{}

HotReloader::HotReloader(const std::filesystem::path& directory,
                         const std::string& entry_file,
                         ReloadCallback on_reload)
    : watched_path_(directory)
    , entry_file_(entry_file)
    , on_reload_(std::move(on_reload))
{}

HotReloader::~HotReloader() = default;

#else  // !TARGET_OS_IPHONE

HotReloader::HotReloader(const std::filesystem::path& js_file, ReloadCallback on_reload)
    : watched_path_(js_file)
    , entry_file_(js_file.filename().string())
    , on_reload_(std::move(on_reload))
{
    seed_observed_content_hashes();

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
    seed_observed_content_hashes();

    watcher_ = std::make_unique<choc::file::Watcher>(
        directory,
        [this](const choc::file::Watcher::Event& event) {
            on_file_changed(event);
        },
        200
    );
}

HotReloader::~HotReloader() = default;

#endif  // TARGET_OS_IPHONE

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

#if !TARGET_OS_IPHONE
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
#endif  // !TARGET_OS_IPHONE

std::optional<std::string> HotReloader::try_read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return std::nullopt;
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

std::string HotReloader::read_file(const std::filesystem::path& path) {
    return try_read_file(path).value_or(std::string{});
}

void HotReloader::seed_observed_content_hashes() {
    auto remember = [this](const std::filesystem::path& path) {
        const auto ext = path.extension().string();
        if (ext != ".js" && ext != ".mjs")
            return;

        if (auto content = try_read_file(path)) {
            const auto key = path.lexically_normal().string();
            observed_content_hashes_[key] = content_hash(*content);
        }
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
    auto content = try_read_file(path);
    if (!content)
        return false;

    const auto key = path.lexically_normal().string();
    const auto next_hash = content_hash(*content);
    auto hash_it = observed_content_hashes_.find(key);
    if (hash_it != observed_content_hashes_.end() && next_hash == hash_it->second)
        return false;

    observed_content_hashes_[key] = next_hash;
    return true;
}

std::uint64_t HotReloader::content_hash(std::string_view content) {
    constexpr std::uint64_t offset = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;

    std::uint64_t hash = offset;
    for (unsigned char c : content) {
        hash ^= c;
        hash *= prime;
    }
    return hash;
}

} // namespace pulp::view
