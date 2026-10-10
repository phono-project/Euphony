// Adapted from phono-fcitx5-addon; Apache-2.0. Dynamic C ABI loading replaces link-time calls.
#include "worker.h"
#include "presentation.h"
#include "common/win_util.h"
#include <phono_api.h>
#include <windows.h>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace euphony {
namespace {
struct Api {
    HMODULE library = nullptr;
    decltype(&::phono_engine_create) phono_engine_create = nullptr;
    decltype(&::phono_engine_destroy) phono_engine_destroy = nullptr;
    decltype(&::phono_engine_pre_max_seqlen) phono_engine_pre_max_seqlen = nullptr;
    decltype(&::phono_engine_post_max_seqlen) phono_engine_post_max_seqlen = nullptr;
    decltype(&::phono_session_beam_size) phono_session_beam_size = nullptr;
    decltype(&::phono_engine_segment_pinyin) phono_engine_segment_pinyin = nullptr;
    decltype(&::phono_tokenizer_find_pinyin_id_exact) phono_tokenizer_find_pinyin_id_exact = nullptr;
    decltype(&::phono_tokenizer_encode_context) phono_tokenizer_encode_context = nullptr;
    decltype(&::phono_context_manager_create) phono_context_manager_create = nullptr;
    decltype(&::phono_context_manager_destroy) phono_context_manager_destroy = nullptr;
    decltype(&::phono_context_manager_get_auto) phono_context_manager_get_auto = nullptr;
    decltype(&::phono_session_create) phono_session_create = nullptr;
    decltype(&::phono_session_destroy) phono_session_destroy = nullptr;
    decltype(&::phono_session_replace_context) phono_session_replace_context = nullptr;
    decltype(&::phono_session_generate) phono_session_generate = nullptr;
    decltype(&::phono_generate_result_free) phono_generate_result_free = nullptr;
    decltype(&::phono_free) phono_free = nullptr;
    decltype(&::phono_error_name) phono_error_name = nullptr;
    decltype(&::phono_last_error_message) phono_last_error_message = nullptr;
    explicit Api(const std::string& filename) {
        auto path = std::filesystem::absolute(std::filesystem::path(wide(filename)));
        library = LoadLibraryExW(path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!library) throw std::runtime_error("Cannot load phono-core library (Windows error " + std::to_string(GetLastError()) + "): " + filename);
        try {
            phono_engine_create = symbol<decltype(phono_engine_create)>("phono_engine_create");
            phono_engine_destroy = symbol<decltype(phono_engine_destroy)>("phono_engine_destroy");
            phono_engine_pre_max_seqlen = symbol<decltype(phono_engine_pre_max_seqlen)>("phono_engine_pre_max_seqlen");
            phono_engine_post_max_seqlen = symbol<decltype(phono_engine_post_max_seqlen)>("phono_engine_post_max_seqlen");
            phono_session_beam_size = symbol<decltype(phono_session_beam_size)>("phono_session_beam_size");
            phono_engine_segment_pinyin = symbol<decltype(phono_engine_segment_pinyin)>("phono_engine_segment_pinyin");
            phono_tokenizer_find_pinyin_id_exact = symbol<decltype(phono_tokenizer_find_pinyin_id_exact)>("phono_tokenizer_find_pinyin_id_exact");
            phono_tokenizer_encode_context = symbol<decltype(phono_tokenizer_encode_context)>("phono_tokenizer_encode_context");
            phono_context_manager_create = symbol<decltype(phono_context_manager_create)>("phono_context_manager_create");
            phono_context_manager_destroy = symbol<decltype(phono_context_manager_destroy)>("phono_context_manager_destroy");
            phono_context_manager_get_auto = symbol<decltype(phono_context_manager_get_auto)>("phono_context_manager_get_auto");
            phono_session_create = symbol<decltype(phono_session_create)>("phono_session_create");
            phono_session_destroy = symbol<decltype(phono_session_destroy)>("phono_session_destroy");
            phono_session_replace_context = symbol<decltype(phono_session_replace_context)>("phono_session_replace_context");
            phono_session_generate = symbol<decltype(phono_session_generate)>("phono_session_generate");
            phono_generate_result_free = symbol<decltype(phono_generate_result_free)>("phono_generate_result_free");
            phono_free = symbol<decltype(phono_free)>("phono_free");
            phono_error_name = symbol<decltype(phono_error_name)>("phono_error_name");
            phono_last_error_message = symbol<decltype(phono_last_error_message)>("phono_last_error_message");
        } catch (...) { FreeLibrary(library); library = nullptr; throw; }
    }
    ~Api() { if (library) FreeLibrary(library); }
    template<class T> T symbol(const char* name) {
        auto address = GetProcAddress(library, name);
        if (!address) throw std::runtime_error(std::string("phono-core ABI symbol missing: ") + name);
        return reinterpret_cast<T>(address);
    }
};
void check(const Api& api, phono_status status) {
    if (status != PHONO_OK) {
        throw std::runtime_error(std::string(api.phono_error_name(status)) + ": " +
                                 api.phono_last_error_message());
    }
}
struct Free { const Api* api; void operator()(void *p) const { api->phono_free(p); } };
template <class T> using Buffer = std::unique_ptr<T, Free>;
int cancel(void *p) { return static_cast<std::atomic_bool *>(p)->load() ? 1 : 0; }
struct Generated {
    const Api& api;
    phono_generate_result value{};
    ~Generated() { api.phono_generate_result_free(&value); }
};
struct Handles {
    Api api;
    explicit Handles(const std::string& library) : api(library) {}
    phono_engine *engine = nullptr;
    phono_context_manager *manager = nullptr;
    phono_session *session = nullptr;
    ~Handles() {
        api.phono_session_destroy(session);
        api.phono_context_manager_destroy(manager);
        api.phono_engine_destroy(engine);
    }
};
}
struct Core::Impl {
    std::unique_ptr<Handles> handles;
    Settings settings;
};
Core::Core() : impl_(std::make_unique<Impl>()) {}
Core::~Core() = default;
Result Core::infer(const Request &request) {
    Result result;
    if (request.cancelled->load()) return result;
    if (!impl_->handles || !(request.settings == impl_->settings)) {
        // Build the replacement before releasing the previous working engine.
        auto next = std::make_unique<Handles>(request.settings.library);
        auto& api = next->api;
        check(api, api.phono_engine_create(request.settings.model.c_str(),
                                  request.settings.engineJson.c_str(), &next->engine));
        check(api, api.phono_context_manager_create(next->engine,
                    request.settings.contextJson.c_str(), request.settings.slots, &next->manager));
        check(api, api.phono_session_create(next->engine, request.settings.contextJson.c_str(), &next->session));
        impl_->handles = std::move(next);
        impl_->settings = request.settings;
    }
    auto &h = *impl_->handles;
    auto& api = h.api;
    result.ready = true;
    if (request.pinyin.empty()) return result;
    if (request.cancelled->load()) return result;
    result.beamSize = api.phono_session_beam_size(h.session);
    std::vector<int32_t> pinyin;
    if (request.explicitSyllables) {
        result.invalid = request.invalid;
        std::vector<ExplicitSyllable> accepted;
        for (const auto &syllable : request.syllables) {
            int32_t id = -1;
            for (const auto &alternative : syllable.alternatives) {
                id = api.phono_tokenizer_find_pinyin_id_exact(h.engine, alternative.c_str());
                if (id >= 0) break;
            }
            if (id < 0) {
                result.invalid.push_back({syllable.begin, syllable.end, 0,
                    request.pinyin.substr(syllable.begin, syllable.end - syllable.begin)});
            } else {
                pinyin.push_back(id);
                accepted.push_back(syllable);
            }
        }
        for (auto &range : result.invalid)
            range.beforeSyllable = std::count_if(accepted.begin(), accepted.end(),
                [&](const auto &token) { return token.end <= range.begin; });
    } else {
        char *raw = nullptr;
        result.usedSegmentation = true;
        const auto status = api.phono_engine_segment_pinyin(h.engine,
            nlohmann::json{{"schema_version", "1.0"}, {"input", request.pinyin}}.dump().c_str(), &raw);
        Buffer<char> segmentation(raw, Free{&api});
        if (status == PHONO_PINYIN_LIMIT_EXCEEDED) return result;
        if (status != PHONO_OK && status != PHONO_INVALID_PINYIN) check(api, status);
        if (!segmentation) return result;
        const auto document = nlohmann::json::parse(segmentation.get());
        result.invalid = invalidFromSegmentation(document);
        pinyin = document.at("pinyin_ids").get<std::vector<int32_t>>();
        if (status == PHONO_INVALID_PINYIN) {
            const auto policy = nlohmann::json::parse(request.settings.engineJson);
            if (policy.value("tokenizer", nlohmann::json::object()).value("segment_mode", "safe") == "strict")
                return result;
        }
    }
    if (request.cancelled->load()) return result;
    // An entirely invalid composition is a frontend literal/deletion choice;
    // it must not enter the generation model with an empty syllable sequence.
    if (pinyin.empty()) {
        if (!result.invalid.empty()) result.candidates.emplace_back();
        return result;
    }
    int32_t *rawIds = nullptr;
    int32_t count = 0;
    check(api, api.phono_tokenizer_encode_context(h.engine, request.history.c_str(), &rawIds, &count));
    Buffer<int32_t> ids(rawIds, Free{&api});
    // The tokenizer returns ordinary tokens; the session adds BOS itself.
    const auto contextConfig = nlohmann::json::parse(request.settings.contextJson);
    const int limit = contextConfig.value("max_history_length",
        contextConfig.value("max_context_length", api.phono_engine_pre_max_seqlen(h.engine) - 1) -
        contextConfig.value("max_pinyin_length", api.phono_engine_post_max_seqlen(h.engine)) - 1);
    std::vector<int32_t> history;
    const int keep = std::min(count, std::max(0, limit));
    if (keep) history.assign(ids.get() + count - keep, ids.get() + count);
    int32_t empty = 0;
    const auto *historyData = history.empty() ? &empty : history.data();
    if (request.cancelled->load()) return result;
    // get_auto finds the best matching committed KV prefix across slots.
    auto *context = api.phono_context_manager_get_auto(h.manager, historyData, static_cast<int32_t>(history.size()));
    if (!context) throw std::runtime_error("Cannot obtain a phono context slot");
    // Explicit history is important even for empty documents: no stale context.
    check(api, api.phono_session_replace_context(h.session, context, historyData, static_cast<int32_t>(history.size())));
    Generated generated{api};
    const auto status = api.phono_session_generate(h.session, context, pinyin.data(), static_cast<int32_t>(pinyin.size()),
        nullptr, 0, cancel, request.cancelled.get(), &generated.value);
    if (status == PHONO_CANCELLED || status == PHONO_NO_CANDIDATES ||
        status == PHONO_PINYIN_LIMIT_EXCEEDED) return result;
    check(api, status);
    if (request.cancelled->load()) return result;
    for (int i = 0; i < generated.value.beam_count; ++i) {
        if (generated.value.beams[i].decoded && *generated.value.beams[i].decoded)
            result.candidates.emplace_back(generated.value.beams[i].decoded);
    }
    return result;
}
} // namespace euphony
