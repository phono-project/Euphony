#include "common/config.h"
#include "common/ipc.h"
#include <iostream>
#include <fstream>

int main() {
    try {
        const std::string text = "拼音😀ABC";
        if (euphony::utf8(euphony::wide(text)) != text) throw std::runtime_error("Unicode roundtrip failed");
        bool invalid = false;
        try { euphony::wide(std::string("\xff")); } catch (...) { invalid = true; }
        if (!invalid) throw std::runtime_error("Invalid UTF8 accepted");
        auto config = euphony::default_config();
        euphony::validate_config(config);
        if (config["user_candidate_color"] != "#FF0000") throw std::runtime_error("Memory color default is not red");
        if (config["page_size"] != 5 || config["candidate_layout"] != "vertical") throw std::runtime_error("Candidate display defaults are incorrect");
        if (config["prefer_unigram_single_syllable"] != true) throw std::runtime_error("Single-syllable unigram default is not enabled");
        for (const auto& patch : {nlohmann::json{{"page_size", 0}}, nlohmann::json{{"context_slots", 33}},
             nlohmann::json{{"input_mode", "unknown"}}, nlohmann::json{{"candidate_layout", "diagonal"}}, nlohmann::json{{"model_candidate_color", "#GG0000"}}, nlohmann::json{{"user_candidate_color", "red"}}, nlohmann::json{{"model_directory", "relative/path"}},
             nlohmann::json{{"language_model_path", "relative.lm"}}, nlohmann::json{{"prefer_unigram_single_syllable", "true"}}}) {
            auto bad = config;
            bad.merge_patch(patch);
            bool rejected = false;
            try { euphony::validate_config(bad); } catch (...) { rejected = true; }
            if (!rejected) throw std::runtime_error("Invalid configuration accepted");
        }
        const auto test_profile = euphony::module_directory() / L"common-test-profile";
        std::filesystem::create_directories(test_profile);
        SetEnvironmentVariableW(L"EUPHONY_USER_DIR", test_profile.c_str());
        { std::ofstream broken(test_profile / L"config.json"); broken << "{broken"; }
        euphony::ConfigStore recovered;
        if (recovered.load_error().empty()) throw std::runtime_error("Damaged config did not report recovery");
        { std::ifstream original(test_profile / L"config.json"); std::string content; original >> content;
          if (content != "{broken") throw std::runtime_error("Recovery overwrote the damaged config"); }
        auto edited = recovered.prepare({{"page_size", 4}, {"candidate_layout", "horizontal"}, {"prefer_unigram_single_syllable", false}, {"context", {{"max_context_length", 128}}}});
        recovered.save(edited);
        edited = recovered.prepare({{"context", {{"max_context_length", nullptr}}}});
        if (edited["context"].contains("max_context_length")) throw std::runtime_error("Automatic window did not clear explicit override");
        recovered.save(edited);
        euphony::ConfigStore restored;
        if (!restored.load_error().empty() || restored.get()["page_size"] != 4) throw std::runtime_error("Config save did not persist");
        if (restored.get()["prefer_unigram_single_syllable"] != false) throw std::runtime_error("Unigram preference did not persist");
        if (restored.get()["candidate_layout"] != "horizontal") throw std::runtime_error("Candidate layout did not persist");
        std::cout << "Unicode and configuration checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
