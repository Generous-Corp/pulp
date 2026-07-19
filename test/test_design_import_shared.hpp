#pragma once

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/anchor_strategy.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/design_sources.hpp>
#include <pulp/view/design_codegen.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/widget_bridge.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace pulp::view;
namespace fs = std::filesystem;

namespace {

#ifndef PULP_REPO_ROOT
#define PULP_REPO_ROOT "."
#endif

std::string read_fixture(const std::string& rel_path) {
    const auto root = std::string(PULP_REPO_ROOT);
    std::vector<std::string> candidates{root + "/" + rel_path};
    constexpr std::string_view planning_prefix{"planning/fixtures/"};
    if (rel_path.rfind(std::string(planning_prefix), 0) == 0) {
        candidates.push_back(root + "/test/fixtures/" + rel_path.substr(planning_prefix.size()));
    }

    std::ostringstream tried;
    for (const auto& path : candidates) {
        if (tried.tellp() > 0) {
            tried << ", ";
        }
        tried << path;

        std::ifstream in(path, std::ios::binary);
        if (!in.good()) {
            continue;
        }

        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    FAIL("reading fixture failed; tried " << tried.str());
    return {};
}

std::size_t count_occurrences(std::string_view haystack, std::string_view needle) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string_view::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

std::string asset_text(const ClaudeBundleAsset& asset) {
    return std::string(asset.data.begin(), asset.data.end());
}

class TempDir {
public:
    explicit TempDir(const std::string& prefix) {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / (prefix + "-" + std::to_string(tick));
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    fs::path path;
};

void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    REQUIRE(f.is_open());
    f << text;
    REQUIRE(f.good());
}

std::optional<std::string> read_env_var(const char* name) {
    if (const char* value = std::getenv(name); value) return std::string(value);
    return std::nullopt;
}

void set_env_var(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void unset_env_var(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

class ScopedEnvVar {
public:
    ScopedEnvVar(const char* name, const std::string& value)
        : name_(name), old_(read_env_var(name)) {
        set_env_var(name_.c_str(), value);
    }

    ~ScopedEnvVar() {
        if (old_) set_env_var(name_.c_str(), *old_);
        else unset_env_var(name_.c_str());
    }

    ScopedEnvVar(const ScopedEnvVar&) = delete;
    ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

private:
    std::string name_;
    std::optional<std::string> old_;
};

bool has_diagnostic(const IRAssetRef& asset, const std::string& code) {
    for (const auto& diagnostic : asset.diagnostics) {
        if (diagnostic.code == code) return true;
    }
    return false;
}

// Joined diagnostic codes for INFO() context on has_diagnostic assertions.
// When a network-asset case flakes in a full CI run, "missed asset-empty"
// alone cannot distinguish a cache hit from a fetcher-resolution failure
// from a timeout reclassification — the codes that WERE present can.
std::string diagnostic_codes(const IRAssetRef& asset) {
    std::string codes;
    for (const auto& diagnostic : asset.diagnostics) {
        if (!codes.empty()) codes += ", ";
        codes += diagnostic.code;
    }
    return codes.empty() ? std::string("<none>") : codes;
}

bool has_import_diagnostic(const std::vector<ImportDiagnostic>& diagnostics,
                           const std::string& code) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.code == code) return true;
    }
    return false;
}

const IRNode* find_descendant(const IRNode& node,
                              const std::function<bool(const IRNode&)>& pred) {
    if (pred(node)) return &node;
    for (const auto& child : node.children) {
        if (const auto* found = find_descendant(child, pred)) return found;
    }
    return nullptr;
}

const char* minimal_host_react_dom_shim() {
    return R"JS(
(function(){
  function flatten(input, out) {
    if (input == null || input === false || input === true) return;
    if (Array.isArray(input)) {
      for (var i = 0; i < input.length; i++) flatten(input[i], out);
      return;
    }
    out.push(input);
  }

  function createElement(type, props) {
    var children = [];
    for (var i = 2; i < arguments.length; i++) flatten(arguments[i], children);
    return { type: type, props: props || {}, children: children };
  }

  function cssValue(key, value) {
    if (value == null) return "";
    if (typeof value === "number") {
      if (key === "flexGrow" || key === "flexShrink" || key === "opacity" ||
          key === "zIndex" || key === "lineHeight") {
        return String(value);
      }
      return String(value) + "px";
    }
    return String(value);
  }

  function applyProps(el, props) {
    props = props || {};
    for (var key in props) {
      if (key === "children" || key === "key") continue;
      var value = props[key];
      if (key === "style" && value) {
        for (var styleKey in value) el.style[styleKey] = cssValue(styleKey, value[styleKey]);
      } else if (key === "ref" && value) {
        value.current = el;
      } else if (key === "className") {
        el.setAttribute("class", String(value));
      } else if (key.slice(0, 2) === "on") {
        el["__" + key] = value;
      } else if (value === true) {
        el.setAttribute(key, "");
      } else if (value !== false && value != null) {
        el.setAttribute(key, String(value));
      }
    }
  }

  function renderNode(node) {
    if (node == null || node === false || node === true) return null;
    if (typeof node === "string" || typeof node === "number") {
      return document.createTextNode(String(node));
    }
    if (typeof node.type === "function") {
      var props = Object.assign({}, node.props || {});
      props.children = node.children;
      return renderNode(node.type(props));
    }
    var el = document.createElement(node.type === globalThis.React.Fragment ? "span" : node.type);
    applyProps(el, node.props);
    for (var i = 0; i < node.children.length; i++) {
      var child = renderNode(node.children[i]);
      if (child) el.appendChild(child);
    }
    return el;
  }

  var effects = [];
  globalThis.React = {
    Fragment: "__fragment",
    createElement: createElement,
    useCallback: function(fn) { return fn; },
    useEffect: function(fn) { effects.push(fn); },
    useMemo: function(fn) { return fn(); },
    useRef: function(value) { return { current: value == null ? null : value }; },
    useState: function(initial) {
      var value = initial;
      return [value, function(next) { value = (typeof next === "function") ? next(value) : next; }];
    }
  };
  globalThis.ReactDOM = {
    createRoot: function(mount) {
      return {
        render: function(element) {
          var node = renderNode(element);
          if (node) mount.appendChild(node);
          for (var i = 0; i < effects.length; i++) effects[i]();
        }
      };
    },
    flushSync: function(fn) { return fn(); }
  };
})();
)JS";
}

