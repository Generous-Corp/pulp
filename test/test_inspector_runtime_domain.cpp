#include <catch2/catch_test_macros.hpp>
#include <pulp/inspect/domain_handler.hpp>
#include <pulp/inspect/protocol.hpp>

#include <choc/text/choc_JSON.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

using namespace pulp::inspect;

// ── Runtime domain: safe router/status around an injected evaluator ──────────

namespace {
class FakeRuntimeEvaluator final : public RuntimeEvaluator {
public:
    RuntimeEvaluatorCapabilities capabilities() const override {
        return {.engine = "QuickJS",
                .can_evaluate = true,
                .can_interrupt = true};
    }
    RuntimeEvaluationResult evaluate(std::string_view code, std::chrono::milliseconds,
                                     std::size_t) override {
        ++evaluate_calls;
        last_code = code;
        if (next_result) {
            auto result = std::move(*next_result);
            next_result.reset();
            return result;
        }
        if (code.find("throw") != std::string_view::npos)
            return {.error = "boom"};
        if (code == "40 + 2")
            return {.ok = true, .json = "42"};
        if (code == "'ab' + 'cd'")
            return {.ok = true, .json = R"("abcd")"};
        return {.ok = true, .json = "null"};
    }
    bool interrupt() override { return interrupt_result; }
    std::string_view binary_marker() const noexcept override {
        return "test-only-fake";
    }

    int evaluate_calls = 0;
    std::string last_code;
    bool interrupt_result = false;
    std::optional<RuntimeEvaluationResult> next_result;
};
} // namespace

TEST_CASE("DomainHandler: Runtime.getCapabilities reports QuickJS honestly",
          "[inspect][domain][runtime]") {
    FakeRuntimeEvaluator evaluator;
    DomainHandler handler;
    handler.set_runtime_evaluator(&evaluator);
    handler.set_runtime_eval_enabled(true);  // opt in so canEvaluate is true

    auto resp = handler.handle(make_request(1, methods::kRuntimeGetCapabilities));
    REQUIRE_FALSE(resp.is_error);
    auto caps = choc::json::parse(resp.params_json);
    REQUIRE(caps["engine"].toString() == "QuickJS");
    REQUIRE(caps["attached"].getWithDefault(false));
    REQUIRE(caps["canEvaluate"].getWithDefault(false));
    REQUIRE(caps["canInterrupt"].getWithDefault(false));
    // Mainline QuickJS has no source-line debug protocol — must report false.
    REQUIRE_FALSE(caps["canBreak"].getWithDefault(true));
    REQUIRE_FALSE(caps["canStep"].getWithDefault(true));
    REQUIRE_FALSE(caps["canInspectLocals"].getWithDefault(true));
}

TEST_CASE("DomainHandler: Runtime.getCapabilities without an engine",
          "[inspect][domain][runtime]") {
    DomainHandler handler;  // no script inspector attached
    auto resp = handler.handle(make_request(1, methods::kRuntimeGetCapabilities));
    REQUIRE_FALSE(resp.is_error);
    auto caps = choc::json::parse(resp.params_json);
    REQUIRE(caps["engine"].toString().empty());
    REQUIRE_FALSE(caps["attached"].getWithDefault(true));
    REQUIRE_FALSE(caps["canEvaluate"].getWithDefault(true));
}

TEST_CASE("DomainHandler: Runtime.evaluate returns a typed result",
          "[inspect][domain][runtime]") {
    FakeRuntimeEvaluator evaluator;
    DomainHandler handler;
    handler.set_runtime_evaluator(&evaluator);
    handler.set_runtime_eval_enabled(true);

    auto resp = handler.handle(make_request(1, methods::kRuntimeEvaluate,
                                            R"({"code":"40 + 2"})"));
    REQUIRE_FALSE(resp.is_error);
    auto result = choc::json::parse(resp.params_json);
    REQUIRE(result["result"].getWithDefault(0) == 42);

    // CDP-compatible alias 'expression'.
    auto aliased = handler.handle(make_request(2, methods::kRuntimeEvaluate,
                                               R"({"expression":"'ab' + 'cd'"})"));
    REQUIRE_FALSE(aliased.is_error);
    auto astr = choc::json::parse(aliased.params_json);
    REQUIRE(astr["result"].toString() == "abcd");
}

