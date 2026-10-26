#include "spsc_ring.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

using tlm::SpscRing;

namespace {

// Instrumented element type: counts every construction, copy, move, and
// destruction, so tests can prove the ring doesn't leak or copy needlessly.
struct Counted {
    inline static int constructed = 0;   // inline static (C++17): defined right here
    inline static int destroyed   = 0;
    inline static int copies      = 0;
    inline static int moves       = 0;

    static void reset() { constructed = destroyed = copies = moves = 0; }
    static int  alive() { return constructed - destroyed; }

    int value;

    explicit Counted(int v) : value{v} { ++constructed; }
    Counted(const Counted& o) : value{o.value} { ++constructed; ++copies; }
    Counted(Counted&& o) noexcept : value{o.value} { ++constructed; ++moves; o.value = -1; }
    Counted& operator=(const Counted&) = delete;
    Counted& operator=(Counted&&)      = delete;
    ~Counted() { ++destroyed; }
};

// No default constructor at all: the ring must not need one.
struct NoDefault {
    explicit NoDefault(int v) : value{v} {}
    int value;
};

} // namespace

// ---------------------------------------------------------------------------
TEST_CASE("A new ring is empty", "[ring]")
{
    SpscRing<int, 8> ring;
    CHECK(ring.empty());
    CHECK_FALSE(ring.full());
    CHECK(ring.size() == 0);
    CHECK(ring.capacity() == 8);
    CHECK_FALSE(ring.pop().has_value());
}

TEST_CASE("Push fails when full, and succeeds again after a pop", "[ring]")
{
    SpscRing<int, 4> ring;
    for (int i = 0; i < 4; ++i) {
        REQUIRE(ring.try_push(i));
    }
    CHECK(ring.full());
    CHECK(ring.size() == 4);
    CHECK_FALSE(ring.try_push(99));           // full: rejected, nothing overwritten

    CHECK(ring.pop() == 0);                   // optional<int> compares with int
    CHECK(ring.try_push(4));
    CHECK(ring.size() == 4);
}

TEST_CASE("Elements come out in FIFO order", "[ring]")
{
    SpscRing<std::string, 8> ring;
    REQUIRE(ring.try_push(std::string{"first"}));
    REQUIRE(ring.try_push(std::string{"second"}));
    REQUIRE(ring.try_push(std::string{"third"}));

    CHECK(ring.pop() == "first");
    CHECK(ring.pop() == "second");
    CHECK(ring.pop() == "third");
    CHECK(ring.pop() == std::nullopt);
}

TEST_CASE("Order survives many trips around the buffer", "[ring]")
{
    SpscRing<int, 4> ring;
    int next_in  = 0;
    int next_out = 0;

    // Keep the ring partly full while the indices wrap dozens of times.
    for (int round = 0; round < 100; ++round) {
        while (ring.size() < 3) {
            REQUIRE(ring.try_push(next_in++));
        }
        for (int k = 0; k < 2; ++k) {
            const auto v = ring.pop();
            REQUIRE(v.has_value());
            CHECK(*v == next_out++);
        }
    }
    CHECK(next_in > 100);   // sanity: we really went around many times
}

// ---------------------------------------------------------------------------
TEST_CASE("Move-only types work", "[ring][move]")
{
    SpscRing<std::unique_ptr<int>, 4> ring;
    auto p = std::make_unique<int>(42);

    REQUIRE(ring.try_push(std::move(p)));
    CHECK(p == nullptr);                      // ownership moved into the ring

    std::optional<std::unique_ptr<int>> out = ring.pop();
    REQUIRE(out.has_value());
    REQUIRE(*out != nullptr);
    CHECK(**out == 42);                       // *out is the unique_ptr, **out the int
}

TEST_CASE("Types without a default constructor work", "[ring]")
{
    SpscRing<NoDefault, 2> ring;
    REQUIRE(ring.try_emplace(7));
    CHECK(ring.pop()->value == 7);
}

