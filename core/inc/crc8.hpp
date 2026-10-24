// CRC-8/SMBUS: polynomial 0x07, init 0x00, no reflection, no final XOR.
// Check value: crc8("123456789") == 0xF4.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace tlm {

inline constexpr std::uint8_t kCrc8Poly = 0x07;

// Shift one byte's worth of bits through the CRC register, MSB first.
// Shared by the bitwise CRC and the table generator so they can't disagree.
[[nodiscard]] constexpr std::uint8_t crc8_step(std::uint8_t crc, std::uint8_t poly) noexcept
{
    for (int bit = 0; bit < 8; ++bit) {
        // `crc << 1` promotes to int; the cast truncates back to 8 bits on purpose.
        const bool msb = (crc & 0x80U) != 0;
        crc = static_cast<std::uint8_t>(crc << 1U);
        if (msb) {
            crc = static_cast<std::uint8_t>(crc ^ poly);
        }
    }
    return crc;
}

// consteval: can only run at compile time. Calling it with a runtime value is an error.
consteval std::array<std::uint8_t, 256> make_crc8_table(std::uint8_t poly)
{
    std::array<std::uint8_t, 256> table{};
    for (std::size_t i = 0; i < table.size(); ++i) {
        table[i] = crc8_step(static_cast<std::uint8_t>(i), poly);
    }
    return table;
}

// inline (C++17): one shared definition across every translation unit.
inline constexpr std::array<std::uint8_t, 256> kCrc8Table = make_crc8_table(kCrc8Poly);

// Reference implementation: no table, about 8 shift/XOR steps per byte.
[[nodiscard]] constexpr std::uint8_t crc8_bitwise(std::span<const std::byte> data) noexcept
{
    std::uint8_t crc = 0x00;
    for (const std::byte b : data) {
        crc = crc8_step(static_cast<std::uint8_t>(crc ^ std::to_integer<std::uint8_t>(b)),
                        kCrc8Poly);
    }
    return crc;
}

// Table-driven: one lookup per byte, at the cost of 256 bytes of read-only data.
[[nodiscard]] constexpr std::uint8_t crc8(std::span<const std::byte> data) noexcept
{
    std::uint8_t crc = 0x00;
    for (const std::byte b : data) {
        crc = kCrc8Table[crc ^ std::to_integer<std::uint8_t>(b)];
    }
    return crc;
}

// Compile-time self-test: the build fails if the check value is wrong.
static_assert([] {
    constexpr char check[] = "123456789";
    std::array<std::byte, sizeof(check) - 1> bytes{};   // -1: drop the '\0'
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::byte>(check[i]);
    }
    return crc8(bytes) == 0xF4 && crc8_bitwise(bytes) == 0xF4;
}());

} // namespace tlm
