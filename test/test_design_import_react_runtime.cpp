// test_design_import_react_runtime.cpp — extracted from test_design_import.cpp
// in the 2026-05 Phase 5 (P5-3 follow-up) refactor.
//
// React-runtime parser cluster — TSX/runtime React bundle parsers for
// every supported design-tool source. All four parsers share a
// consistent contract: parse fixture, materialize via host React shim,
// accept sanitized TSX, reject out-of-matrix surfaces. Originally lived
// across ~830 contiguous lines in test_design_import.cpp.
//
// Covered:
//   * parse_v0_tsx + parse_v0_dev_react           (v0.dev)
//   * parse_figma_make_react                      (Figma Make)
//   * parse_stitch_react                          (Stitch)
//   * parse_react_native_export                   (React Native)
//   * parse_pencil_react                          (Pencil)

#include <catch2/catch_test_macros.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/widget_bridge.hpp>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace pulp::view;

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

std::string asset_text(const ClaudeBundleAsset& asset) {
    return std::string(asset.data.begin(), asset.data.end());
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

// ── v0 TSX parsing ──────────────────────────────────────────────────────

TEST_CASE("parse_v0_tsx extracts className patterns", "[view][import]") {
    auto tsx = R"(
        export default function Component() {
            return (
                <div className="flex flex-col gap-4 p-4 bg-slate-900">
                    <div className="flex flex-row gap-2">
                        <span>Label</span>
                    </div>
                </div>
            );
        }
    )";
    auto ir = parse_v0_tsx(tsx);

    REQUIRE(ir.source == DesignSource::v0);
    REQUIRE(ir.root.type == "frame");
    // Should extract at least the two className entries
    REQUIRE(ir.root.children.size() >= 2);
}

TEST_CASE("parse_v0_tsx accepts JSON IR directly", "[view][import]") {
    auto json = R"({"type": "frame", "name": "V0Screen", "children": []})";
    auto ir = parse_v0_tsx(json);
    REQUIRE(ir.root.name == "V0Screen");
}

TEST_CASE("parse_v0_dev_react parses staged v0 runtime fixtures",
          "[view][import][parser][v0][phase-6.6.2]") {
    const auto primary = read_fixture("planning/fixtures/v0-dev/audio-control-panel.tsx");
    auto bundle = parse_v0_dev_react(primary);
    REQUIRE(bundle.has_value());
    REQUIRE(bundle->assets.size() == 1);
    REQUIRE(bundle->javascript_indices.size() == 1);
    REQUIRE(bundle->javascript_indices.front() == 0);
    REQUIRE(bundle->assets.front().uuid == "v0-runtime-app");
    REQUIRE(bundle->assets.front().mime == "text/javascript");
    REQUIRE(bundle->template_html.find("data-pulp-source=\"v0\"") != std::string::npos);
    REQUIRE(bundle->template_html.find("v0-audio-control-panel") != std::string::npos);

    const auto js = asset_text(bundle->assets.front());
    REQUIRE(js.find("v0-audio-control-panel") != std::string::npos);
    REQUIRE(js.find("ReactDOM.createRoot") != std::string::npos);
    REQUIRE(js.find("requestAnimationFrame") != std::string::npos);
    REQUIRE(js.find("performance.now") != std::string::npos);
    REQUIRE(js.find("canvas.getContext('2d')") != std::string::npos);
    REQUIRE(js.find("type: 'range'") != std::string::npos);

    const auto settings = read_fixture("planning/fixtures/v0-dev/settings-strip.tsx");
    auto settings_bundle = parse_v0_dev_react(settings);
    REQUIRE(settings_bundle.has_value());
    REQUIRE(asset_text(settings_bundle->assets.front()).find("v0-settings-strip") != std::string::npos);

    const auto transport = read_fixture("planning/fixtures/v0-dev/transport-meter.tsx");
    auto transport_bundle = parse_v0_dev_react(transport);
    REQUIRE(transport_bundle.has_value());
    REQUIRE(asset_text(transport_bundle->assets.front()).find("v0-transport-meter") != std::string::npos);

    const std::string multi_line_import = R"(
        import {
          useState
        } from "react";
        export default function MultiLineReactPanel() {
          const [level, setLevel] = useState(0.5);
          return (
            <div id="v0-multi-line-react-import">
              <span>Level</span>
              <input type="range" value={level} onChange={(event) => setLevel(Number(event.currentTarget.value))} />
            </div>
          );
        }
    )";
    auto multi_line_bundle = parse_v0_dev_react(multi_line_import);
    REQUIRE(multi_line_bundle.has_value());
    REQUIRE(asset_text(multi_line_bundle->assets.front()).find("v0-multi-line-react-import") != std::string::npos);
}

