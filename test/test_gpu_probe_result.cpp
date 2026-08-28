#include <pulp_tooling/gpu_probe/probe_result.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <string>

namespace gp = pulp::tooling::gpu_probe;

namespace {

gp::ProbeResult passing_result(std::string_view id = gp::kRecipeIds[1]) {
    const auto* recipe = gp::find_recipe(id);
    REQUIRE(recipe != nullptr);

    gp::ProbeResult result;
    result.gpu_evidence_id = "0123456789abcdef0123456789abcdef";
    result.recipe_id = std::string(recipe->id);
    result.source_digest = std::string(64, 'a');
    result.signature_digest = std::string(64, 'b');
    result.dimensions = recipe->dimensions;
    result.seed = recipe->seed;
    result.clock = recipe->clock;
    result.input_format = recipe->input_format;
    result.output_format = recipe->output_format;
    result.encoding = recipe->encoding;
    result.tolerance = recipe->tolerance;
    result.adapter_policy = recipe->adapter_policy;
    result.adapter.status = gp::IdentityStatus::authentic;
    result.adapter.classification = gp::AdapterClass::hardware;
    result.adapter.backend = "Metal";
    result.numeric_sample_count = 16;
    result.verdict = gp::Verdict::pass;
    for (std::size_t i = 0; i < recipe->semantic_passes.size(); ++i) {
        result.passes.push_back({static_cast<std::uint32_t>(i),
                                 std::string(recipe->semantic_passes[i]),
                                 gp::Verdict::pass, true, {}, {}, {},
                                 "gpu.probe.pass"});
    }
    result.artifacts.push_back({"values.json", gp::ArtifactKind::json,
                                "application/json", 128, std::string(64, 'c')});
    return result;
}

} // namespace

TEST_CASE("GPU probe registry freezes four versioned recipes", "[gpu][probe][contract]") {
    const auto recipes = gp::recipes();
    REQUIRE(recipes.size() == gp::kRecipeIds.size());
    for (std::size_t i = 0; i < recipes.size(); ++i) {
        CHECK(recipes[i].id == gp::kRecipeIds[i]);
        CHECK_FALSE(recipes[i].semantic_passes.empty());
        CHECK_FALSE(recipes[i].positive_control.empty());
        CHECK_FALSE(recipes[i].negative_mutation.empty());
        CHECK(recipes[i].dimensions.width <= gp::kMaxDimension);
        CHECK(recipes[i].dimensions.height <= gp::kMaxDimension);
        CHECK(recipes[i].dimensions.work_items <= gp::kMaxWorkItems);
    }
}

TEST_CASE("GPU probe result binds execution identity and correctness to its recipe",
          "[gpu][probe][contract]") {
    std::string error;
    auto result = passing_result();
    REQUIRE(gp::validate(result, &error));

    result.seed++;
    REQUIRE_FALSE(gp::validate(result, &error));
    CHECK(error.find("execution identity") != std::string::npos);

    result = passing_result();
    result.passes.back().work_completed = false;
    REQUIRE_FALSE(gp::validate(result, &error));
    CHECK(error.find("work completed") != std::string::npos);

    result = passing_result();
    result.passes.back().verdict = gp::Verdict::fail;
    REQUIRE_FALSE(gp::validate(result, &error));
    CHECK(error.find("aggregate") != std::string::npos);

    result = passing_result();
    result.adapter.status = gp::IdentityStatus::unverified;
    REQUIRE_FALSE(gp::validate(result, &error));
    CHECK(error.find("authentic hardware") != std::string::npos);

    result = passing_result();
    result.adapter.classification = gp::AdapterClass::null_adapter;
    REQUIRE_FALSE(gp::validate(result, &error));
    CHECK(error.find("hardware") != std::string::npos);
}

TEST_CASE("GPU probe verdict and numeric evidence aggregate coherently",
          "[gpu][probe][contract]") {
    std::string error;
    auto result = passing_result();
    result.verdict = gp::Verdict::fail;
    REQUIRE_FALSE(gp::validate(result, &error));
    CHECK(error.find("aggregate") != std::string::npos);

    result = passing_result();
    result.passes.back().expected = std::numeric_limits<double>::infinity();
    result.passes.back().observed = 1.0;
    result.passes.back().absolute_error = 1.0;
    REQUIRE_FALSE(gp::validate(result, &error));
    CHECK(error.find("finite") != std::string::npos);

    result = passing_result();
    result.passes.back().expected = 2.0;
    result.passes.back().observed = 5.0;
    result.passes.back().absolute_error = 2.0;
    REQUIRE_FALSE(gp::validate(result, &error));
    CHECK(error.find("observed minus expected") != std::string::npos);

    result = passing_result();
    result.passes.back().verdict = gp::Verdict::unverified;
    result.verdict = gp::Verdict::unverified;
    REQUIRE(gp::validate(result, &error));
}

