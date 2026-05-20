// test_editor_url.cpp — Inspector Phase 5.3 editor URI plumbing.
//
// Covers the format helper, env-vs-config-vs-default precedence,
// template validation, and the two new protocol methods
// (`Inspector.setEditorUrlTemplate` / `Inspector.getEditorUrlTemplate`).

#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/domain_handler.hpp>
#include <pulp/inspect/editor_url.hpp>
#include <pulp/inspect/protocol.hpp>
#include <pulp/inspect/source_jump.hpp>
#include <pulp/view/view.hpp>

#include <choc/text/choc_JSON.h>

#include <cstdlib>
#include <string>

using namespace pulp::inspect;

namespace {

// Scoped env var setter — restores prior state on destruction so tests
// stay order-independent. The Phase 5.3 env override is global state,
// so any test that mutates it must clean up.
struct ScopedEnv {
    explicit ScopedEnv(std::string name) : name_(std::move(name)) {
        if (const char* prev = std::getenv(name_.c_str())) {
            prev_ = std::string(prev);
            had_prev_ = true;
        }
    }
    void set(const std::string& value) {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), value.c_str());
#else
        ::setenv(name_.c_str(), value.c_str(), /*overwrite=*/1);
#endif
    }
    void unset() {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), "");
#else
        ::unsetenv(name_.c_str());
#endif
    }
    ~ScopedEnv() {
        if (had_prev_) set(prev_);
        else unset();
    }
private:
    std::string name_;
    std::string prev_;
    bool had_prev_ = false;
};

} // namespace

TEST_CASE("format_editor_url substitutes path and line for vscode-style template",
          "[inspect][editor-url]") {
    auto url = format_editor_url("vscode://file/{path}:{line}",
                                 "/Users/foo/bar.cpp", 42);
    REQUIRE(url == "vscode://file//Users/foo/bar.cpp:42");
}

TEST_CASE("format_editor_url substitutes col when template includes {col}",
          "[inspect][editor-url]") {
    auto url = format_editor_url("zed://file/{path}:{line}:{col}",
                                 "/tmp/x.rs", 10, 7);
    REQUIRE(url == "zed://file//tmp/x.rs:10:7");
}

TEST_CASE("format_editor_url omits col cleanly when template lacks {col}",
          "[inspect][editor-url]") {
    // Cursor template has no {col} — supplying one should not break it.
    auto url = format_editor_url("cursor://file/{path}:{line}",
                                 "src/main.cpp", 3, 99);
    REQUIRE(url == "cursor://file/src/main.cpp:3");
}

TEST_CASE("format_editor_url with absent col blanks {col} token",
          "[inspect][editor-url]") {
    // Template references {col} but caller did not supply one — the token
    // becomes empty. This is the expected behavior for templates that
    // request a column but operate on an anchor that doesn't have one.
    auto url = format_editor_url("zed://file/{path}:{line}:{col}",
                                 "main.rs", 5);
    REQUIRE(url == "zed://file/main.rs:5:");
}

TEST_CASE("format_editor_url handles sublime / idea query-style templates",
          "[inspect][editor-url]") {
    auto sub = format_editor_url(
        "sublime://open?url=file://{path}&line={line}", "/abs/x.py", 12);
    REQUIRE(sub == "sublime://open?url=file:///abs/x.py&line=12");

    auto idea = format_editor_url(
        "idea://open?file={path}&line={line}", "/abs/y.java", 7);
    REQUIRE(idea == "idea://open?file=/abs/y.java&line=7");
}

TEST_CASE("validate_editor_url_template rejects templates without {path}",
          "[inspect][editor-url]") {
    std::string err;
    REQUIRE_FALSE(validate_editor_url_template("vscode://file/bare", &err));
    REQUIRE_FALSE(err.empty());

    REQUIRE_FALSE(validate_editor_url_template("", &err));
    REQUIRE_FALSE(err.empty());

    REQUIRE(validate_editor_url_template("vscode://file/{path}:{line}"));
    REQUIRE(validate_editor_url_template("custom://{path}"));
}