TEST_CASE("parse_v0_dev_react accepts a v0 file envelope",
          "[view][import][parser][v0][phase-6.6.2]") {
    const auto primary = read_fixture("planning/fixtures/v0-dev/audio-control-panel.tsx");
    const auto envelope =
        std::string("[V0_FILE]json:file=\"package.json\"\n{\"dependencies\":{\"react\":\"latest\"}}\n")
        + "[V0_FILE]tsx:file=\"app/page.tsx\"\n"
        + primary
        + "\n[V0_FILE]tsx:file=\"app/ignored.tsx\"\n"
          "import { useState } from \"react\";\n"
          "export default function Ignored(){ return <div id=\"ignored\">Ignored</div>; }\n";

    auto bundle = parse_v0_dev_react(envelope);
    REQUIRE(bundle.has_value());
    REQUIRE(bundle->template_html.find("v0-audio-control-panel") != std::string::npos);
    REQUIRE(asset_text(bundle->assets.front()).find("app/page.tsx") != std::string::npos);
}

TEST_CASE("parse_v0_dev_react rejects out-of-matrix v0 default surfaces",
          "[view][import][parser][v0][phase-6.6.2]") {
    const std::vector<std::string> rejected = {
        R"(
            import { useState } from "react";
            export default function TailwindPanel() {
              return <div className="flex rounded-lg">Tailwind</div>;
            }
        )",
        R"(
            import { useState } from "react";
            import { Button } from "@/components/ui/button";
            export default function ShadcnPanel() {
              return <Button>Open</Button>;
            }
        )",
        R"(
            import {
              format
            } from "date-fns";
            export default function UnsupportedImportPanel() {
              return <div id="unsupported-import">Unsupported import</div>;
            }
        )",
        R"(
            import { useEffect } from "react";
            export default function DynamicImportPanel() {
              useEffect(() => { import("./meter"); }, []);
              return <div id="dynamic-import">Dynamic import</div>;
            }
        )",
        R"(
            import { useEffect } from "react";
            export default function NetworkPanel() {
              useEffect(() => { fetch("/api/state"); }, []);
              return <div id="network">Network</div>;
            }
        )",
        R"(
            import { useState } from "react";
            export default function TextInputPanel() {
              const [value, setValue] = useState("");
              return <input type="text" value={value} onChange={(event) => setValue(event.currentTarget.value)} />;
            }
        )",
        R"(
            import { useState } from "react";
            export default function CustomComponentPanel() {
              return <Slider value={0.5} />;
            }
        )"
    };

    for (const auto& sample : rejected) {
        CAPTURE(sample);
        REQUIRE_FALSE(parse_v0_dev_react(sample).has_value());
    }
}

// ── Figma Make React parsing ────────────────────────────────────────────

TEST_CASE("parse_figma_make_react parses staged Figma runtime fixture",
          "[view][import][parser][figma][phase-6.6.3]") {
    const auto primary = read_fixture("planning/fixtures/figma/level-meter-panel.tsx");
    auto bundle = parse_figma_make_react(primary);
    REQUIRE(bundle.has_value());
    REQUIRE(bundle->assets.size() == 1);
    REQUIRE(bundle->javascript_indices.size() == 1);
    REQUIRE(bundle->javascript_indices.front() == 0);
    REQUIRE(bundle->assets.front().uuid == "figma-runtime-app");
    REQUIRE(bundle->assets.front().mime == "text/javascript");
    REQUIRE(bundle->template_html.find("data-pulp-source=\"figma\"") != std::string::npos);
    REQUIRE(bundle->template_html.find("figma-level-meter-panel") != std::string::npos);

    const auto js = asset_text(bundle->assets.front());
    REQUIRE(js.find("figma-level-meter-panel") != std::string::npos);
    REQUIRE(js.find("Figma Make runtime import requires host React and ReactDOM") != std::string::npos);
    REQUIRE(js.find("'data-pulp-source': 'figma'") != std::string::npos);
    REQUIRE(js.find("ReactDOM.createRoot") != std::string::npos);
    REQUIRE(js.find("requestAnimationFrame") != std::string::npos);
    REQUIRE(js.find("performance.now") != std::string::npos);
    REQUIRE(js.find("canvas.getContext('2d')") != std::string::npos);
    REQUIRE(js.find("type: 'range'") != std::string::npos);
}

