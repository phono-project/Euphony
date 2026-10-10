#pragma once
#include <memory>
#include <functional>
#include <nlohmann/json.hpp>

namespace euphony {
struct Request;
struct Result;
// Called serially by the broker. Inference runs on the owned worker, and results
// are applied only while handling a request on the broker thread.
class Engine {
public:
    explicit Engine(const nlohmann::json& config);
    // Inject an inference implementation for deterministic worker/IPC tests.
    Engine(const nlohmann::json& config, std::function<Result(const Request&)> inference);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    nlohmann::json handle(const nlohmann::json& request);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