TEST_CASE("effective_editor_url: env override beats config and default",
          "[inspect][editor-url]") {
    ScopedEnv env("PULP_INSPECTOR_EDITOR_URL");
    env.set("cursor://file/{path}:{line}");

    InspectorConfig cfg;
    cfg.editor_url_template = "vscode://file/{path}:{line}";

    auto eff = effective_editor_url(cfg);
    REQUIRE(eff.template_str == "cursor://file/{path}:{line}");
    REQUIRE(eff.source == EditorUrlSource::Environment);
    REQUIRE(std::string(editor_url_source_name(eff.source)) == "environment");
}

TEST_CASE("effective_editor_url: config beats default when set, no env",
          "[inspect][editor-url]") {
    ScopedEnv env("PULP_INSPECTOR_EDITOR_URL");
    env.unset();

    InspectorConfig cfg;
    cfg.editor_url_template = "zed://file/{path}:{line}:{col}";

    auto eff = effective_editor_url(cfg);
    REQUIRE(eff.template_str == "zed://file/{path}:{line}:{col}");
    REQUIRE(eff.source == EditorUrlSource::Config);
}

TEST_CASE("effective_editor_url: default kicks in when nothing is set",
          "[inspect][editor-url]") {
    ScopedEnv env("PULP_INSPECTOR_EDITOR_URL");
    env.unset();

    InspectorConfig cfg; // default-constructed
    auto eff = effective_editor_url(cfg);
    REQUIRE(eff.template_str == "vscode://file/{path}:{line}");
    REQUIRE(eff.source == EditorUrlSource::Default);
}

TEST_CASE("editor_url_env_override returns nullopt for empty env",
          "[inspect][editor-url]") {
    ScopedEnv env("PULP_INSPECTOR_EDITOR_URL");
    env.set("");
    REQUIRE_FALSE(editor_url_env_override().has_value());
}

// ── Protocol surface ───────────────────────────────────────────────────────

