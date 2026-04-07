#include <pulp/runtime/xml.hpp>
#include <pugixml.hpp>
#include <sstream>

namespace pulp::runtime {

struct XmlDocument::Impl {
    pugi::xml_document doc;
};

XmlDocument::XmlDocument() : impl_(new Impl) {}
XmlDocument::~XmlDocument() { delete impl_; }

XmlDocument::XmlDocument(XmlDocument&& other) noexcept
    : impl_(other.impl_), valid_(other.valid_), error_(std::move(other.error_)) {
    other.impl_ = new Impl;
    other.valid_ = false;
}

XmlDocument& XmlDocument::operator=(XmlDocument&& other) noexcept {
    if (this != &other) {
        delete impl_;
        impl_ = other.impl_;
        valid_ = other.valid_;
        error_ = std::move(other.error_);
        other.impl_ = new Impl;
        other.valid_ = false;
    }
    return *this;
}

bool XmlDocument::parse(std::string_view xml_string) {
    auto result = impl_->doc.load_buffer(xml_string.data(), xml_string.size());
    valid_ = result;
    if (!valid_)
        error_ = result.description();
    else
        error_.clear();
    return valid_;
}

bool XmlDocument::load_file(std::string_view path) {
    std::string path_str(path);
    auto result = impl_->doc.load_file(path_str.c_str());
    valid_ = result;
    if (!valid_)
        error_ = result.description();
    else
        error_.clear();
    return valid_;
}

std::string XmlDocument::to_string() const {
    std::ostringstream ss;
    impl_->doc.save(ss, "  ");
    return ss.str();
}

bool XmlDocument::save_file(std::string_view path) const {
    std::string path_str(path);
    return impl_->doc.save_file(path_str.c_str(), "  ");
}

std::string XmlDocument::root_name() const {
    auto root = impl_->doc.document_element();
    return root ? root.name() : "";
}

std::optional<std::string> XmlDocument::root_attribute(std::string_view name) const {
    auto root = impl_->doc.document_element();
    if (!root) return std::nullopt;
    std::string name_str(name);
    auto attr = root.attribute(name_str.c_str());
    if (!attr) return std::nullopt;
    return std::string(attr.value());
}

std::vector<std::string> XmlDocument::xpath_strings(std::string_view xpath) const {
    std::vector<std::string> results;
    std::string xpath_str(xpath);
    try {
        auto nodes = impl_->doc.select_nodes(xpath_str.c_str());
        for (auto& node : nodes) {
            if (node.node())
                results.push_back(node.node().text().get());
            else if (node.attribute())
                results.push_back(node.attribute().value());
        }
    } catch (...) {
        // Invalid XPath
    }
    return results;
}

std::optional<std::string> XmlDocument::xpath_string(std::string_view xpath) const {
    auto results = xpath_strings(xpath);
    if (results.empty()) return std::nullopt;
    return results[0];
}

void XmlDocument::walk(std::function<void(std::string_view, std::string_view)> fn) const {
    struct Walker : pugi::xml_tree_walker {
        std::function<void(std::string_view, std::string_view)>& callback;
        Walker(std::function<void(std::string_view, std::string_view)>& cb) : callback(cb) {}
        bool for_each(pugi::xml_node& node) override {
            if (node.type() == pugi::node_element)
                callback(node.name(), node.text().get());
            return true;
        }
    };
    Walker walker(fn);
    impl_->doc.traverse(walker);
}

std::string xml_generate(std::string_view root_name,
                         const std::vector<std::pair<std::string, std::string>>& elements) {
    pugi::xml_document doc;
    auto root = doc.append_child(std::string(root_name).c_str());
    for (auto& [key, value] : elements) {
        auto child = root.append_child(key.c_str());
        child.text().set(value.c_str());
    }
    std::ostringstream ss;
    doc.save(ss, "  ");
    return ss.str();
}

}  // namespace pulp::runtime
