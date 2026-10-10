#pragma once
#include "win_util.h"
#include <nlohmann/json.hpp>
namespace euphony {
nlohmann::json default_config();
void validate_config(const nlohmann::json& config);
class ConfigStore {
public:
    ConfigStore();
    const nlohmann::json& get() const { return value_; }
    const std::string& load_error() const { return load_error_; }
    nlohmann::json prepare(const nlohmann::json& input) const;
    void save(const nlohmann::json& value);
private:
    nlohmann::json value_;
    std::string load_error_;
};
}