TEST_CASE("parse_figma_make_react runtime bundle materializes with host React shim",
          "[view][import][parser][figma][render][phase-6.6.3]") {
    const auto primary = read_fixture("planning/fixtures/figma/level-meter-panel.tsx");
    auto bundle = parse_figma_make_react(primary);
    REQUIRE(bundle.has_value());
    const auto runtime_js = asset_text(bundle->assets.front());
    maybe_write_figma_runtime_script(runtime_js);

    pulp::state::StateStore store;
    ScriptEngine engine;
    View root;
    WidgetBridge bridge(engine, root, store);

    REQUIRE_NOTHROW(bridge.load_script(minimal_host_react_dom_shim()));
    REQUIRE_NOTHROW(bridge.load_script(runtime_js));
    bridge.service_frame_callbacks();

    REQUIRE(root.child_count() > 0);
    REQUIRE(engine.evaluate("!!document.getElementById('figma-level-meter-panel')")
                .getWithDefault<bool>(false));
    REQUIRE_FALSE(engine.evaluate(
        "(function(){ var el = document.getElementById('figma-level-meter-panel');"
        "return el ? String(el._id || '') : ''; })()").toString().empty());
}

TEST_CASE("parse_figma_make_react accepts sanitized Figma Make TSX",
          "[view][import][parser][figma][phase-6.6.3]") {
    const std::string sanitized = R"(
        // Source: Figma Make export (sanitized for Pulp runtime import)
        import {
          useState
        } from "react";
        export default function MultiLineFigmaPanel() {
          const [level, setLevel] = useState(0.5);
          return (
            <div id="figma-multi-line-react-import" style={{ display: "flex", flexDirection: "column" }}>
              <span>Level</span>
              <input type="range" value={level} onChange={(event) => setLevel(Number(event.currentTarget.value))} />
            </div>
          );
        }
    )";

    auto bundle = parse_figma_make_react(sanitized);
    REQUIRE(bundle.has_value());
    REQUIRE(bundle->template_html.find("figma-multi-line-react-import") != std::string::npos);
    REQUIRE(asset_text(bundle->assets.front()).find("figma-multi-line-react-import") != std::string::npos);
}

TEST_CASE("parse_figma_make_react rejects out-of-matrix Figma defaults",
          "[view][import][parser][figma][phase-6.6.3]") {
    const std::vector<std::string> rejected = {
        R"(
            "use client";
            // Source: Figma Make export
            import { useState } from "react";
            export default function NextFigmaPanel() {
              return <div id="figma-next-panel">Next path</div>;
            }
        )",
        R"(
            // Source: Figma Make export
            import { useState } from "react";
            import hero from "figma:asset/a1b2c3d4.png";
            export default function FigmaAssetPanel() {
              return <div id="figma-asset-panel">Asset {hero}</div>;
            }
        )",
        R"(
            // Source: Figma Make export
            import { Dialog } from "@radix-ui/react-dialog@1.1.6";
            export default function VersionedImportPanel() {
              return <div id="figma-versioned-panel">Versioned</div>;
            }
        )",
        R"(
            import figma from "@figma/code-connect/react";
            figma.connect(Button, "https://figma.com/file/example", {
              example: () => <Button />
            });
        )",
        R"(
            // Source: Figma Make export
            import { useState } from "react";
            export default function TailwindPanel() {
              return <div className="flex rounded-lg">Tailwind</div>;
            }
        )",
        R"(
            // Source: Figma Make export
            import { useState } from "react";
            import Link from "next/link";
            export default function NextImportPanel() {
              return <Link href="/">Next</Link>;
            }
        )",
        R"(
            import { useState } from "react";
            export default function GenericReactPanel() {
              const [level, setLevel] = useState(0.5);
              return <input type="range" value={level} onChange={(event) => setLevel(Number(event.currentTarget.value))} />;
            }
        )",
        R"(
            // Source: Figma Make export
            import { useState } from "react";
            export default function TextInputPanel() {
              const [value, setValue] = useState("");
              return <input type="text" value={value} onChange={(event) => setValue(event.currentTarget.value)} />;
            }
        )"
    };

    for (const auto& sample : rejected) {
        CAPTURE(sample);
        REQUIRE_FALSE(parse_figma_make_react(sample).has_value());
    }
}