TEST_CASE("DomainHandler: Runtime.evaluate surfaces JS errors and bad params",
          "[inspect][domain][runtime]") {
    FakeRuntimeEvaluator evaluator;
    DomainHandler handler;
    handler.set_runtime_evaluator(&evaluator);
    handler.set_runtime_eval_enabled(true);

    auto thrown = handler.handle(make_request(1, methods::kRuntimeEvaluate,
                                              R"json({"code":"throw new Error('boom')"})json"));
    REQUIRE(thrown.is_error);
    REQUIRE(thrown.params_json.find("boom") != std::string::npos);

    auto empty = handler.handle(make_request(2, methods::kRuntimeEvaluate, R"({"code":""})"));
    REQUIRE(empty.is_error);

    auto bad = handler.handle(make_request(3, methods::kRuntimeEvaluate, "{"));
    REQUIRE(bad.is_error);
}

TEST_CASE("DomainHandler: Runtime.evaluate rejects malformed and evasive payloads",
          "[inspect][domain][runtime][negative][limits]") {
    FakeRuntimeEvaluator evaluator;
    DomainHandler handler;
    handler.set_runtime_evaluator(&evaluator);
    handler.set_runtime_eval_enabled(true);

    for (const auto& params : {
             std::string("null"), std::string("[]"), std::string("{}"),
             std::string(R"({"code":7})"), std::string(R"({"expression":false})"),
             std::string("{\"code\":")}) {
        const auto response = handler.handle(
            make_request(1, methods::kRuntimeEvaluate, params));
        REQUIRE(response.is_error);
    }
    REQUIRE(evaluator.evaluate_calls == 0);

    const std::string at_limit(kRuntimeEvalMaxCodeBytes, 'a');
    auto accepted = handler.handle(make_request(
        2, methods::kRuntimeEvaluate, "{\"code\":\"" + at_limit + "\"}"));
    REQUIRE_FALSE(accepted.is_error);
    REQUIRE(evaluator.last_code.size() == kRuntimeEvalMaxCodeBytes);

    const std::string over_limit(kRuntimeEvalMaxCodeBytes + 1, 'a');
    auto oversized = handler.handle(make_request(
        3, methods::kRuntimeEvaluate, "{\"code\":\"" + over_limit + "\"}"));
    REQUIRE(oversized.is_error);
    REQUIRE(oversized.params_json ==
            "Runtime.evaluate code exceeds the 65536-byte limit");

    std::string escaped = "{\"expression\":\"";
    escaped.reserve(escaped.size() + (kRuntimeEvalMaxCodeBytes + 1) * 6 + 2);
    for (std::size_t i = 0; i <= kRuntimeEvalMaxCodeBytes; ++i)
        escaped += "\\u0061";
    escaped += "\"}";
    auto decoded_oversized = handler.handle(
        make_request(4, methods::kRuntimeEvaluate, escaped));
    REQUIRE(decoded_oversized.is_error);
    REQUIRE(decoded_oversized.params_json ==
            "Runtime.evaluate code exceeds the 65536-byte limit");

    auto nul = handler.handle(make_request(
        5, methods::kRuntimeEvaluate, R"({"code":"a\u0000b"})"));
    REQUIRE(nul.is_error);
    REQUIRE(evaluator.evaluate_calls == 1);
}