TEST_CASE("try_emplace constructs in place, with no copies or moves", "[ring][move]")
{
    Counted::reset();
    {
        SpscRing<Counted, 4> ring;
        CHECK(Counted::constructed == 0);     // empty slots construct nothing

        REQUIRE(ring.try_emplace(5));         // forwards 5 to Counted(int)
        CHECK(Counted::constructed == 1);
        CHECK(Counted::copies == 0);
        CHECK(Counted::moves == 0);
    }
    CHECK(Counted::alive() == 0);
}

TEST_CASE("try_push copies an lvalue and moves an rvalue", "[ring][move]")
{
    Counted::reset();
    SpscRing<Counted, 4> ring;
    Counted c{1};

    REQUIRE(ring.try_push(c));                // lvalue: copied
    CHECK(Counted::copies == 1);
    CHECK(c.value == 1);                      // original untouched

    REQUIRE(ring.try_push(std::move(c)));     // rvalue: moved
    CHECK(Counted::moves == 1);
    CHECK(Counted::copies == 1);
}

TEST_CASE("pop moves the element out and destroys the slot's copy", "[ring][move]")
{
    Counted::reset();
    SpscRing<Counted, 4> ring;
    REQUIRE(ring.try_emplace(9));

    {
        const auto out = ring.pop();
        REQUIRE(out.has_value());
        CHECK(out->value == 9);
        CHECK(Counted::copies == 0);
        CHECK(Counted::alive() == 1);         // only the one inside `out`
    }
    CHECK(Counted::alive() == 0);
}

TEST_CASE("A full ring rejects a push without constructing anything", "[ring][move]")
{
    Counted::reset();
    SpscRing<Counted, 2> ring;
    REQUIRE(ring.try_emplace(1));
    REQUIRE(ring.try_emplace(2));
    const int before = Counted::constructed;

    CHECK_FALSE(ring.try_emplace(3));
    CHECK(Counted::constructed == before);
}

TEST_CASE("Destroying a non-empty ring destroys its elements", "[ring][raii]")
{
    Counted::reset();
    {
        SpscRing<Counted, 8> ring;
        for (int i = 0; i < 5; ++i) REQUIRE(ring.try_emplace(i));
        REQUIRE(ring.pop().has_value());      // leave 4 behind
        CHECK(Counted::alive() == 4);
    }                                         // ~SpscRing runs here
    CHECK(Counted::alive() == 0);

    // Same check with heap-owning elements: ASan would report a leak.
    SpscRing<std::unique_ptr<int>, 4> owners;
    REQUIRE(owners.try_push(std::make_unique<int>(1)));
    REQUIRE(owners.try_push(std::make_unique<int>(2)));
}

// ---------------------------------------------------------------------------
// A requires-expression can ask "would this type be valid?" without
// breaking the build when the answer is no.
template <typename T, std::size_t N>
concept RingAccepts = requires { typename SpscRing<T, N>; };

struct ThrowingMove {
    ThrowingMove(ThrowingMove&&) {}           // not noexcept
};

TEST_CASE("Compile-time constraints on the ring's parameters", "[ring][constexpr]")
{
    STATIC_REQUIRE(tlm::PowerOfTwo<2>);
    STATIC_REQUIRE(tlm::PowerOfTwo<1024>);
    STATIC_REQUIRE_FALSE(tlm::PowerOfTwo<0>);
    STATIC_REQUIRE_FALSE(tlm::PowerOfTwo<1>);
    STATIC_REQUIRE_FALSE(tlm::PowerOfTwo<6>);

    STATIC_REQUIRE(RingAccepts<int, 8>);
    STATIC_REQUIRE_FALSE(RingAccepts<int, 6>);            // not a power of two
    STATIC_REQUIRE_FALSE(RingAccepts<ThrowingMove, 8>);   // move might throw

    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<SpscRing<int, 8>>);
    STATIC_REQUIRE_FALSE(std::is_move_constructible_v<SpscRing<int, 8>>);
}