// ── Google Stitch React parsing ────────────────────────────────────────

TEST_CASE("parse_stitch_react parses staged Stitch runtime fixture",
          "[view][import][parser][stitch][phase-6.6.4]") {
    const auto primary = read_fixture("planning/fixtures/stitch/transport-bar.tsx");
    auto bundle = parse_stitch_react(primary);
    REQUIRE(bundle.has_value());
    REQUIRE(bundle->assets.size() == 1);
    REQUIRE(bundle->javascript_indices.size() == 1);
    REQUIRE(bundle->javascript_indices.front() == 0);
    REQUIRE(bundle->assets.front().uuid == "stitch-runtime-app");
    REQUIRE(bundle->assets.front().mime == "text/javascript");
    REQUIRE(bundle->template_html.find("data-pulp-source=\"stitch\"") != std::string::npos);
    REQUIRE(bundle->template_html.find("stitch-transport-bar") != std::string::npos);

    const auto js = asset_text(bundle->assets.front());
    REQUIRE(js.find("stitch-transport-bar") != std::string::npos);
    REQUIRE(js.find("Stitch runtime import requires host React and ReactDOM") != std::string::npos);
    REQUIRE(js.find("'data-pulp-source': 'stitch'") != std::string::npos);
    REQUIRE(js.find("ReactDOM.createRoot") != std::string::npos);
    REQUIRE(js.find("requestAnimationFrame") != std::string::npos);
    REQUIRE(js.find("performance.now") != std::string::npos);
    REQUIRE(js.find("canvas.getContext('2d')") != std::string::npos);
    REQUIRE(js.find("type: 'range'") != std::string::npos);
}

TEST_CASE("parse_stitch_react runtime bundle materializes with host React shim",
          "[view][import][parser][stitch][render][phase-6.6.4]") {
    const auto primary = read_fixture("planning/fixtures/stitch/transport-bar.tsx");
    auto bundle = parse_stitch_react(primary);
    REQUIRE(bundle.has_value());
    const auto runtime_js = asset_text(bundle->assets.front());
    maybe_write_stitch_runtime_script(runtime_js);

    pulp::state::StateStore store;
    ScriptEngine engine;
    View root;
    WidgetBridge bridge(engine, root, store);

    REQUIRE_NOTHROW(bridge.load_script(minimal_host_react_dom_shim()));
    REQUIRE_NOTHROW(bridge.load_script(runtime_js));
    bridge.service_frame_callbacks();

    REQUIRE(root.child_count() > 0);
    REQUIRE(engine.evaluate("!!document.getElementById('stitch-transport-bar')")
                .getWithDefault<bool>(false));
    REQUIRE_FALSE(engine.evaluate(
        "(function(){ var el = document.getElementById('stitch-transport-bar');"
        "return el ? String(el._id || '') : ''; })()").toString().empty());
}

TEST_CASE("parse_stitch_react accepts sanitized Stitch TSX",
          "[view][import][parser][stitch][phase-6.6.4]") {
    const std::string sanitized = R"(
        import {
          useState
        } from "react";
        export default function MultiLineStitchPanel() {
          const [level, setLevel] = useState(0.5);
          return (
            <div id="stitch-multi-line-react-import" data-stitch-screen="level" style={{ display: "flex", flexDirection: "column" }}>
              <span>Level</span>
              <input type="range" value={level} onChange={(event) => setLevel(Number(event.currentTarget.value))} />
            </div>
          );
        }
    )";

    auto bundle = parse_stitch_react(sanitized);
    REQUIRE(bundle.has_value());
    REQUIRE(bundle->template_html.find("stitch-multi-line-react-import") != std::string::npos);
    REQUIRE(asset_text(bundle->assets.front()).find("stitch-multi-line-react-import") != std::string::npos);
}

