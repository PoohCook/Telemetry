#include "messages.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <variant>

using namespace tlm;

namespace {

template <typename... Ts>
constexpr std::array<std::byte, sizeof...(Ts)> bytes(Ts... vs)
{
    return {static_cast<std::byte>(vs)...};
}

// Compare a frame's used bytes against an expected sequence.
template <std::size_t N>
bool frame_equals(const Frame& f, const std::array<std::byte, N>& expected)
{
    return std::ranges::equal(f.view(), expected);
}

} // namespace

// ---------------------------------------------------------------------------
// Golden vectors: exact bytes on the wire. CRCs cross-checked with an
// independent Python implementation. If these change, the protocol changed.
TEST_CASE("Encoding produces the exact wire bytes", "[messages][encode]")
{
    SECTION("Temperature: sensor 3, -12.50 C")
    {
        const Frame f = encode(Temperature{3, -1250});      // -1250 = 0xFB1E
        CHECK(f.size == 7);
        CHECK(frame_equals(f, bytes(0xA5, 0x01, 0x03, 0x03, 0x1E, 0xFB, 0x8B)));
    }
    SECTION("Voltage: rail 1, 3300 mV")
    {
        const Frame f = encode(Voltage{1, 3300});           // 3300 = 0x0CE4
        CHECK(f.size == 7);
        CHECK(frame_equals(f, bytes(0xA5, 0x02, 0x03, 0x01, 0xE4, 0x0C, 0xA6)));
    }
    SECTION("Fault: code 0xBEEF")
    {
        const Frame f = encode(Fault{0xBEEF});
        CHECK(f.size == 6);
        CHECK(frame_equals(f, bytes(0xA5, 0x03, 0x02, 0xEF, 0xBE, 0x5F)));
    }
}

TEST_CASE("Every frame's CRC checks out", "[messages][encode]")
{
    for (const Message& m : {Message{Temperature{0, 0}}, Message{Voltage{2, 12000}},
                             Message{Fault{0}}, Message{Temperature{255, -32768}}}) {
        const Frame f = encode(m);
        REQUIRE(f.view().front() == kSync);
        // Residue property from Task 1: CRC over everything after sync is zero.
        CHECK(crc8(f.view().subspan(1)) == 0x00);
        // The length byte agrees with the actual frame size.
        CHECK(std::to_integer<std::size_t>(f.bytes[2]) == f.size - kHeaderSize - kCrcSize);
    }
}

TEST_CASE("Unused frame bytes stay zero", "[messages][encode]")
{
    const Frame f = encode(Fault{0xFFFF});
    const auto tail = std::span{f.bytes}.subspan(f.size);
    CHECK(std::ranges::all_of(tail, [](std::byte b) { return b == std::byte{0}; }));
}

// ---------------------------------------------------------------------------
TEST_CASE("Defaulted <=> compares members in declaration order", "[messages][compare]")
{
    CHECK(Temperature{1, 100} == Temperature{1, 100});
    CHECK(Temperature{1, 100} != Temperature{1, 101});

    // First member decides unless equal; then the next member decides.
    CHECK(Temperature{1, 999} < Temperature{2, -999});
    CHECK(Temperature{1, -5} < Temperature{1, 5});

    // <=> itself returns an ordering object you can inspect.
    CHECK((Voltage{0, 5000} <=> Voltage{0, 3300}) == std::strong_ordering::greater);
}

TEST_CASE("Variant compares by alternative index, then by value", "[messages][compare]")
{
    const Message t = Temperature{9, 9};
    const Message v = Voltage{0, 0};

    CHECK(t == Message{Temperature{9, 9}});
    CHECK(t != v);
    CHECK(t < v);    // Temperature is alternative 0, Voltage is 1: index wins
}

// ---------------------------------------------------------------------------
TEST_CASE("Inspecting which alternative a Message holds", "[messages][variant]")
{
    Message m = Voltage{2, 5000};

    CHECK(m.index() == 1);
    CHECK(std::holds_alternative<Voltage>(m));
    CHECK(type_of(m) == MsgType::Voltage);
    CHECK(name_of(type_of(m)) == "Voltage");

    // get_if: pointer to the value, or nullptr. Never throws.
    REQUIRE(std::get_if<Voltage>(&m) != nullptr);
    CHECK(std::get_if<Voltage>(&m)->millivolts == 5000);
    CHECK(std::get_if<Fault>(&m) == nullptr);

    // Assigning a different alternative switches the variant.
    m = Fault{7};
    CHECK(type_of(m) == MsgType::Fault);
}

TEST_CASE("std::visit with a generic lambda", "[messages][variant]")
{
    // One lambda with an `auto` parameter handles every alternative.
    // if constexpr picks the per-type behavior at compile time.
    auto describe = [](const auto& msg) -> std::string_view {
        using T = std::decay_t<decltype(msg)>;
        if constexpr (std::is_same_v<T, Fault>) return "alarm";
        else                                    return "reading";
    };

    CHECK(std::visit(describe, Message{Fault{1}}) == "alarm");
    CHECK(std::visit(describe, Message{Temperature{1, 1}}) == "reading");
}

// ---------------------------------------------------------------------------
TEST_CASE("Encoding works at compile time", "[messages][constexpr]")
{
    STATIC_REQUIRE(encode(Fault{0xBEEF}).size == 6);
    STATIC_REQUIRE(encode(Voltage{1, 3300}).bytes[6] == std::byte{0xA6});
    STATIC_REQUIRE(type_of(Message{Temperature{}}) == MsgType::Temperature);
}

TEST_CASE("Type properties the rest of the pipeline relies on", "[messages][constexpr]")
{
    STATIC_REQUIRE(sizeof(MsgType) == 1);
    STATIC_REQUIRE(std::is_trivially_copyable_v<Message>);
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<Frame>);
    STATIC_REQUIRE(std::variant_size_v<Message> == 3);
}
