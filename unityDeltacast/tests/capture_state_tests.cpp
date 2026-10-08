#include "../capture_state.hpp"

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>

static void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

static void frame_tests() {
    deltacast::PublishedFrame frame;
    std::array<uint8_t, 18> target;
    target.fill(0xCD);
    check(frame.copy(target.data() + 1, 16) == 0, "empty frame");
    frame.publish(std::vector<uint8_t>(16, 0x42));
    check(frame.copy(nullptr, 16) == 0, "null output");
    check(frame.copy(target.data() + 1, -1) == 0, "negative capacity");
    check(frame.copy(target.data() + 1, 0) == 0, "zero capacity");
    check(frame.copy(target.data() + 1, 15) == 0, "short output");
    for (auto byte : target) check(byte == 0xCD, "short output modified");
    check(frame.copy(target.data() + 1, 16) == 16, "exact copy");
    check(target.front() == 0xCD && target.back() == 0xCD, "copy overrun");
    for (size_t i = 1; i < 17; ++i) check(target[i] == 0x42, "copy contents");
    frame.publish(std::vector<uint8_t>(32, 0x69));
    check(frame.copy(target.data() + 1, 16) == 0, "growth truncated");
    check(target[1] == 0x42, "growth modified output");
    frame.publish(std::vector<uint8_t>(8, 0x73));
    check(frame.copy(target.data() + 1, 16) == 8, "shrink copy size");
    uint64_t seen = 0;
    std::vector<uint8_t> output;
    check(frame.copy_new(seen, 16, output) == deltacast::SnapshotResult::SizeChanged,
          "recorder resize detection");
    check(seen == 0 && output.empty(), "resize changed snapshot");
    check(frame.copy_new(seen, 8, output) == deltacast::SnapshotResult::Copied,
          "recorder copy");
    check(seen == 3 && output.size() == 8 && output[0] == 0x73, "recorder snapshot");
    check(frame.copy_new(seen, 8, output) == deltacast::SnapshotResult::Unchanged,
          "duplicate copied");
    frame.reset();
    check(frame.sequence() == 0 && frame.copy(target.data(), 18) == 0, "stale restart frame");
}

static void publication_race_test() {
    deltacast::PublishedFrame frame;
    std::atomic<bool> done{false};
    std::thread producer([&] {
        for (uint64_t i = 1; i <= 10000; ++i) {
            std::vector<uint8_t> bytes(sizeof(i));
            std::memcpy(bytes.data(), &i, sizeof(i));
            frame.publish(bytes);
        }
        done.store(true);
    });
    uint64_t seen = 0;
    size_t samples = 0;
    bool coherent = true;
    std::vector<uint8_t> output;
    do {
        if (frame.copy_new(seen, sizeof(seen), output) == deltacast::SnapshotResult::Copied) {
            uint64_t pixels = 0;
            std::memcpy(&pixels, output.data(), sizeof(pixels));
            coherent &= pixels == seen;
            ++samples;
        }
    } while (!done.load());
    producer.join();
    if (frame.copy_new(seen, sizeof(seen), output) == deltacast::SnapshotResult::Copied) {
        uint64_t pixels = 0;
        std::memcpy(&pixels, output.data(), sizeof(pixels));
        coherent &= pixels == seen;
        ++samples;
    }
    check(coherent && samples > 0, "pixels and sequence mismatch");
}

static void stereo_size_tests() {
    check(deltacast::stereo_extent(1920, 1080, 1920, 1080) == std::make_pair(3840, 1080),
          "full-width stereo size");
    check(deltacast::stereo_extent(1280, 720, 1920, 1080) == std::make_pair(3200, 1080),
          "left reconfiguration size");
    check(deltacast::stereo_extent(1280, 720, 640, 480) == std::make_pair(1920, 720),
          "asymmetric stereo size");
    for (auto size : {0, -1, std::numeric_limits<int>::max()}) {
        bool rejected = false;
        try { deltacast::stereo_extent(size, 1080, 1920, 1080); }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "invalid stereo size accepted");
    }
}

static void lifecycle_tests() {
    deltacast::CaptureWorker worker;
    std::atomic<int> entered{0};
    int prepared = 0;
    auto prepare = [&] { ++prepared; };
    auto work = [&] {
        ++entered;
        while (worker.running.load()) std::this_thread::yield();
    };
    check(worker.start(prepare, work), "first start");
    check(!worker.start(prepare, work), "duplicate start");
    worker.stop();
    worker.stop();
    check(prepared == 1 && entered == 1, "duplicate work or prepare");
    check(worker.start(prepare, [&] { ++entered; throw std::runtime_error("injected failure"); }),
          "failure start");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (worker.running.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    check(!worker.running.load(), "worker failure left running true");
    check(worker.start(prepare, work), "restart after failure");
    worker.stop();
    check(entered == 3, "restart worker missing");
    try { worker.start([] { throw std::runtime_error("prepare failure"); }, work); }
    catch (const std::runtime_error&) { }
    check(!worker.running.load(), "failed prepare left running true");
    std::thread starter([&] { for (int i = 0; i < 100; ++i) worker.start([] {}, work); });
    std::thread stopper([&] { for (int i = 0; i < 100; ++i) worker.stop(); });
    starter.join();
    stopper.join();
    worker.stop();
    check(!worker.running.load(), "concurrent lifecycle did not stop");

    std::atomic<bool> finalizing{false}, release{false}, finalized{false}, attempted{false};
    std::atomic<bool> ordered{false};
    std::thread finishing([&] {
        worker.stop([&] {
            finalizing.store(true);
            while (!release.load()) std::this_thread::yield();
            finalized.store(true);
        });
    });
    while (!finalizing.load()) std::this_thread::yield();
    std::thread restarting([&] {
        attempted.store(true);
        worker.start([&] { ordered.store(finalized.load()); }, work);
    });
    while (!attempted.load()) std::this_thread::yield();
    release.store(true);
    finishing.join();
    restarting.join();
    worker.stop();
    check(ordered.load(), "restart overtook stop finalization");
}

int main() {
    try {
        frame_tests();
        publication_race_test();
        lifecycle_tests();
        stereo_size_tests();
        std::cout << "capture state tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