void maybe_write_figma_runtime_script(const std::string& runtime_js) {
    const char* out = std::getenv("PULP_FIGMA_RUNTIME_JS_OUT");
    if (out == nullptr || *out == '\0') return;

    std::ofstream file(out, std::ios::binary);
    REQUIRE(file.good());
    file << minimal_host_react_dom_shim() << "\n" << runtime_js << "\n";
}

void maybe_write_stitch_runtime_script(const std::string& runtime_js) {
    const char* out = std::getenv("PULP_STITCH_RUNTIME_JS_OUT");
    if (out == nullptr || *out == '\0') return;

    std::ofstream file(out, std::ios::binary);
    REQUIRE(file.good());
    file << minimal_host_react_dom_shim() << "\n" << runtime_js << "\n";
}

void maybe_write_rn_runtime_script(const std::string& runtime_js) {
    const char* out = std::getenv("PULP_RN_RUNTIME_JS_OUT");
    if (out == nullptr || *out == '\0') return;

    std::ofstream file(out, std::ios::binary);
    REQUIRE(file.good());
    file << minimal_host_react_dom_shim() << "\n" << runtime_js << "\n";
}

void maybe_write_pencil_runtime_script(const std::string& runtime_js) {
    const char* out = std::getenv("PULP_PENCIL_RUNTIME_JS_OUT");
    if (out == nullptr || *out == '\0') return;

    std::ofstream file(out, std::ios::binary);
    REQUIRE(file.good());
    file << minimal_host_react_dom_shim() << "\n" << runtime_js << "\n";
}

} // namespace