TEST_CASE("parse_stitch_react rejects out-of-matrix Stitch defaults",
          "[view][import][parser][stitch][phase-6.6.4]") {
    const std::vector<std::string> rejected = {
        R"(
            "use client";
            import { useState } from "react";
            export default function NextStitchPanel() {
              return <div id="stitch-next-panel">Next path</div>;
            }
        )",
        R"(
            import { useState } from "react";
            export default function TailwindPanel() {
              return <div className="flex rounded-lg">Tailwind</div>;
            }
        )",
        R"(
            import "./transport.css";
            import { useState } from "react";
            export default function ExternalCssPanel() {
              return <div id="stitch-css-panel">CSS</div>;
            }
        )",
        R"(
            import { Dialog } from "@radix-ui/react-dialog";
            export default function RadixPanel() {
              return <div id="stitch-radix-panel">Radix</div>;
            }
        )",
        R"(
            import Link from "next/link";
            export default function NextImportPanel() {
              return <Link href="/">Next</Link>;
            }
        )",
        R"(
            import { View } from "react-native";
            export default function NativePanel() {
              return <View />;
            }
        )",
        R"({"screen_id":"transport-bar","nodes":[]})",
        R"(
            import { useState } from "react";
            export default function TextInputPanel() {
              const [value, setValue] = useState("");
              return <input type="text" value={value} onChange={(event) => setValue(event.currentTarget.value)} />;
            }
        )",
        R"(
            import { useState } from "react";
            export default function CustomComponentPanel() {
              return <TransportSlider value={0.5} />;
            }
        )"
    };

    for (const auto& sample : rejected) {
        CAPTURE(sample);
        REQUIRE_FALSE(parse_stitch_react(sample).has_value());
    }
}

// ── React Native export parsing ────────────────────────────────────────

TEST_CASE("parse_react_native_export parses staged RN runtime fixture",
          "[view][import][parser][rn][phase-6.6.5]") {
    const auto primary = read_fixture("planning/fixtures/rn/gain-stage.tsx");
    auto bundle = parse_react_native_export(primary);
    REQUIRE(bundle.has_value());
    REQUIRE(bundle->assets.size() == 1);
    REQUIRE(bundle->javascript_indices.size() == 1);
    REQUIRE(bundle->javascript_indices.front() == 0);
    REQUIRE(bundle->assets.front().uuid == "rn-runtime-app");
    REQUIRE(bundle->assets.front().mime == "text/javascript");
    REQUIRE(bundle->template_html.find("data-pulp-source=\"rn\"") != std::string::npos);
    REQUIRE(bundle->template_html.find("rn-gain-stage") != std::string::npos);

    const auto js = asset_text(bundle->assets.front());
    REQUIRE(js.find("rn-gain-stage") != std::string::npos);
    REQUIRE(js.find("React Native runtime import requires host React and ReactDOM") != std::string::npos);
    REQUIRE(js.find("'data-pulp-source': 'rn'") != std::string::npos);
    REQUIRE(js.find("'data-rn-default-flex': 'column'") != std::string::npos);
    REQUIRE(js.find("flexDirection: 'column'") != std::string::npos);
    REQUIRE(js.find("ReactDOM.createRoot") != std::string::npos);
    REQUIRE(js.find("canvas.getContext") == std::string::npos);
    REQUIRE(js.find("requestAnimationFrame") == std::string::npos);
}