TEST_CASE("GPU probe artifacts are confined, unique, and bounded",
          "[gpu][probe][contract]") {
    std::string error;
    auto result = passing_result();

    result.artifacts[0].name = "../escaped.json";
    REQUIRE_FALSE(gp::validate(result, &error));
    CHECK(error.find("confined") != std::string::npos);

    result = passing_result();
    result.artifacts.push_back(result.artifacts.front());
    REQUIRE_FALSE(gp::validate(result, &error));
    CHECK(error.find("unique") != std::string::npos);

    result = passing_result();
    result.artifacts[0].bytes = gp::kMaxArtifactBytes;
    REQUIRE(gp::validate(result, &error));
    result.artifacts[0].bytes++;
    REQUIRE_FALSE(gp::validate(result, &error));
    CHECK(error.find("per-artifact") != std::string::npos);

    result = passing_result();
    result.artifacts.clear();
    for (int i = 0; i < 5; ++i) {
        result.artifacts.push_back({"artifact-" + std::to_string(i) + ".bin",
                                    gp::ArtifactKind::numeric_samples,
                                    "application/octet-stream", gp::kMaxArtifactBytes,
                                    std::string(64, static_cast<char>('a' + i))});
    }
    REQUIRE_FALSE(gp::validate(result, &error));
    CHECK(error.find("total byte") != std::string::npos);

    result = passing_result();
    result.numeric_sample_count = gp::kMaxNumericSamples;
    REQUIRE(gp::validate(result, &error));
    result.numeric_sample_count++;
    REQUIRE_FALSE(gp::validate(result, &error));
    CHECK(error.find("numeric sample") != std::string::npos);
}

TEST_CASE("GPU probe evidence identifiers and digests are closed",
          "[gpu][probe][contract]") {
    std::string error;
    auto result = passing_result();
    result.gpu_evidence_id[0] = 'A';
    REQUIRE_FALSE(gp::validate(result, &error));
    CHECK(error.find("lowercase hex") != std::string::npos);

    result = passing_result();
    result.source_digest.pop_back();
    REQUIRE_FALSE(gp::validate(result, &error));
    CHECK(error.find("SHA-256") != std::string::npos);
}

TEST_CASE("GPU probe JSON and human projections preserve typed evidence",
          "[gpu][probe][contract]") {
    const auto result = passing_result();
    const auto compact = gp::to_json(result);
    CHECK(compact.find("\"schema\":\"pulp.gpu-probe-result.v1\"") !=
          std::string::npos);
    CHECK(compact.find("\"recipe_id\":\"gpu-compute.magnitude.v1\"") !=
          std::string::npos);
    CHECK(compact.find("\"work_completed\":true") != std::string::npos);
    CHECK(compact.find('\n') == std::string::npos);

    const auto pretty = gp::to_json(result, true);
    CHECK(pretty.find('\n') != std::string::npos);
    const auto human = gp::render_human(result);
    CHECK(human.find("GPU probe: gpu-compute.magnitude.v1") != std::string::npos);
    CHECK(human.find("artifact: values.json") != std::string::npos);
    CHECK(gp::exit_code(result) == 0);

    auto failed = result;
    failed.passes.back().verdict = gp::Verdict::fail;
    failed.verdict = gp::Verdict::fail;
    CHECK(gp::exit_code(failed) == 1);
}

TEST_CASE("GPU probe JSON parser round-trips and rejects schema drift",
          "[gpu][probe][contract]") {
    const auto expected = passing_result();
    std::string error;
    const auto parsed = gp::from_json(gp::to_json(expected), &error);
    REQUIRE(parsed.has_value());
    CHECK(error.empty());
    CHECK(parsed->gpu_evidence_id == expected.gpu_evidence_id);
    CHECK(parsed->recipe_id == expected.recipe_id);
    CHECK(parsed->adapter.backend == expected.adapter.backend);
    CHECK(parsed->passes.size() == expected.passes.size());
    CHECK(parsed->artifacts.size() == expected.artifacts.size());
    CHECK(gp::to_json(*parsed) == gp::to_json(expected));

    auto unknown = gp::to_json(expected);
    unknown.insert(unknown.size() - 1, R"JSON(,"unexpected":true)JSON");
    CHECK_FALSE(gp::from_json(unknown, &error).has_value());
    CHECK(error.find("unknown member") != std::string::npos);

    auto wrong_type = gp::to_json(expected);
    const auto offset = wrong_type.find(R"JSON("version":1)JSON");
    REQUIRE(offset != std::string::npos);
    wrong_type.replace(offset, std::string(R"JSON("version":1)JSON").size(),
                       R"JSON("version":"1")JSON");
    CHECK_FALSE(gp::from_json(wrong_type, &error).has_value());
    CHECK(error.find("non-negative integer") != std::string::npos);
}
