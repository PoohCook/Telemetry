#include "crc8.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

using tlm::crc8;
using tlm::crc8_bitwise;

namespace {

// Runtime helper: view a string's characters as bytes (not constexpr).
std::span<const std::byte> bytes_of(std::string_view s)
{
    return std::as_bytes(std::span{s});
}

// Constexpr helper for compile-time checks.
template <std::size_t N>
constexpr std::array<std::byte, N - 1> literal_bytes(const char (&s)[N])
{
    std::array<std::byte, N - 1> out{};
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = static_cast<std::byte>(s[i]);
    return out;
}

} // namespace

TEST_CASE("CRC-8 check value", "[crc8]")
{
    const auto data = bytes_of("123456789");
    CHECK(crc8(data) == 0xF4);
    CHECK(crc8_bitwise(data) == 0xF4);
}

TEST_CASE("CRC-8 of empty input is the initial value", "[crc8]")
{
    const std::span<const std::byte> empty{};
    CHECK(crc8(empty) == 0x00);
    CHECK(crc8_bitwise(empty) == 0x00);
}

TEST_CASE("CRC-8 table spot checks", "[crc8]")
{
    CHECK(tlm::kCrc8Table[0] == 0x00);
    // 0x01 has one bit set; after 7 shifts it reaches the MSB, and the 8th
    // shift pushes it out and XORs in the polynomial. So the entry is the poly.
    CHECK(tlm::kCrc8Table[1] == tlm::kCrc8Poly);
}

TEST_CASE("Table-driven CRC matches bitwise CRC for every single byte", "[crc8]")
{
    const int value = GENERATE(range(0, 256));
    const std::array<std::byte, 1> data{static_cast<std::byte>(value)};
    INFO("byte value = " << value);     // printed only if the assertion fails
    REQUIRE(crc8(data) == crc8_bitwise(data));
}

TEST_CASE("Appending the CRC to a message yields a zero CRC", "[crc8]")
{
    const std::string_view msg = GENERATE(as<std::string_view>{},
                                          "123456789", "a", "telemetry frame", "\x00\xff\x5a");
    std::vector<std::byte> framed(bytes_of(msg).begin(), bytes_of(msg).end());
    framed.push_back(static_cast<std::byte>(crc8(framed)));

    INFO("message = \"" << msg << '"');
    REQUIRE(crc8(framed) == 0x00);
}

TEST_CASE("Every single-bit error changes the CRC", "[crc8]")
{
    const auto original = bytes_of("CAN frame");
    const std::uint8_t good = crc8(original);

    std::vector<std::byte> data(original.begin(), original.end());
    for (std::size_t byte = 0; byte < data.size(); ++byte) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            const auto mask = static_cast<std::byte>(1U << bit);
            data[byte] ^= mask;
            INFO("byte " << byte << ", bit " << bit);
            CHECK(crc8(data) != good);
            data[byte] ^= mask;          // restore
        }
    }
    REQUIRE(crc8(data) == good);         // sanity: fully restored
}

TEST_CASE("CRC-8 is usable at compile time", "[crc8][constexpr]")
{
    // STATIC_REQUIRE is a static_assert that also counts as a test assertion.
    STATIC_REQUIRE(crc8(literal_bytes("123456789")) == 0xF4);
    STATIC_REQUIRE(crc8_bitwise(literal_bytes("123456789")) == 0xF4);
    STATIC_REQUIRE(tlm::kCrc8Table.size() == 256);
}
