// Adapted from phono-fcitx5-addon; Apache-2.0. See data/licenses/PHONO-ADDON-APACHE-2.0.txt.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace euphony {
using Cancellation = std::shared_ptr<std::atomic_bool>;
enum class InvalidInputPolicy { PassThrough, Delete };
struct InvalidInput {
    size_t begin = 0;
    size_t end = 0;
    size_t beforeSyllable = 0;
    std::string text;
};
struct ExplicitSyllable {
    size_t begin = 0;
    size_t end = 0;
    std::vector<std::string> alternatives;
};
struct Settings {
    std::string model;
    std::string library;
    std::string engineJson;
    std::string contextJson;
    int slots = 4;
    bool operator==(const Settings &) const = default;
};
struct Request {
    uint64_t contextId = 0;
    std::string pinyin;
    std::string history;
    Settings settings;
    Cancellation cancelled = std::make_shared<std::atomic_bool>(false);
    // A rule-based frontend supplies syllables directly. The core only performs
    // exact vocabulary lookup and generation, without invoking segmentation.
    bool explicitSyllables = false;
    std::vector<ExplicitSyllable> syllables;
    std::vector<InvalidInput> invalid;
    InvalidInputPolicy invalidPolicy = InvalidInputPolicy::PassThrough;
};
struct Result {
    std::vector<std::string> candidates;
    std::string error;
    int beamSize = 0;
    bool ready = false;
    std::vector<InvalidInput> invalid;
    bool usedSegmentation = false;
};

// All core handles live on this single worker. No UI object crosses threads.
class Core {
public:
    Core();
    ~Core();
    Result infer(const Request &);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class Worker {
public:
    using Inference = std::function<Result(const Request &)>;
    using Completion = std::function<void(Result)>;
    explicit Worker(Inference inference);
    ~Worker();
    void submit(Request request, Completion completion);
    void stop();
private:
    struct Job { Request request; Completion completion; };
    void run();
    Inference inference_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> pending_;
    Cancellation running_;
    uint64_t runningContext_ = 0;
    bool stopping_ = false;
    std::thread thread_;
};
} // namespace euphony
