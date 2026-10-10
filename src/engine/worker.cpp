// Adapted from phono-fcitx5-addon; Apache-2.0. See data/licenses/PHONO-ADDON-APACHE-2.0.txt.
#include "worker.h"
#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

namespace euphony {
Worker::Worker(Inference inference)
    : inference_(std::move(inference)), thread_([this] { run(); }) {}
Worker::~Worker() { stop(); }
void Worker::stop() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        if (running_) running_->store(true);
        for (auto &job : pending_) job.request.cancelled->store(true);
        pending_.clear();
    }
    wake_.notify_one();
    if (thread_.joinable()) thread_.join();
}
void Worker::submit(Request request, Completion completion) {
    {
        std::lock_guard lock(mutex_);
        if (stopping_) { request.cancelled->store(true); return; }
        if (running_ && runningContext_ == request.contextId) running_->store(true);
        std::erase_if(pending_, [&](Job &job) {
            if (job.request.contextId != request.contextId) return false;
            job.request.cancelled->store(true);
            return true;
        });
        pending_.push_back({std::move(request), std::move(completion)});
    }
    wake_.notify_one();
}
void Worker::run() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return stopping_ || !pending_.empty(); });
            if (stopping_) return;
            job = std::move(pending_.front());
            pending_.pop_front();
            running_ = job.request.cancelled;
            runningContext_ = job.request.contextId;
        }
        if (!job.request.cancelled->load()) {
            Result result;
            try { result = inference_(job.request); }
            catch (const std::exception &e) { result.error = e.what(); }
            catch (...) { result.error = "Unknown inference error"; }
            if (!job.request.cancelled->load()) job.completion(std::move(result));
        }
        {
            std::lock_guard lock(mutex_);
            running_.reset();
        }
    }
}
} // namespace euphony