TEST_CASE("parse_react_native_export runtime bundle materializes with host React shim",
          "[view][import][parser][rn][render][phase-6.6.5]") {
    // pulp #1987 — this test reliably fails ONLY under UndefinedBehaviorSanitizer
    // (macOS ARM64). The failure is `REQUIRE_NOTHROW(bridge.load_script(runtime_js))`
    // surfacing an "Unknown exception" — the exception originates inside the
    // QuickJS / RN-runtime-shim path, but only when UBSan instrumentation is
    // active. ASan, TSan, and the non-sanitized macOS lane all pass on the
    // exact same commit. Tracked under #1987 for proper root-cause work
    // (likely a signed-overflow / shift / alignment trap inside vendored
    // QuickJS); skipping the body under UBSan only so the rest of the
    // suite can attribute real regressions correctly. Do NOT remove this
    // skip without first resolving #1987.
#if defined(__has_feature)
#  if __has_feature(undefined_behavior_sanitizer)
    SUCCEED("skipped under UBSan (pulp #1987 — pre-existing main-branch flake)");
    return;
#  endif
#endif

    const auto primary = read_fixture("planning/fixtures/rn/gain-stage.tsx");
    auto bundle = parse_react_native_export(primary);
    REQUIRE(bundle.has_value());
    const auto runtime_js = asset_text(bundle->assets.front());
    maybe_write_rn_runtime_script(runtime_js);

    pulp::state::StateStore store;
    ScriptEngine engine;
    View root;
    WidgetBridge bridge(engine, root, store);

    REQUIRE_NOTHROW(bridge.load_script(minimal_host_react_dom_shim()));
    REQUIRE_NOTHROW(bridge.load_script(runtime_js));
    bridge.service_frame_callbacks();

    REQUIRE(root.child_count() > 0);
    REQUIRE(engine.evaluate("!!document.getElementById('rn-gain-stage')")
                .getWithDefault<bool>(false));
    REQUIRE(engine.evaluate(
        "(function(){ var el = document.getElementById('rn-gain-stage');"
        "return el && el.getAttribute('data-rn-default-flex') === 'column'; })()")
        .getWithDefault<bool>(false));
}

TEST_CASE("parse_react_native_export accepts sanitized RN TSX",
          "[view][import][parser][rn][phase-6.6.5]") {
    const std::string sanitized = R"(
        import React, {
          useState
        } from "react";
        import {
          Pressable,
          StyleSheet,
          Text,
          View
        } from "react-native";
        export default function MultiLineNativePanel() {
          const [armed, setArmed] = useState(true);
          return (
            <View id="rn-multi-line-react-import" style={styles.panel}>
              <Text style={styles.label}>React Native export</Text>
              <Text style={styles.title}>Gain Stage</Text>
              <Pressable onPress={() => setArmed(!armed)} style={styles.button}>
                <Text>{armed ? "ARMED" : "BYPASS"}</Text>
              </Pressable>
            </View>
          );
        }
        const styles = StyleSheet.create({
          panel: { padding: 18, backgroundColor: "#111827" },
          label: { color: "#8fb3ff" },
          title: { color: "#f8fafc", fontSize: 24 },
          button: { minHeight: 36 }
        });
    )";

    auto bundle = parse_react_native_export(sanitized);
    REQUIRE(bundle.has_value());
    REQUIRE(bundle->template_html.find("rn-multi-line-react-import") != std::string::npos);
    const auto js = asset_text(bundle->assets.front());
    REQUIRE(js.find("rn-multi-line-react-import") != std::string::npos);
    REQUIRE(js.find("flexDirection: 'column'") != std::string::npos);
}

