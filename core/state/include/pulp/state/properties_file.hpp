#pragma once

// PropertiesFile — JSON-backed persistent key-value settings.
// ApplicationProperties — user + common properties with platform-standard paths.

#include <string>
#include <string_view>
#include <optional>
#include <vector>
#include <map>
#include <cstdint>

namespace pulp::state {

/// JSON-backed persistent settings file
class PropertiesFile {
public:
    PropertiesFile() = default;

    /// Load properties from a JSON file. Returns false if file doesn't exist or is invalid.
    bool load(std::string_view path);

    /// Save properties to a JSON file. Stores the path for subsequent save() calls.
    bool save(std::string_view path);

    /// Save to the path it was loaded from (or last saved to).
    bool save() const;

    /// Get/set string properties
    std::optional<std::string> get_string(std::string_view key) const;
    void set_string(std::string_view key, std::string_view value);

    /// Get/set integer properties
    std::optional<int64_t> get_int(std::string_view key) const;
    void set_int(std::string_view key, int64_t value);

    /// Get/set float properties
    std::optional<double> get_double(std::string_view key) const;
    void set_double(std::string_view key, double value);

    /// Get/set boolean properties
    std::optional<bool> get_bool(std::string_view key) const;
    void set_bool(std::string_view key, bool value);

    /// Remove a property
    void remove(std::string_view key);

    /// Whether a key exists
    bool contains(std::string_view key) const;

    /// Clear all properties
    void clear();

    /// Number of properties
    int size() const { return static_cast<int>(values_.size()); }

    /// Get all keys
    std::vector<std::string> keys() const;

    /// The file path (set after load or explicit set)
    const std::string& path() const { return path_; }
    void set_path(std::string_view p) { path_ = std::string(p); }

private:
    bool save_to(const std::string& dest) const;
    std::map<std::string, std::string, std::less<>> values_;
    std::string path_;
};

/// Application-level properties with platform-standard paths.
/// Provides user-specific and shared settings.
class ApplicationProperties {
public:
    /// Initialize with application name (determines storage path)
    explicit ApplicationProperties(std::string_view app_name);

    /// User-specific settings (e.g., ~/Library/Preferences/AppName.json on macOS)
    PropertiesFile& user_settings() { return user_; }
    const PropertiesFile& user_settings() const { return user_; }

    /// Common/shared settings (e.g., /Library/Application Support/AppName/settings.json)
    PropertiesFile& common_settings() { return common_; }
    const PropertiesFile& common_settings() const { return common_; }

    /// Load both user and common settings from their platform paths
    void load();

    /// Save both settings files
    void save();

    /// Application name
    const std::string& app_name() const { return app_name_; }

    /// Platform-specific user settings directory
    static std::string user_settings_dir(std::string_view app_name);

    /// Platform-specific common settings directory
    static std::string common_settings_dir(std::string_view app_name);

private:
    std::string app_name_;
    PropertiesFile user_;
    PropertiesFile common_;
};

}  // namespace pulp::state
