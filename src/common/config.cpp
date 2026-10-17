#include "config.h"
#include <fstream>
#include <set>
#include <cmath>

namespace euphony {
namespace {
std::string path_string(const std::filesystem::path& path) { return utf8(path.wstring()); }
void integer(const nlohmann::json& c, const char* key, int low, int high) {
    if (!c.contains(key) || !c[key].is_number_integer()) throw std::runtime_error(std::string(key) + " must be an integer");
    const auto value = c[key].get<std::int64_t>();
    if (value < low || value > high) throw std::runtime_error(std::string(key) + " is outside supported range");
}
void choice(const nlohmann::json& c, const char* key, const std::set<std::string>& values) {
    if (!c.contains(key) || !c[key].is_string() || !values.contains(c[key].get<std::string>()))
        throw std::runtime_error(std::string(key) + " is not supported");
}
}
nlohmann::json default_config() {
    const auto bin = module_directory();
    std::filesystem::path core = bin / L"phono_core.dll";
    std::filesystem::path model;
    return {{"schema_version", 1}, {"model_directory", path_string(model)}, {"core_library", path_string(core)},
        {"dictionary_path", path_string(bin / L"data/libime.tsv")}, {"language_model_path", path_string(bin / L"data/zh_CN.lm")},
        {"prefer_unigram_single_syllable", true}, {"user_words_exact_only", true}, {"user_words_enabled", true}, {"user_words_first", true},
        {"input_mode", "pinyin"}, {"shuangpin_scheme", "xiaohe"},
        {"page_size", 5}, {"candidate_layout", "vertical"}, {"model_candidate_color", "#DFF2E5"}, {"user_candidate_color", "#FBE4EC"}, {"model_enabled", true}, {"use_surrounding", true}, {"history_characters", 256},
        {"learning", true}, {"chinese_punctuation", true}, {"full_width", false}, {"invalid_input", "pass"}, {"context_slots", 4}, {"user_directory", path_string(user_directory())},
        {"engine", {{"schema_version", "1.0"}, {"tokenizer", {{"normalization", {{"lowercase_ascii", true},
            {"normalize_v_to_u", true}, {"separators", "'"}}}, {"segment_mode", "safe"}, {"use_segmenter", true},
            {"repair", false}, {"max_pinyin_chars", 128}}}}},
        {"context", {{"schema_version", "1.0"}, {"min_accept_context", 8}, {"trial_ratio", 0.25},
            {"decay_alpha", 0.5}, {"decay_lambda", 0.02}}}};
}
void validate_config(const nlohmann::json& c) {
    if (!c.is_object() || c.value("schema_version", 0) != 1) throw std::runtime_error("Unsupported configuration version");
    for (const char* key : {"model_directory", "core_library", "dictionary_path", "language_model_path", "user_directory"}) {
        if (!c.contains(key) || !c[key].is_string()) throw std::runtime_error(std::string(key) + " must be a string");
        const auto value = c[key].get<std::string>();
        if (value.find('\0') != std::string::npos || value.size() > 32767) throw std::runtime_error("Invalid path");
        if (!value.empty() && !std::filesystem::path(wide(value)).is_absolute()) throw std::runtime_error(std::string(key) + " must be absolute");
    }
    for (const char* key : {"model_enabled", "use_surrounding", "learning", "chinese_punctuation", "full_width", "prefer_unigram_single_syllable", "user_words_exact_only", "user_words_enabled", "user_words_first"})
        if (!c.contains(key) || !c[key].is_boolean()) throw std::runtime_error(std::string(key) + " must be boolean");
    integer(c, "page_size", 3, 10);
    integer(c, "history_characters", 0, 8192);
    integer(c, "context_slots", 1, 32);
    choice(c, "input_mode", {"pinyin", "shuangpin"});
    choice(c, "candidate_layout", {"vertical", "horizontal"});
    for (const auto* key : {"model_candidate_color", "user_candidate_color"}) {
        if (!c.contains(key) || !c[key].is_string()) throw std::runtime_error("Candidate color must be #RRGGBB");
        const auto color=c[key].get<std::string>();
        if (color.size()!=7 || color[0]!='#' || color.find_first_not_of("0123456789abcdefABCDEF",1)!=std::string::npos)
            throw std::runtime_error("Candidate color must be #RRGGBB");
    }
    choice(c, "shuangpin_scheme", {"ziranma", "ms", "ziguang", "abc", "zhongwenzhixing", "pinyinjiajia", "xiaohe"});
    choice(c, "invalid_input", {"pass", "delete"});
    if (!c.at("engine").is_object() || c.at("engine").value("schema_version", "") != "1.0" ||
        !c.at("context").is_object() || c.at("context").value("schema_version", "") != "1.0")
        throw std::runtime_error("Invalid engine/context configuration version");
    const auto& tokenizer = c.at("engine").at("tokenizer");
    integer(tokenizer, "max_pinyin_chars", 1, 4096);
    const auto& context = c.at("context");
    integer(context, "min_accept_context", 1, 8192);
    for (const char* key : {"trial_ratio", "decay_alpha", "decay_lambda"}) {
        if (!context.contains(key) || !context[key].is_number()) throw std::runtime_error("Context parameter must be numeric");
        const double value = context[key].get<double>();
        if (!std::isfinite(value) || value < 0 || (std::string(key) == "trial_ratio" && (value <= 0 || value > 1)))
            throw std::runtime_error("Context parameter out of range");
    }
    for (const char* key : {"max_context_length", "max_history_length", "max_pinyin_length"})
        if (context.contains(key)) integer(context, key, 1, 8192);
    if (context.contains("slack_interval")) integer(context, "slack_interval", 0, 8192);
}
ConfigStore::ConfigStore() : value_(default_config()) {
    std::filesystem::create_directories(user_directory());
    const auto path = user_directory() / L"config.json";
    if (std::filesystem::exists(path)) {
        try {
            std::ifstream stream(path);
            nlohmann::json stored;
            stream >> stored;
            auto merged = value_;
            merged.merge_patch(stored);
            merged["user_directory"] = path_string(user_directory());
            validate_config(merged);
            value_ = std::move(merged);
        } catch (const std::exception&) {
            // Keep the file intact until the user explicitly saves a correction.
            // A damaged configuration must not prevent opening its settings UI.
            load_error_ = "The saved configuration is invalid; defaults are active until settings are saved";
        }
    }
    value_["user_directory"] = path_string(user_directory());
    validate_config(value_);
}
nlohmann::json ConfigStore::prepare(const nlohmann::json& input) const {
    if (!input.is_object()) throw std::runtime_error("Configuration must be an object");
    auto next = value_;
    next.merge_patch(input);
    next["user_directory"] = path_string(user_directory());
    validate_config(next);
    return next;
}
void ConfigStore::save(const nlohmann::json& value) {
    validate_config(value);
    auto temp = user_directory() / (L"config." + std::to_wstring(GetCurrentProcessId()) + L".tmp");
    const auto destination = user_directory() / L"config.json";
    {
        std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
        stream << value.dump(2) << '\n';
        stream.flush();
        if (!stream) throw std::runtime_error("Cannot save configuration");
    }
    if (!MoveFileExW(temp.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        throw std::runtime_error("Cannot replace configuration");
    }
    value_ = value;
    load_error_.clear();
}
}
