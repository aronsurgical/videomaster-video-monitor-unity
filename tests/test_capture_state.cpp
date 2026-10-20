/* White-box unit tests of the frame publication and worker lifecycle in
 * capture/capture_state.hpp (header-only, no device). */

#include "capture/capture_state.hpp"
#include "doctest.h"

#include <array>
#include <chrono>
#include <limits>
#include <stdexcept>

TEST_CASE("PublishedFrame copies whole frames only")
{
    deltacast::PublishedFrame frame;
    std::array<uint8_t, 18> target;
    target.fill(0xCD);
    CHECK(frame.copy(target.data() + 1, 16) == 0); // empty frame
    frame.publish(std::vector<uint8_t>(16, 0x42));
    CHECK(frame.copy(nullptr, 16) == 0);
    CHECK(frame.copy(target.data() + 1, -1) == 0);
    CHECK(frame.copy(target.data() + 1, 0) == 0);
    CHECK(frame.copy(target.data() + 1, 15) == 0);
    for (auto byte : target) CHECK(byte == 0xCD); // short output not modified
    CHECK(frame.copy(target.data() + 1, 16) == 16);
    CHECK(target.front() == 0xCD);
    CHECK(target.back() == 0xCD);
    for (size_t i = 1; i < 17; ++i) CHECK(target[i] == 0x42);

    frame.publish(std::vector<uint8_t>(32, 0x69));
    CHECK(frame.copy(target.data() + 1, 16) == 0); // growth is not truncated
    CHECK(target[1] == 0x42);
    frame.publish(std::vector<uint8_t>(8, 0x73));
    CHECK(frame.copy(target.data() + 1, 16) == 8);

    SUBCASE("recorder snapshot")
    {
        uint64_t seen = 0;
        std::vector<uint8_t> output;
        CHECK(frame.copy_new(seen, 16, output) == deltacast::SnapshotResult::SizeChanged);
        CHECK(seen == 0);
        CHECK(output.empty());
        CHECK(frame.copy_new(seen, 8, output) == deltacast::SnapshotResult::Copied);
        CHECK(seen == 3);
        REQUIRE(output.size() == 8);
        CHECK(output[0] == 0x73);
        CHECK(frame.copy_new(seen, 8, output) == deltacast::SnapshotResult::Unchanged);
    }
    SUBCASE("reset clears a stale frame")
    {
        frame.reset();
        CHECK(frame.sequence() == 0);
        CHECK(frame.copy(target.data(), 18) == 0);
    }
}

TEST_CASE("PublishedFrame snapshots keep pixels and sequence together")
{
    deltacast::PublishedFrame frame;
    std::atomic<bool> done{false};
    std::thread producer([&] {
        for (uint64_t i = 1; i <= 10000; ++i)
        {
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
    auto sample = [&] {
        if (frame.copy_new(seen, sizeof(seen), output) == deltacast::SnapshotResult::Copied)
        {
            uint64_t pixels = 0;
            std::memcpy(&pixels, output.data(), sizeof(pixels));
            coherent &= pixels == seen;
            ++samples;
        }
    };
    do sample(); while (!done.load());
    producer.join();
    sample();
    CHECK(coherent);
    CHECK(samples > 0);
}

TEST_CASE("stereo_extent")
{
    CHECK(deltacast::stereo_extent(1920, 1080, 1920, 1080) == std::make_pair(3840, 1080));
    CHECK(deltacast::stereo_extent(1280, 720, 1920, 1080) == std::make_pair(3200, 1080));
    CHECK(deltacast::stereo_extent(1280, 720, 640, 480) == std::make_pair(1920, 720));
    for (auto size : {0, -1, std::numeric_limits<int>::max()})
    {
        CAPTURE(size);
        CHECK_THROWS_AS(deltacast::stereo_extent(size, 1080, 1920, 1080), std::invalid_argument);
    }
}

TEST_CASE("CaptureWorker lifecycle")
{
    deltacast::CaptureWorker worker;
    std::atomic<int> entered{0};
    int prepared = 0;
    auto prepare = [&] { ++prepared; };
    auto work = [&] {
        ++entered;
        while (worker.running.load()) std::this_thread::yield();
    };
    CHECK(worker.start(prepare, work));
    CHECK_FALSE(worker.start(prepare, work)); // duplicate start
    worker.stop();
    worker.stop();
    CHECK(prepared == 1);
    CHECK(entered == 1);

    CHECK(worker.start(prepare, [&] { ++entered; throw std::runtime_error("injected failure"); }));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (worker.running.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    CHECK_FALSE(worker.running.load()); // a failed worker clears running
    CHECK(worker.start(prepare, work)); // and can be restarted
    worker.stop();
    CHECK(entered == 3);

    CHECK_THROWS_AS(worker.start([] { throw std::runtime_error("prepare failure"); }, work),
                    std::runtime_error);
    CHECK_FALSE(worker.running.load());

    std::thread starter([&] { for (int i = 0; i < 100; ++i) worker.start([] {}, work); });
    std::thread stopper([&] { for (int i = 0; i < 100; ++i) worker.stop(); });
    starter.join();
    stopper.join();
    worker.stop();
    CHECK_FALSE(worker.running.load());

    // A restart must not overtake the finalization of a concurrent stop.
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
    CHECK(ordered.load());
}

TEST_CASE("PublishedFrame hands out the capture time of the frame it copies")
{
    deltacast::PublishedFrame published;
    std::vector<uint8_t> destination;
    uint64_t seen = 0;
    int64_t hostUs = -1;
    published.publish(std::vector<uint8_t>(8, 1), 1234);
    CHECK(published.copy_new(seen, 8, destination, &hostUs) == deltacast::SnapshotResult::Copied);
    CHECK(hostUs == 1234);
    published.publish(std::vector<uint8_t>(8, 2), 5678);
    CHECK(published.copy_new(seen, 8, destination) == deltacast::SnapshotResult::Copied);
    CHECK(published.copy_new(seen, 8, destination, &hostUs) == deltacast::SnapshotResult::Unchanged);
    CHECK(hostUs == 1234);
}
