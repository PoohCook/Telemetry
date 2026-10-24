#include "byte_order.hpp"

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

using tlm::load_be;
using tlm::load_le;
using tlm::store_be;
using tlm::store_le;

namespace {

// bytes(0x12, 0x34) -> std::array<std::byte, 2>{0x12, 0x34}
template <typename... Ts>
constexpr std::array<std::byte, sizeof...(Ts)> bytes(Ts... vs)
{
    return {static_cast<std::byte>(vs)...};   // pack expansion (C++11)
}

} // namespace

TEST_CASE("32-bit value has the expected byte layout", "[byte_order]")
{
    std::array<std::byte, 4> buf{};

    SECTION("little-endian puts the least significant byte first")
    {
        store_le<std::uint32_t>(0x12345678, buf);
        CHECK(buf == bytes(0x78, 0x56, 0x34, 0x12));
    }
    SECTION("big-endian puts the most significant byte first")
    {
        store_be<std::uint32_t>(0x12345678, buf);
        CHECK(buf == bytes(0x12, 0x34, 0x56, 0x78));
    }
}

TEST_CASE("Loading decodes known byte sequences", "[byte_order]")
{
    constexpr auto raw = bytes(0x01, 0x02);
    CHECK(load_le<std::uint16_t>(raw) == 0x0201);
    CHECK(load_be<std::uint16_t>(raw) == 0x0102);
}

TEST_CASE("Negative values use two's complement", "[byte_order]")
{
    std::array<std::byte, 2> buf{};
    store_le<std::int16_t>(-2, buf);
    CHECK(buf == bytes(0xFE, 0xFF));
    CHECK(load_le<std::int16_t>(buf) == -2);

    // Same bytes read as unsigned: proves no sign bits were lost or invented.
    CHECK(load_le<std::uint16_t>(buf) == 0xFFFE);
}

// One test case per type. ctest lists each instantiation separately.
TEMPLATE_TEST_CASE("Store then load round-trips", "[byte_order][template]",
                   std::uint8_t, std::int8_t, std::uint16_t, std::int16_t,
                   std::uint32_t, std::int32_t, std::uint64_t, std::int64_t)
{
    using Lim = std::numeric_limits<TestType>;
    const std::array<TestType, 5> values{Lim::min(), Lim::max(), TestType{0}, TestType{1},
                                         static_cast<TestType>(Lim::max() / 3)};

    for (const TestType v : values) {
        std::array<std::byte, sizeof(TestType)> buf{};

        store_le(v, std::span{buf});
        CHECK(load_le<TestType>(buf) == v);

        store_be(v, std::span{buf});
        CHECK(load_be<TestType>(buf) == v);
    }
}

TEMPLATE_TEST_CASE("Little- and big-endian encodings are byte-reversed", "[byte_order][template]",
                   std::uint16_t, std::uint32_t, std::uint64_t)
{
    const auto v = static_cast<TestType>(0x0102030405060708ULL);   // truncated to fit
    std::array<std::byte, sizeof(TestType)> le{};
    std::array<std::byte, sizeof(TestType)> be{};
    store_le(v, std::span{le});
    store_be(v, std::span{be});

    std::ranges::reverse(be);
    CHECK(le == be);
}

TEST_CASE("Fields can be read out of a larger frame", "[byte_order]")
{
    // A Modbus-style response: [addr][func][byte count][reg hi][reg lo]
    constexpr auto frame = bytes(0x11, 0x03, 0x02, 0x12, 0x34);
    const std::span<const std::byte> view{frame};   // dynamic extent

    // subspan<Offset, Count>() returns a fixed-extent span, so it matches
    // load_be's parameter type. view.subspan(3, 2) (runtime size) would not.
    CHECK(load_be<std::uint16_t>(view.subspan<3, 2>()) == 0x1234);
    CHECK(load_be<std::uint8_t>(view.first<1>()) == 0x11);
}

TEST_CASE("Load and store work at compile time", "[byte_order][constexpr]")
{
    STATIC_REQUIRE(load_le<std::uint32_t>(bytes(0x78, 0x56, 0x34, 0x12)) == 0x12345678);
    STATIC_REQUIRE(load_be<std::int16_t>(bytes(0xFF, 0xFE)) == -2);

    STATIC_REQUIRE([] {
        std::array<std::byte, 8> buf{};
        store_be<std::uint64_t>(0xDEADBEEFCAFEF00DULL, buf);
        return load_be<std::uint64_t>(buf) == 0xDEADBEEFCAFEF00DULL;
    }());
}

TEST_CASE("Word concept accepts integers but not bool or floats", "[byte_order][constexpr]")
{
    STATIC_REQUIRE(tlm::Word<std::uint8_t>);
    STATIC_REQUIRE(tlm::Word<std::int64_t>);
    STATIC_REQUIRE(tlm::Word<char>);
    STATIC_REQUIRE_FALSE(tlm::Word<bool>);
    STATIC_REQUIRE_FALSE(tlm::Word<float>);
    STATIC_REQUIRE_FALSE(tlm::Word<std::byte>);   // an enum class, not an integer
}
