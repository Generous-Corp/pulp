// Executed contract for the shipping transient-interaction prelude.

#include <catch2/catch_test_macros.hpp>
#include "test_helpers.hpp"

using namespace pulp::test;

TEST_CASE("WebCompat: transient interaction keeps live updates and commits latest once",
          "[webcompat][interaction][performance]") {
    TestEnvironment env;
    env.eval(R"JS(
        var __liveValues = [];
        var __committedValues = [];
        var __interaction = window.pulp.createTransientInteraction({
            onUpdate: function(value) { __liveValues.push(value); },
            onCommit: function(value) { __committedValues.push(value); }
        });
        var __session = __interaction.begin(0);
        for (var i = 1; i <= 180; ++i) __session.update(i);
        var __firstCommit = __session.commit();
        var __duplicateCommit = __session.commit();
    )JS");

    REQUIRE(env.engine.evaluate("__liveValues.length").getWithDefault<int32_t>(-1) == 180);
    REQUIRE(env.engine.evaluate("__liveValues[179]").getWithDefault<int32_t>(-1) == 180);
    REQUIRE(env.engine.evaluate("__committedValues.length").getWithDefault<int32_t>(-1) == 1);
    REQUIRE(env.engine.evaluate("__committedValues[0]").getWithDefault<int32_t>(-1) == 180);
    REQUIRE(env.engine.evaluate("__firstCommit").getWithDefault<bool>(false));
    REQUIRE_FALSE(env.engine.evaluate("__duplicateCommit").getWithDefault<bool>(true));
    REQUIRE_FALSE(env.engine.evaluate("__interaction.isActive()").getWithDefault<bool>(true));
}

TEST_CASE("WebCompat: a newer transient session rejects stale completion",
          "[webcompat][interaction][lifecycle]") {
    TestEnvironment env;
    env.eval(R"JS(
        var __commits = [];
        var __cancels = [];
        var __interaction2 = window.pulp.createTransientInteraction({
            onCommit: function(value) { __commits.push(value); },
            onCancel: function(value) { __cancels.push(value); }
        });
        var __oldSession = __interaction2.begin('old');
        var __newSession = __interaction2.begin('new');
        var __staleUpdate = __oldSession.update('stale');
        var __staleCommit = __oldSession.commit();
        var __newCommit = __newSession.commit('final');

        var __cancelSession = __interaction2.begin('cancelled');
        var __cancelled = __interaction2.cancel();
        var __duplicateControllerCancel = __interaction2.cancel();
        var __commitAfterCancel = __cancelSession.commit();
    )JS");

    REQUIRE_FALSE(env.engine.evaluate("__staleUpdate").getWithDefault<bool>(true));
    REQUIRE_FALSE(env.engine.evaluate("__staleCommit").getWithDefault<bool>(true));
    REQUIRE(env.engine.evaluate("__newCommit").getWithDefault<bool>(false));
    REQUIRE(std::string(env.engine.evaluate("__commits.join(',')").getWithDefault<std::string_view>("")) == "final");
    REQUIRE(env.engine.evaluate("__cancelled").getWithDefault<bool>(false));
    REQUIRE_FALSE(env.engine.evaluate("__duplicateControllerCancel").getWithDefault<bool>(true));
    REQUIRE_FALSE(env.engine.evaluate("__commitAfterCancel").getWithDefault<bool>(true));
    REQUIRE(std::string(env.engine.evaluate("__cancels.join(',')").getWithDefault<std::string_view>("")) == "cancelled");
}

TEST_CASE("WebCompat: transient callback failures leave lifecycle state closed or owned",
          "[webcompat][interaction][lifecycle]") {
    TestEnvironment env;
    env.eval(R"JS(
        var __updateThrows = window.pulp.createTransientInteraction({
            onUpdate: function() { throw new Error('update'); }
        });
        var __updateSession = __updateThrows.begin(1);
        try { __updateSession.update(2); } catch (_) {}
        var __activeAfterUpdateThrow = __updateSession.isActive();

        var __commitCalls = 0;
        var __commitThrows = window.pulp.createTransientInteraction({
            onCommit: function() { __commitCalls++; throw new Error('commit'); }
        });
        var __commitSession = __commitThrows.begin(3);
        try { __commitSession.commit(); } catch (_) {}
        var __activeAfterCommitThrow = __commitThrows.isActive();
        var __secondCommitAfterThrow = __commitSession.commit();

        var __cancelCalls = 0;
        var __cancelThrows = window.pulp.createTransientInteraction({
            onCancel: function() { __cancelCalls++; throw new Error('cancel'); }
        });
        var __cancelThrowSession = __cancelThrows.begin(4);
        try { __cancelThrowSession.cancel(); } catch (_) {}
        var __activeAfterCancelThrow = __cancelThrows.isActive();
        var __secondCancelAfterThrow = __cancelThrowSession.cancel();
    )JS");

    REQUIRE(env.engine.evaluate("__activeAfterUpdateThrow").getWithDefault<bool>(false));
    REQUIRE_FALSE(env.engine.evaluate("__activeAfterCommitThrow").getWithDefault<bool>(true));
    REQUIRE(env.engine.evaluate("__commitCalls").getWithDefault<int32_t>(-1) == 1);
    REQUIRE_FALSE(env.engine.evaluate("__secondCommitAfterThrow").getWithDefault<bool>(true));
    REQUIRE_FALSE(env.engine.evaluate("__activeAfterCancelThrow").getWithDefault<bool>(true));
    REQUIRE(env.engine.evaluate("__cancelCalls").getWithDefault<int32_t>(-1) == 1);
    REQUIRE_FALSE(env.engine.evaluate("__secondCancelAfterThrow").getWithDefault<bool>(true));
}
