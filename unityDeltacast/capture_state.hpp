#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace deltacast {

inline bool valid_index(int index) { return index >= 0 && index < 4; }

inline std::pair<int, int> stereo_extent(int left_width, int left_height,
                                        int right_width, int right_height) {
    if (left_width <= 0 || right_width <= 0 || left_height <= 0 || right_height <= 0 ||
        left_width > std::numeric_limits<int>::max() - right_width)
        throw std::invalid_argument("Invalid stereo dimensions");
    const int width = left_width + right_width;
    const int height = left_height > right_height ? left_height : right_height;
    if (static_cast<uint64_t>(width) * height * 4 > std::numeric_limits<int>::max())
        throw std::invalid_argument("Stereo frame exceeds copy API capacity");
    return {width, height};
}

class CaptureWorker {
public:
    std::atomic<bool> running{false};

    template<class Prepare, class Work>
    bool start(Prepare prepare, Work work) {
        std::lock_guard<std::mutex> lock(lifecycle_);
        if (running.load()) return false;
        if (thread_.joinable()) thread_.join();
        prepare();
        running.store(true);
        try {
            thread_ = std::thread([this, work = std::move(work)]() mutable {
                try { work(); } catch (...) { }
                running.store(false);
            });
        } catch (...) {
            running.store(false);
            throw;
        }
        return true;
    }

    template<class Finish>
    void stop(Finish finish) {
        std::lock_guard<std::mutex> lock(lifecycle_);
        running.store(false);
        if (thread_.joinable()) thread_.join();
        finish();
    }

    void stop() { stop([] {}); }

    ~CaptureWorker() { stop(); }

private:
    std::mutex lifecycle_;
    std::thread thread_;
};

enum class SnapshotResult { Unchanged, Copied, SizeChanged };

class PublishedFrame {
public:
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        pixels_.clear();
        sequence_ = 0;
    }

    void publish(const std::vector<uint8_t>& pixels) {
        std::lock_guard<std::mutex> lock(mutex_);
        pixels_ = pixels;
        ++sequence_;
    }

    uint64_t sequence() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return sequence_;
    }

    int copy(uint8_t* destination, int capacity) const {
        if (!destination || capacity <= 0) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        if (pixels_.empty() || pixels_.size() > static_cast<size_t>(capacity) ||
            pixels_.size() > static_cast<size_t>(std::numeric_limits<int>::max())) return 0;
        std::memcpy(destination, pixels_.data(), pixels_.size());
        return static_cast<int>(pixels_.size());
    }

    SnapshotResult copy_new(uint64_t& seen, size_t expected_bytes,
                            std::vector<uint8_t>& destination) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sequence_ == seen || pixels_.empty()) return SnapshotResult::Unchanged;
        if (pixels_.size() != expected_bytes) return SnapshotResult::SizeChanged;
        destination = pixels_;
        seen = sequence_;
        return SnapshotResult::Copied;
    }

private:
    mutable std::mutex mutex_;
    std::vector<uint8_t> pixels_;
    uint64_t sequence_ = 0;
};

}
