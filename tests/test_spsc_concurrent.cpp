// The ring under real concurrency: one producer thread, one consumer thread.
// Run these under ThreadSanitizer (the `tsan` preset) as well as normally.
//
// Rule: Catch2 assertions are not thread-safe. Threads only record what they
// saw; the CHECKs run on the main thread after the threads have joined.

#include "messages.hpp"
#include "parser.hpp"
#include "spsc_ring.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stop_token>   // std::stop_token (C++20)
#include <thread>       // std::jthread (C++20)
#include <utility>

using tlm::SpscRing;

namespace {

// Spin until the ring accepts the value. A failed try_push doesn't touch its
// argument, so retrying with the same forwarded value is safe.
template <typename Ring, typename U>
void push_blocking(Ring& ring, U&& value)
{
    while (!ring.try_push(std::forward<U>(value))) {
        std::this_thread::yield();
    }
}

// Deterministic message for sequence number i, so the consumer can predict
// exactly what it should receive.
tlm::Message message_for(std::uint32_t i)
{
    switch (i % 3) {
        case 0:  return tlm::Temperature{static_cast<std::uint8_t>(i % 4),
                                         static_cast<std::int16_t>(i % 5000)};
        case 1:  return tlm::Voltage{static_cast<std::uint8_t>(i % 3),
                                     static_cast<std::uint16_t>(i % 65536)};
        default: return tlm::Fault{static_cast<std::uint16_t>(i % 65536)};
    }
}

} // namespace

// ---------------------------------------------------------------------------
TEST_CASE("A million integers arrive complete and in order", "[ring][concurrent]")
{
    constexpr std::uint64_t kCount = 1'000'000;
    SpscRing<std::uint64_t, 1024> ring;

    // Written only by the consumer thread, read only after it has joined.
    std::uint64_t received     = 0;
    std::uint64_t out_of_order = 0;
    std::uint64_t sum          = 0;

    {
        std::jthread producer{[&] {
            for (std::uint64_t i = 0; i < kCount; ++i) {
                push_blocking(ring, i);
            }
        }};

        std::jthread consumer{[&] {
            std::uint64_t expected = 0;
            while (expected < kCount) {
                if (const auto v = ring.pop()) {
                    if (*v != expected) ++out_of_order;
                    sum += *v;
                    ++expected;
                } else {
                    std::this_thread::yield();
                }
            }
            received = expected;
        }};
    }   // jthread destructors join both threads here; join() also makes the
        // consumer's writes to received/out_of_order/sum visible to us.

    CHECK(received == kCount);
    CHECK(out_of_order == 0);
    CHECK(sum == kCount * (kCount - 1) / 2);   // nothing duplicated or dropped
    CHECK(ring.empty());
}

// ---------------------------------------------------------------------------
TEST_CASE("Encoded frames cross threads intact", "[ring][concurrent]")
{
    constexpr std::uint32_t kCount = 100'000;
    SpscRing<tlm::Frame, 8> ring;   // tiny ring: producer and consumer constantly collide

    std::uint32_t parse_errors = 0;
    std::uint32_t mismatches   = 0;

    {
        std::jthread producer{[&] {
            for (std::uint32_t i = 0; i < kCount; ++i) {
                push_blocking(ring, tlm::encode(message_for(i)));
            }
        }};

        std::jthread consumer{[&] {
            for (std::uint32_t i = 0; i < kCount;) {
                const auto frame = ring.pop();
                if (!frame) {
                    std::this_thread::yield();
                    continue;
                }
                const auto msg = tlm::parse(frame->view());
                if (!msg) {
                    ++parse_errors;
                } else if (*msg != message_for(i)) {
                    ++mismatches;
                }
                ++i;
            }
        }};
    }

    CHECK(parse_errors == 0);
    CHECK(mismatches == 0);
}

// ---------------------------------------------------------------------------
namespace {

// Counts live objects across threads: constructed on the producer thread,
// destroyed on the consumer thread, so the counter itself must be atomic.
struct Tracked {
    inline static std::atomic<int> alive{0};
    int value;
    explicit Tracked(int v) : value{v} { alive.fetch_add(1, std::memory_order_relaxed); }
    ~Tracked() { alive.fetch_sub(1, std::memory_order_relaxed); }
    Tracked(const Tracked&)            = delete;
    Tracked& operator=(const Tracked&) = delete;
};

} // namespace

TEST_CASE("Ownership of heap objects transfers between threads", "[ring][concurrent]")
{
    constexpr int kCount = 50'000;
    SpscRing<std::unique_ptr<Tracked>, 4> ring;
    long long sum = 0;

    {
        std::jthread producer{[&] {
            for (int i = 0; i < kCount; ++i) {
                push_blocking(ring, std::make_unique<Tracked>(i));
            }
        }};
        std::jthread consumer{[&] {
            for (int n = 0; n < kCount;) {
                if (auto p = ring.pop()) {   // p owns the object now...
                    sum += (*p)->value;
                    ++n;
                }                            // ...and frees it here
                else {
                    std::this_thread::yield();
                }
            }
        }};
    }

    CHECK(sum == static_cast<long long>(kCount) * (kCount - 1) / 2);
    CHECK(Tracked::alive.load() == 0);       // every object freed exactly once
}

// ---------------------------------------------------------------------------
TEST_CASE("A consumer stops cleanly via std::stop_token", "[ring][concurrent]")
{
    constexpr int kCount = 10'000;
    SpscRing<int, 64> ring;
    int received = 0;

    {
        // A jthread whose function takes a std::stop_token gets one automatically.
        // The destructor calls request_stop() and then join().
        std::jthread consumer{[&](std::stop_token stop) {
            // Keep going until asked to stop AND everything queued is drained.
            while (!stop.stop_requested() || !ring.empty()) {
                if (ring.pop()) ++received;
                else            std::this_thread::yield();
            }
        }};

        std::jthread producer{[&] {
            for (int i = 0; i < kCount; ++i) push_blocking(ring, i);
        }};

        // Locals are destroyed in reverse order of declaration:
        //   1. producer joins (all items pushed),
        //   2. consumer gets request_stop(), drains the rest, and joins.
    }

    CHECK(received == kCount);
}
