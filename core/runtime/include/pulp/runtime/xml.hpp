#pragma once

// XML parse/generate wrapper over pugixml.
// Provides a simplified API for common XML operations.

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <functional>

// Forward-declare pugixml types to avoid header leak
namespace pugi { class xml_document; class xml_node; }

namespace pulp::runtime {

/// Parsed XML document (RAII wrapper over pugi::xml_document)
class XmlDocument {
public:
    XmlDocument();
    ~XmlDocument();

    /// Parse XML from a string. Returns false on failure.
    bool parse(std::string_view xml_string);

    /// Load XML from a file. Returns false on failure.
    bool load_file(std::string_view path);

    /// Save the document to a string.
    std::string to_string() const;

    /// Save the document to a file. Returns false on failure.
    bool save_file(std::string_view path) const;

    /// Get the root element name.
    std::string root_name() const;

    /// Get an attribute value from the root element.
    std::optional<std::string> root_attribute(std::string_view name) const;

    /// Find all elements matching an XPath expression.
    /// Returns their text content.
    std::vector<std::string> xpath_strings(std::string_view xpath) const;

    /// Get text content of the first element matching an XPath expression.
    std::optional<std::string> xpath_string(std::string_view xpath) const;

    /// Walk all elements, calling fn with (element_name, text_content).
    void walk(std::function<void(std::string_view name, std::string_view text)> fn) const;

    /// Whether the document was parsed successfully.
    bool is_valid() const { return valid_; }

    /// Last parse error message.
    const std::string& error() const { return error_; }

    // No copy, move OK
    XmlDocument(const XmlDocument&) = delete;
    XmlDocument& operator=(const XmlDocument&) = delete;
    XmlDocument(XmlDocument&&) noexcept;
    XmlDocument& operator=(XmlDocument&&) noexcept;

private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool valid_ = false;
    std::string error_;
};

/// Generate a simple XML string from key-value pairs.
/// Creates: <root><key1>value1</key1><key2>value2</key2></root>
std::string xml_generate(std::string_view root_name,
                         const std::vector<std::pair<std::string, std::string>>& elements);

}  // namespace pulp::runtime
