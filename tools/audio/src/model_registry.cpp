#include <pulp/tools/audio/model_registry.hpp>

namespace pulp::tools::audio {

const std::vector<RegisteredModel>& registered_models() {
    static const std::vector<RegisteredModel> models = {{
        .model_id = "clap_music_audioset_v1",
        .display_name = "CLAP Music AudioSet",
        .backend = "clap",
        .checkpoint_ref = "hf://lukewys/laion_clap/music.pt",
        .task_tags = {"music", "excerpt_find"},
        .size_bytes = 2523293286ULL,
        .download_url = "https://huggingface.co/lukewys/laion_clap/resolve/main/music.pt",
        .sha256 = "",  // Populated after first verified download
        .auto_downloadable = true,
    }};
    return models;
}

const RegisteredModel* find_registered_model(std::string_view model_id) {
    for (const auto& model : registered_models()) {
        if (model.model_id == model_id) return &model;
    }
    return nullptr;
}

std::string resolve_checkpoint_url(const std::string& checkpoint_ref) {
    // Handle hf:// protocol
    if (checkpoint_ref.starts_with("hf://")) {
        auto path = checkpoint_ref.substr(5);  // after "hf://"
        return "https://huggingface.co/" + path.substr(0, path.find('/')) + "/"
             + path.substr(path.find('/') + 1, path.rfind('/') - path.find('/') - 1)
             + "/resolve/main/"
             + path.substr(path.rfind('/') + 1);
    }
    // Handle direct URLs
    if (checkpoint_ref.starts_with("http://") || checkpoint_ref.starts_with("https://"))
        return checkpoint_ref;
    return {};
}

} // namespace pulp::tools::audio
