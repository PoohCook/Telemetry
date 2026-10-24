// Endian-safe integer load/store between byte buffers and integers.
//
// The C way, *(uint16_t*)p, is undefined behavior in C++ (strict aliasing, and
// possibly misaligned). std::bit_cast is the sanctioned replacement: it copies
// the object representation, is constexpr, and optimizes to a single load.
#pragma once

#include <algorithm>    // std::ranges::copy
#include <array>
#include <bit>          // std::bit_cast, std::endian (C++20), std::byteswap (C++23)
#include <concepts>
#include <cstddef>
#include <span>
#include <type_traits>

namespace tlm {

// Mixed-endian (PDP-11 style) platforms exist in theory; refuse to build on them.
static_assert(std::endian::native == std::endian::little ||
              std::endian::native == std::endian::big,
              "mixed-endian platforms are not supported");

// A concept: integral types except bool. std::integral alone would accept bool,
// which has no meaningful byte order.
template <typename T>
concept Word = std::integral<T> && !std::same_as<std::remove_cv_t<T>, bool>;

// Read a T stored in byte order `Order`. The fixed-extent span makes the
// length part of the parameter's type: passing the wrong size won't compile.
template <std::endian Order, Word T>
[[nodiscard]] constexpr T load(std::span<const std::byte, sizeof(T)> in) noexcept
{
    std::array<std::byte, sizeof(T)> raw{};
    std::ranges::copy(in, raw.begin());
    T value = std::bit_cast<T>(raw);
    if constexpr (Order != std::endian::native) {   // branch removed at compile time
        value = std::byteswap(value);
    }
    return value;
}

// Write `value` into `out` in byte order `Order`.
template <std::endian Order, Word T>
constexpr void store(T value, std::span<std::byte, sizeof(T)> out) noexcept
{
    if constexpr (Order != std::endian::native) {
        value = std::byteswap(value);
    }
    const auto raw = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
    std::ranges::copy(raw, out.begin());
}

// Convenience names. Little-endian: CAN payloads, most MCUs.
// Big-endian ("network order"): Modbus registers, TCP/IP headers.
template <Word T>
[[nodiscard]] constexpr T load_le(std::span<const std::byte, sizeof(T)> in) noexcept
{
    return load<std::endian::little, T>(in);
}
template <Word T>
[[nodiscard]] constexpr T load_be(std::span<const std::byte, sizeof(T)> in) noexcept
{
    return load<std::endian::big, T>(in);
}
template <Word T>
constexpr void store_le(T value, std::span<std::byte, sizeof(T)> out) noexcept
{
    store<std::endian::little>(value, out);
}
template <Word T>
constexpr void store_be(T value, std::span<std::byte, sizeof(T)> out) noexcept
{
    store<std::endian::big>(value, out);
}

} // namespace tlm