TEST_CASE("DomainHandler: Runtime.evaluate caps the encoded response",
          "[inspect][domain][runtime][negative][limits]") {
    FakeRuntimeEvaluator evaluator;
    evaluator.next_result = RuntimeEvaluationResult{
        .ok = true,
        .json = "\"" + std::string(kRuntimeEvalMaxResponseBytes, 'x') + "\""};
    DomainHandler handler;
    handler.set_runtime_evaluator(&evaluator);
    handler.set_runtime_eval_enabled(true);

    const auto response = handler.handle(make_request(
        1, methods::kRuntimeEvaluate, R"({"code":"large"})"));
    REQUIRE(response.is_error);
    REQUIRE(response.params_json ==
            "Runtime.evaluate response exceeds the 1048576-byte limit");
    REQUIRE(encode_message(response).size() < kRuntimeEvalMaxResponseBytes);
}

TEST_CASE("DomainHandler: Runtime.evaluate is disabled by default (security)",
          "[inspect][domain][runtime]") {
    // Even with an engine wired, evaluate must not run until the host opts in —
    // the inspector transport is unauthenticated, and evaluate is RCE.
    FakeRuntimeEvaluator evaluator;
    DomainHandler handler;
    handler.set_runtime_evaluator(&evaluator);  // wired but NOT enabled

    auto disabled = handler.handle(make_request(1, methods::kRuntimeEvaluate,
                                                R"({"code":"1+1"})"));
    REQUIRE(disabled.is_error);
    REQUIRE(disabled.params_json.find("disabled") != std::string::npos);

    // getCapabilities reports the honest false so a client never tries.
    auto caps = choc::json::parse(
        handler.handle(make_request(2, methods::kRuntimeGetCapabilities)).params_json);
    REQUIRE(caps["attached"].getWithDefault(false));
    REQUIRE_FALSE(caps["canEvaluate"].getWithDefault(true));

    // Enabled but no engine → unavailable (distinct from disabled).
    DomainHandler enabled_no_engine;
    enabled_no_engine.set_runtime_eval_enabled(true);
    auto no_engine = enabled_no_engine.handle(make_request(3, methods::kRuntimeEvaluate,
                                                           R"({"code":"1+1"})"));
    REQUIRE(no_engine.is_error);
    REQUIRE(no_engine.params_json.find("no scripted-UI engine") != std::string::npos);
}

TEST_CASE("DomainHandler: Runtime.evaluate never breaks response framing (NaN/Infinity)",
          "[inspect][domain][runtime]") {
    FakeRuntimeEvaluator evaluator;
    DomainHandler handler;
    handler.set_runtime_evaluator(&evaluator);
    handler.set_runtime_eval_enabled(true);

    // 1/0 → Infinity; ({x:0/0}) → nested NaN. Whichever the engine yields, the
    // response must remain parseable JSON (no bare NaN/Infinity token).
    for (const char* expr : {R"json({"code":"1/0"})json", R"json({"code":"({x: 0/0})"})json"}) {
        auto resp = handler.handle(make_request(1, methods::kRuntimeEvaluate, expr));
        if (!resp.is_error) {
            // params_json must parse; the runtime inspector guarantees valid JSON.
            REQUIRE_NOTHROW(choc::json::parse(resp.params_json));
        }
    }
}

TEST_CASE("DomainHandler: Runtime.interrupt reports whether it armed",
          "[inspect][domain][runtime]") {
    // No engine → cannot interrupt.
    DomainHandler bare;
    auto none = bare.handle(make_request(1, methods::kRuntimeInterrupt));
    REQUIRE_FALSE(none.is_error);
    REQUIRE_FALSE(choc::json::parse(none.params_json)["interrupted"].getWithDefault(true));

    // Engine attached but nothing in flight → interrupt is a no-op (false),
    // which prevents spuriously aborting the next evaluation.
    FakeRuntimeEvaluator evaluator;
    DomainHandler handler;
    handler.set_runtime_evaluator(&evaluator);
    handler.set_runtime_eval_enabled(true);  // reach the idle-guard, not the gate
    auto idle = handler.handle(make_request(2, methods::kRuntimeInterrupt));
    REQUIRE_FALSE(idle.is_error);
    REQUIRE_FALSE(choc::json::parse(idle.params_json)["interrupted"].getWithDefault(true));
}
