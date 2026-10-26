#include "parser.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace tlm;

// Teach Catch2 to print ParseError by name instead of as a number.
template <>
struct Catch::StringMaker<ParseError> {
    static std::string convert(ParseError e) { return std::string{to_string(e)}; }
};

namespace {

using Bytes = std::vector<std::byte>;

// Build a raw frame with a *correct* CRC, so tests can reach the checks that
// come after the CRC (unknown type, wrong payload size).
Bytes make_frame(std::uint8_t type, std::initializer_list<std::uint8_t> payload)
{
    Bytes f{kSync, std::byte{type}, static_cast<std::byte>(payload.size())};
    for (const auto b : payload) f.push_back(std::byte{b});
    f.push_back(std::byte{crc8(std::span{f}.subspan(1))});
    return f;
}

Bytes to_bytes(const Frame& f) { return {f.view().begin(), f.view().end()}; }

} // namespace

// ---------------------------------------------------------------------------
TEST_CASE("Golden frames decode to the expected messages", "[parser]")
{
    // The Task 3 golden vectors, read back.
    const Bytes temp{std::byte{0xA5}, std::byte{0x01}, std::byte{0x03}, std::byte{0x03},
                     std::byte{0x1E}, std::byte{0xFB}, std::byte{0x8B}};
    const ParseResult r = parse(temp);

    REQUIRE(r.has_value());
    CHECK(*r == Message{Temperature{3, -1250}});
}

TEST_CASE("encode then parse round-trips every message", "[parser][roundtrip]")
{
    const Message original = GENERATE(
        Message{Temperature{0, 0}}, Message{Temperature{255, -32768}},
        Message{Temperature{7, 32767}}, Message{Voltage{0, 0}},
        Message{Voltage{2, 65535}}, Message{Fault{0}}, Message{Fault{0xBEEF}});

    const Frame f = encode(original);
    const ParseResult r = parse(f.view());

    REQUIRE(r.has_value());
    CHECK(*r == original);
}

// ---------------------------------------------------------------------------
// Table-driven error cases: one row per failure path.
struct ErrorCase {
    const char* name;
    Bytes       frame;
    ParseError  expected;
};

TEST_CASE("Each malformed frame is rejected with the right error", "[parser][errors]")
{
    const Bytes good = to_bytes(encode(Voltage{1, 3300}));

    auto with = [&](std::size_t i, std::uint8_t v) { Bytes b = good; b[i] = std::byte{v}; return b; };
    auto truncated = [&](std::size_t n) { return Bytes(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(n)); };

    const ErrorCase c = GENERATE_REF(values<ErrorCase>({
        {"empty",                  Bytes{},                         ParseError::TooShort},
        {"three bytes",            truncated(3),                    ParseError::TooShort},
        {"wrong sync byte",        with(0, 0x5A),                   ParseError::BadSync},
        {"length says 2, have 3",  with(2, 0x02),                   ParseError::BadLength},
        {"length over the max",    with(2, 0xFF),                   ParseError::BadLength},
        {"one byte missing",       truncated(good.size() - 1),      ParseError::BadLength},
        {"payload byte corrupted", with(4, 0x00),                   ParseError::BadCrc},
        {"CRC byte corrupted",     with(6, 0x00),                   ParseError::BadCrc},
        {"type corrupted",         with(1, 0x01),                   ParseError::BadCrc},
        {"unknown type, good CRC", make_frame(0x7F, {1, 2}),        ParseError::UnknownType},
        {"fault with 3 bytes",     make_frame(0x03, {1, 2, 3}),     ParseError::BadPayloadSize},
        {"voltage with 0 bytes",   make_frame(0x02, {}),            ParseError::BadPayloadSize},
    }));

    INFO("case: " << c.name);
    const ParseResult r = parse(c.frame);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == c.expected);
}

TEST_CASE("No single-bit error yields a successful parse", "[parser][errors]")
{
    const Bytes good = to_bytes(encode(Temperature{4, 2150}));

    for (std::size_t byte = 0; byte < good.size(); ++byte) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            Bytes bad = good;
            bad[byte] ^= static_cast<std::byte>(1U << bit);
            INFO("byte " << byte << ", bit " << bit);
            CHECK_FALSE(parse(bad).has_value());
        }
    }
}

TEST_CASE("Every truncated prefix is rejected", "[parser][errors]")
{
    const Bytes good = to_bytes(encode(Fault{0x1234}));
    for (std::size_t n = 0; n < good.size(); ++n) {
        INFO("prefix length " << n);
        CHECK_FALSE(parse(std::span{good}.first(n)).has_value());
    }
}

// ---------------------------------------------------------------------------
// C++23 monadic operations: chain steps; an error skips the rest.
TEST_CASE("Chaining with and_then, transform, and or_else", "[parser][monadic]")
{
    // Step that only accepts Voltage messages.
    auto voltage_mv = [](const Message& m) -> std::expected<std::uint16_t, ParseError> {
        if (const auto* v = std::get_if<Voltage>(&m)) return v->millivolts;
        return std::unexpected(ParseError::UnknownType);
    };
    auto to_volts = [](std::uint16_t mv) { return mv / 1000.0; };

    SECTION("success flows through every step")
    {
        const Frame f = encode(Voltage{0, 3300});
        const auto volts = parse(f.view()).and_then(voltage_mv).transform(to_volts);
        REQUIRE(volts.has_value());
        CHECK(*volts == Catch::Approx(3.3));   // never == on floating point
    }
    SECTION("a parse error skips the later steps")
    {
        const Bytes bad{std::byte{0x00}, std::byte{0x02}, std::byte{0x00}, std::byte{0x00}};
        const auto volts = parse(bad).and_then(voltage_mv).transform(to_volts);
        REQUIRE_FALSE(volts.has_value());
        CHECK(volts.error() == ParseError::BadSync);
    }
    SECTION("a step can fail too")
    {
        const Frame f = encode(Fault{1});
        const auto volts = parse(f.view()).and_then(voltage_mv).transform(to_volts);
        CHECK(volts.error() == ParseError::UnknownType);
    }
    SECTION("value_or and or_else supply fallbacks")
    {
        const Bytes bad{};
        CHECK(parse(bad).and_then(voltage_mv).value_or(0) == 0);

        // or_else runs only on error and can recover with a value.
        const auto recovered = parse(bad).or_else([](ParseError) -> ParseResult {
            return Message{Fault{0xFFFF}};
        });
        REQUIRE(recovered.has_value());
        CHECK(*recovered == Message{Fault{0xFFFF}});
    }
}

TEST_CASE("Parsing works at compile time", "[parser][constexpr]")
{
    STATIC_REQUIRE(parse(encode(Fault{0xBEEF}).view()).value() == Message{Fault{0xBEEF}});
    STATIC_REQUIRE(parse(std::span<const std::byte>{}).error() == ParseError::TooShort);
}
