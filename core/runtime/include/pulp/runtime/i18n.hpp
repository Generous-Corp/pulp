#pragma once

// Internationalization — string translation and locale management.
// Supports .strings and .po file formats, positional argument substitution.

#include <string>
#include <string_view>
#include <map>
#include <optional>
#include <vector>

namespace pulp::runtime {

/// Translation string lookup — maps keys to localized strings.
class LocalisedStrings {
public:
    LocalisedStrings() = default;

    /// Load translations from a .strings file (Apple format: "key" = "value";)
    bool load_strings_file(std::string_view path);

    /// Load translations from a .po file (gettext format)
    bool load_po_file(std::string_view path);

    /// Load translations from a JSON file ({"key": "value"})
    bool load_json_file(std::string_view path);

    /// Add a single translation
    void add(std::string_view key, std::string_view value);

    /// Look up a translation. Returns the key itself if not found.
    std::string translate(std::string_view key) const;

    /// Look up with positional argument substitution.
    /// Replaces {0}, {1}, {2}, ... with the provided arguments.
    std::string translate(std::string_view key, const std::vector<std::string>& args) const;

    /// Shorthand: t("key") == translate("key")
    std::string t(std::string_view key) const { return translate(key); }

    /// Whether a key has a translation
    bool has(std::string_view key) const;

    /// Number of translations loaded
    int count() const { return static_cast<int>(strings_.size()); }

    /// Clear all translations
    void clear() { strings_.clear(); }

    /// The locale identifier (e.g., "en", "de", "ja")
    const std::string& locale() const { return locale_; }
    void set_locale(std::string_view loc) { locale_ = std::string(loc); }

    // ── Global instance ─────────────────────────────────────────────────

    /// Get the global translation instance
    static LocalisedStrings& instance();

    /// Detect the system locale
    static std::string system_locale();

private:
    std::map<std::string, std::string, std::less<>> strings_;
    std::string locale_ = "en";
};

/// Convenience: translate a key using the global instance
inline std::string tr(std::string_view key) {
    return LocalisedStrings::instance().translate(key);
}

/// Convenience: translate with arguments
inline std::string tr(std::string_view key, const std::vector<std::string>& args) {
    return LocalisedStrings::instance().translate(key, args);
}

}  // namespace pulp::runtime