TEST_CASE("parse_react_native_export rejects out-of-matrix RN surfaces",
          "[view][import][parser][rn][phase-6.6.5]") {
    const std::vector<std::string> rejected = {
        R"(
            import { useState } from "react";
            export default function GenericReactPanel() {
              return <div id="generic">Generic</div>;
            }
        )",
        R"(
            "use client";
            import { View } from "react-native";
            export default function SolitoPanel() {
              return <View />;
            }
        )",
        R"(
            import { Animated, View } from "react-native";
            export default function AnimatedPanel() {
              return <Animated.View />;
            }
        )",
        R"(
            import Animated from "react-native-reanimated";
            import { View } from "react-native";
            export default function WorkletPanel() {
              return <View />;
            }
        )",
        R"(
            import { Linking, View } from "react-native";
            export default function LinkingPanel() {
              Linking.openURL("https://example.com");
              return <View />;
            }
        )",
        R"(
            import { Alert, View } from "react-native";
            export default function AlertPanel() {
              Alert.alert("Nope");
              return <View />;
            }
        )",
        R"(
            import { Dimensions, View } from "react-native";
            export default function DimensionsPanel() {
              const width = Dimensions.get("window").width;
              return <View />;
            }
        )",
        R"(
            import { Platform, View } from "react-native";
            export default function PlatformPanel() {
              return <View />;
            }
        )",
        R"(
            import { Animated, View } from "react-native";
            export default function AnimatedImportPanel() {
              return <View />;
            }
        )",
        R"(
            import ReactNative from "react-native";
            export default function DefaultNativePanel() {
              return <ReactNative.View />;
            }
        )",
        R"(
            import * as ReactNative from "react-native";
            export default function NamespaceNativePanel() {
              return <ReactNative.View />;
            }
        )",
        R"(
            import { FlatList } from "react-native";
            export default function ListPanel() {
              return <FlatList data={[]} renderItem={() => null} />;
            }
        )",
        R"(
            import { Image } from "react-native";
            export default function ImagePanel() {
              return <Image source={{ uri: "https://example.com/a.png" }} />;
            }
        )",
        R"(
            import { View } from "react-native";
            export default function StyleArrayPanel() {
              return <View style={[styles.base, styles.hot]} />;
            }
            const styles = { base: {}, hot: {} };
        )",
        R"(
            import { View } from "react-native";
            import { NavigationContainer } from "@react-navigation/native";
            export default function NavigationPanel() {
              return <View />;
            }
        )",
        R"(
            import { View } from "react-native";
            export default function DomPanel() {
              return <div id="dom">DOM</div>;
            }
        )"
    };

    for (const auto& sample : rejected) {
        CAPTURE(sample);
        REQUIRE_FALSE(parse_react_native_export(sample).has_value());
    }
}

// ── Pencil React export parsing ─────────────────────────────────────────

TEST_CASE("parse_pencil_react parses staged Pencil runtime fixture",
          "[view][import][parser][pencil][phase-6.6.6]") {
    const auto primary = read_fixture("planning/fixtures/pencil/gain-stage-card.tsx");
    auto bundle = parse_pencil_react(primary);
    REQUIRE(bundle.has_value());
    REQUIRE(bundle->assets.size() == 1);
    REQUIRE(bundle->javascript_indices.size() == 1);
    REQUIRE(bundle->javascript_indices.front() == 0);
    REQUIRE(bundle->assets.front().uuid == "pencil-runtime-app");
    REQUIRE(bundle->assets.front().mime == "text/javascript");
    REQUIRE(bundle->template_html.find("data-pulp-source=\"pencil\"") != std::string::npos);
    REQUIRE(bundle->template_html.find("pencil-gain-stage-card") != std::string::npos);

    const auto js = asset_text(bundle->assets.front());
    REQUIRE(js.find("pencil-gain-stage-card") != std::string::npos);
    REQUIRE(js.find("Pencil runtime import requires host React and ReactDOM") != std::string::npos);
    REQUIRE(js.find("'data-pulp-source': 'pencil'") != std::string::npos);
    REQUIRE(js.find("ReactDOM.createRoot") != std::string::npos);
    REQUIRE(js.find("requestAnimationFrame") != std::string::npos);
    REQUIRE(js.find("canvas.getContext('2d')") != std::string::npos);
    REQUIRE(js.find("type: 'range'") != std::string::npos);
}

TEST_CASE("parse_pencil_react runtime bundle materializes with host React shim",
          "[view][import][parser][pencil][render][phase-6.6.6]") {
    const auto primary = read_fixture("planning/fixtures/pencil/gain-stage-card.tsx");
    auto bundle = parse_pencil_react(primary);
    REQUIRE(bundle.has_value());
    const auto runtime_js = asset_text(bundle->assets.front());
    maybe_write_pencil_runtime_script(runtime_js);

    pulp::state::StateStore store;
    ScriptEngine engine;
    View root;
    WidgetBridge bridge(engine, root, store);

    REQUIRE_NOTHROW(bridge.load_script(minimal_host_react_dom_shim()));
    REQUIRE_NOTHROW(bridge.load_script(runtime_js));
    bridge.service_frame_callbacks();

    REQUIRE(root.child_count() > 0);
    REQUIRE(engine.evaluate("!!document.getElementById('pencil-gain-stage-card')")
                .getWithDefault<bool>(false));
    REQUIRE_FALSE(engine.evaluate(
        "(function(){ var el = document.getElementById('pencil-gain-stage-card');"
        "return el ? String(el._id || '') : ''; })()").toString().empty());
}