TEST_CASE("Inspector.setEditorUrlTemplate stores valid template and round-trips",
          "[inspect][editor-url][protocol]") {
    ScopedEnv env("PULP_INSPECTOR_EDITOR_URL");
    env.unset();

    DomainHandler handler;

    auto set_req = make_request(
        1, methods::kInspectorSetEditorUrlTemplate,
        R"({"template":"zed://file/{path}:{line}:{col}"})");
    auto set_resp = handler.handle(set_req);
    REQUIRE_FALSE(set_resp.is_error);

    auto set_obj = choc::json::parse(set_resp.params_json);
    REQUIRE(set_obj["ok"].getBool());
    REQUIRE(std::string(set_obj["template"].getString())
            == "zed://file/{path}:{line}:{col}");

    auto get_req = make_request(2, methods::kInspectorGetEditorUrlTemplate, "{}");
    auto get_resp = handler.handle(get_req);
    REQUIRE_FALSE(get_resp.is_error);

    auto get_obj = choc::json::parse(get_resp.params_json);
    REQUIRE(std::string(get_obj["template"].getString())
            == "zed://file/{path}:{line}:{col}");
    REQUIRE(std::string(get_obj["source"].getString()) == "config");
    REQUIRE(std::string(get_obj["configTemplate"].getString())
            == "zed://file/{path}:{line}:{col}");
}

TEST_CASE("Inspector.setEditorUrlTemplate rejects template without {path}",
          "[inspect][editor-url][protocol]") {
    DomainHandler handler;
    auto req = make_request(
        1, methods::kInspectorSetEditorUrlTemplate,
        R"({"template":"vscode://file/no-token"})");
    auto resp = handler.handle(req);
    REQUIRE(resp.is_error);
    // The config must be left untouched.
    REQUIRE(handler.config().editor_url_template
            == "vscode://file/{path}:{line}");
}

TEST_CASE("Inspector.setEditorUrlTemplate rejects missing/non-string template",
          "[inspect][editor-url][protocol]") {
    DomainHandler handler;

    auto missing = handler.handle(make_request(
        1, methods::kInspectorSetEditorUrlTemplate, R"({})"));
    REQUIRE(missing.is_error);

    auto wrong_type = handler.handle(make_request(
        2, methods::kInspectorSetEditorUrlTemplate, R"({"template":42})"));
    REQUIRE(wrong_type.is_error);
}

TEST_CASE("Inspector.getEditorUrlTemplate reports environment override",
          "[inspect][editor-url][protocol]") {
    ScopedEnv env("PULP_INSPECTOR_EDITOR_URL");
    env.set("idea://open?file={path}&line={line}");

    DomainHandler handler;
    auto resp = handler.handle(make_request(
        1, methods::kInspectorGetEditorUrlTemplate, "{}"));
    REQUIRE_FALSE(resp.is_error);

    auto obj = choc::json::parse(resp.params_json);
    REQUIRE(std::string(obj["template"].getString())
            == "idea://open?file={path}&line={line}");
    REQUIRE(std::string(obj["source"].getString()) == "environment");
    REQUIRE(obj.hasObjectMember("envTemplate"));
    REQUIRE(std::string(obj["envTemplate"].getString())
            == "idea://open?file={path}&line={line}");
}

TEST_CASE("DomainHandler::set_config swaps the template without touching env",
          "[inspect][editor-url][protocol]") {
    ScopedEnv env("PULP_INSPECTOR_EDITOR_URL");
    env.unset();

    DomainHandler handler;
    InspectorConfig cfg;
    cfg.editor_url_template = "cursor://file/{path}:{line}";
    handler.set_config(cfg);
    REQUIRE(handler.config().editor_url_template
            == "cursor://file/{path}:{line}");

    auto resp = handler.handle(make_request(
        1, methods::kInspectorGetEditorUrlTemplate, "{}"));
    auto obj = choc::json::parse(resp.params_json);
    REQUIRE(std::string(obj["source"].getString()) == "config");
    REQUIRE(std::string(obj["template"].getString())
            == "cursor://file/{path}:{line}");
}

// ── Phase 5.1: source-jump resolution ──────────────────────────────────────

TEST_CASE("resolve_source_jump formats the URL for a view with provenance",
          "[inspect][source-jump]") {
    ScopedEnv env("PULP_INSPECTOR_EDITOR_URL");
    env.unset();

    pulp::view::View view;
    view.set_source_loc({"src/Synth.jsx", 42, 7});

    InspectorConfig cfg;  // default vscode template (no {col})
    auto result = resolve_source_jump(cfg, &view);
    REQUIRE(result.ok);
    REQUIRE(result.path == "src/Synth.jsx");
    REQUIRE(result.line == 42);
    REQUIRE(result.col == 7);
    REQUIRE(result.url == "vscode://file/src/Synth.jsx:42");
    REQUIRE_FALSE(result.launched);  // resolve never launches
}

TEST_CASE("resolve_source_jump honors a {col}-bearing editor template",
          "[inspect][source-jump]") {
    ScopedEnv env("PULP_INSPECTOR_EDITOR_URL");
    env.unset();

    pulp::view::View view;
    view.set_source_loc({"app/Knob.tsx", 13, 4});

    InspectorConfig cfg;
    cfg.editor_url_template = "zed://file/{path}:{line}:{col}";
    auto result = resolve_source_jump(cfg, &view);
    REQUIRE(result.ok);
    REQUIRE(result.url == "zed://file/app/Knob.tsx:13:4");
}

TEST_CASE("resolve_source_jump uses the env-override template",
          "[inspect][source-jump]") {
    ScopedEnv env("PULP_INSPECTOR_EDITOR_URL");
    env.set("cursor://file/{path}:{line}");

    pulp::view::View view;
    view.set_source_loc({"x.jsx", 9, 0});

    InspectorConfig cfg;  // config says vscode; env wins
    auto result = resolve_source_jump(cfg, &view);
    REQUIRE(result.ok);
    REQUIRE(result.url == "cursor://file/x.jsx:9");
}

TEST_CASE("resolve_source_jump treats a 0-line as file-top (line 1)",
          "[inspect][source-jump]") {
    ScopedEnv env("PULP_INSPECTOR_EDITOR_URL");
    env.unset();

    pulp::view::View view;
    view.set_source_loc({"top.jsx", 0, 0});  // unknown line

    InspectorConfig cfg;
    auto result = resolve_source_jump(cfg, &view);
    REQUIRE(result.ok);
    // line/col in the result echo the recorded values...
    REQUIRE(result.line == 0);
    // ...but the formatted URL substitutes 1 so the editor opens cleanly.
    REQUIRE(result.url == "vscode://file/top.jsx:1");
}

TEST_CASE("resolve_source_jump is a graceful no-op for a null view",
          "[inspect][source-jump]") {
    InspectorConfig cfg;
    auto result = resolve_source_jump(cfg, nullptr);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.url.empty());
    REQUIRE_FALSE(result.error.empty());
}

TEST_CASE("resolve_source_jump is a graceful no-op for a view without provenance",
          "[inspect][source-jump]") {
    pulp::view::View view;  // no set_source_loc — non-imported view
    REQUIRE_FALSE(view.has_source_loc());

    InspectorConfig cfg;
    auto result = resolve_source_jump(cfg, &view);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.url.empty());
    REQUIRE(result.error.find("no source location") != std::string::npos);
}