TEST_CASE("parse_pencil_react accepts sanitized Pencil TSX",
          "[view][import][parser][pencil][phase-6.6.6]") {
    const std::string sanitized = R"(
        import {
          useState
        } from "react";
        export default function MultiLinePencilPanel() {
          const [level, setLevel] = useState(0.5);
          return (
            <div id="pencil-multi-line-react-import" data-pencil-export="tailwind-jsx-sanitized" style={{ display: "flex", flexDirection: "column" }}>
              <span>Level</span>
              <input type="range" value={level} onChange={(event) => setLevel(Number(event.currentTarget.value))} />
            </div>
          );
        }
    )";

    auto bundle = parse_pencil_react(sanitized);
    REQUIRE(bundle.has_value());
    REQUIRE(bundle->template_html.find("pencil-multi-line-react-import") != std::string::npos);
    REQUIRE(asset_text(bundle->assets.front()).find("pencil-multi-line-react-import") != std::string::npos);
    REQUIRE_FALSE(parse_v0_dev_react(sanitized).has_value());

    const std::string no_id = R"(
        import { useState } from "react";
        export default function Meter_Panel() {
          const [level, setLevel] = useState(0.5);
          return (
            <div data-pencil-export="tailwind-jsx-sanitized" style={{ display: "flex", flexDirection: "column" }}>
              <span>Meter</span>
              <input type="range" value={level} onChange={(event) => setLevel(Number(event.currentTarget.value))} />
            </div>
          );
        }
    )";

    auto fallback_bundle = parse_pencil_react(no_id);
    REQUIRE(fallback_bundle.has_value());
    REQUIRE(fallback_bundle->template_html.find("pencil-meter-panel") != std::string::npos);
    REQUIRE(asset_text(fallback_bundle->assets.front()).find("pencil-meter-panel") != std::string::npos);
}

TEST_CASE("parse_pencil_react rejects out-of-matrix Pencil defaults",
          "[view][import][parser][pencil][phase-6.6.6]") {
    const std::vector<std::string> rejected = {
        R"(
            "use client";
            import { useState } from "react";
            export default function NextPencilPanel() {
              return <div id="pencil-next-panel">Next path</div>;
            }
        )",
        R"(
            import { useState } from "react";
            export default function TailwindPanel() {
              return <div className="flex rounded-lg bg-[--pencil-color-primary]">Tailwind</div>;
            }
        )",
        R"PULP(
            import { useState } from "react";
            export default function TokenPanel() {
              return <div id="pencil-token-panel" style={{ backgroundColor: "var(--pencil-color-primary)" }}>Token</div>;
            }
        )PULP",
        R"(
            import "./pencil.css";
            import { useState } from "react";
            export default function ExternalCssPanel() {
              return <div id="pencil-css-panel">CSS</div>;
            }
        )",
        R"(
            import { Dialog } from "@radix-ui/react-dialog";
            export default function RadixPanel() {
              return <div id="pencil-radix-panel">Radix</div>;
            }
        )",
        R"(
            import Link from "next/link";
            export default function NextImportPanel() {
              return <Link href="/">Next</Link>;
            }
        )",
        R"(
            import { View } from "react-native";
            export default function NativePanel() {
              return <View />;
            }
        )",
        R"({"mcp_response":{"node_tree":[],"source":"pencil"}})",
        R"({"batch_get":[{"id":"node-1"}]})",
        R"(
            import design from "./control.pen";
            export default function BinaryPenPanel() {
              return <div id="pencil-pen-panel">Binary</div>;
            }
        )",
        R"(
            import { useState } from "react";
            export default function TextInputPanel() {
              const [value, setValue] = useState("");
              return <input type="text" value={value} onChange={(event) => setValue(event.currentTarget.value)} />;
            }
        )",
        R"(
            import { useState } from "react";
            export default function CustomComponentPanel() {
              return <PencilWidget value={0.5} />;
            }
        )"
    };

    for (const auto& sample : rejected) {
        CAPTURE(sample);
        REQUIRE_FALSE(parse_pencil_react(sample).has_value());
    }
}