TEST_CASE("jump_to_source with dry_run resolves but never launches",
          "[inspect][source-jump]") {
    ScopedEnv env("PULP_INSPECTOR_EDITOR_URL");
    env.unset();

    pulp::view::View view;
    view.set_source_loc({"src/Main.jsx", 100, 0});

    InspectorConfig cfg;
    auto result = jump_to_source(cfg, &view, /*dry_run=*/true);
    REQUIRE(result.ok);
    REQUIRE(result.url == "vscode://file/src/Main.jsx:100");
    REQUIRE_FALSE(result.launched);
}

TEST_CASE("launch_editor_url rejects an empty URL", "[inspect][source-jump]") {
    REQUIRE_FALSE(launch_editor_url(""));
}

// ── Phase 5.1: Inspector.jumpToSource protocol round-trip ──────────────────

TEST_CASE("Inspector.jumpToSource resolves an anchored view in dryRun",
          "[inspect][source-jump][protocol]") {
    ScopedEnv env("PULP_INSPECTOR_EDITOR_URL");
    env.unset();

    pulp::view::View root;
    auto child = std::make_unique<pulp::view::View>();
    child->set_anchor_id("figma:node-7");
    child->set_source_loc({"src/Panel.jsx", 24, 3});
    root.add_child(std::move(child));

    DomainHandler handler;
    handler.set_root_view(&root);

    auto resp = handler.handle(make_request(
        1, methods::kInspectorJumpToSource,
        R"({"anchorId":"figma:node-7","dryRun":true})"));
    REQUIRE_FALSE(resp.is_error);

    auto obj = choc::json::parse(resp.params_json);
    REQUIRE(obj["ok"].getBool());
    REQUIRE(std::string(obj["url"].getString())
            == "vscode://file/src/Panel.jsx:24");
    REQUIRE(std::string(obj["path"].getString()) == "src/Panel.jsx");
    REQUIRE(obj["line"].getInt64() == 24);
    REQUIRE_FALSE(obj["launched"].getBool());
    REQUIRE(obj["dryRun"].getBool());
}

TEST_CASE("Inspector.jumpToSource returns ok:false for an unknown anchor",
          "[inspect][source-jump][protocol]") {
    pulp::view::View root;
    DomainHandler handler;
    handler.set_root_view(&root);

    auto resp = handler.handle(make_request(
        1, methods::kInspectorJumpToSource,
        R"({"anchorId":"does-not-exist","dryRun":true})"));
    // Not a protocol error — a structured ok:false response.
    REQUIRE_FALSE(resp.is_error);
    auto obj = choc::json::parse(resp.params_json);
    REQUIRE_FALSE(obj["ok"].getBool());
    REQUIRE(obj.hasObjectMember("error"));
}

TEST_CASE("Inspector.jumpToSource returns ok:false when the view lacks provenance",
          "[inspect][source-jump][protocol]") {
    pulp::view::View root;
    auto child = std::make_unique<pulp::view::View>();
    child->set_anchor_id("anchor-no-source");
    // deliberately NO set_source_loc
    root.add_child(std::move(child));

    DomainHandler handler;
    handler.set_root_view(&root);

    auto resp = handler.handle(make_request(
        1, methods::kInspectorJumpToSource,
        R"({"anchorId":"anchor-no-source","dryRun":true})"));
    REQUIRE_FALSE(resp.is_error);
    auto obj = choc::json::parse(resp.params_json);
    REQUIRE_FALSE(obj["ok"].getBool());
    REQUIRE(std::string(obj["error"].getString()).find("no source location")
            != std::string::npos);
}

TEST_CASE("Inspector.jumpToSource rejects malformed params",
          "[inspect][source-jump][protocol]") {
    DomainHandler handler;
    auto resp = handler.handle(make_request(
        1, methods::kInspectorJumpToSource, "{not json"));
    REQUIRE(resp.is_error);
}
