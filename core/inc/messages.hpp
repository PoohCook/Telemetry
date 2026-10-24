// Telemetry message types and the frame encoder.
//
// Wire format (multi-byte fields little-endian):
//   [0xA5 sync][type u8][len u8][payload: len bytes][CRC-8 over type..payload]
#pragma once

#include "byte_order.hpp"
#include "crc8.hpp"

#include <array>
#include <compare>      // operator<=> (C++20)
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>      // std::to_underlying (C++23)
#include <variant>      // std::variant (C++17)

namespace tlm {

// ---------------------------------------------------------------------------
// overloaded: build one callable out of several lambdas, for std::visit.
// Inherits each lambda's operator() and pulls them all into one overload set.
template <typename... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;            // C++17: pack expansion in a using-declaration
};
// (C++17 needed a deduction guide here; C++20 aggregate CTAD makes it unnecessary.)

// ---------------------------------------------------------------------------
// Scoped enum (C++11): no implicit conversion to int, names don't leak.
// The underlying type pins its size to one byte, matching the wire format.
enum class MsgType : std::uint8_t {
    Temperature = 0x01,
    Voltage = 0x02,
    Fault = 0x03
};

// Plain aggregates. Defaulted <=> (C++20) generates ==, !=, <, <=, >, >=,
// comparing members in declaration order.
struct Temperature {
    std::uint8_t sensor_id;
    std::int16_t centi_c;               // hundredths of a degree C
    auto operator<=>(const Temperature&) const = default;
};

struct Voltage {
    std::uint8_t  rail;
    std::uint16_t millivolts;
    auto operator<=>(const Voltage&) const = default;
};

struct Fault {
    std::uint16_t code;
    auto operator<=>(const Fault&) const = default;
};

// A tagged union: holds exactly one of these, knows which, no heap.
using Message = std::variant<Temperature, Voltage, Fault>;

// ---------------------------------------------------------------------------
inline constexpr std::byte   kSync{0xA5};
inline constexpr std::size_t kHeaderSize = 3;   // sync, type, len
inline constexpr std::size_t kCrcSize    = 1;
inline constexpr std::size_t kMaxPayload = 8;
inline constexpr std::size_t kMaxFrame   = kHeaderSize + kMaxPayload + kCrcSize;

// Fixed-size and trivially copyable: no heap, safe to memcpy or put in a ring buffer.
struct Frame {
    std::array<std::byte, kMaxFrame> bytes{};
    std::size_t size{0};

    // Non-owning view of the used bytes. Only valid while this Frame lives.
    [[nodiscard]] constexpr std::span<const std::byte> view() const noexcept
    {
        return {bytes.data(), size};
    }
};
static_assert(std::is_trivially_copyable_v<Frame>);

// ---------------------------------------------------------------------------
[[nodiscard]] constexpr MsgType type_of(const Message& m) noexcept
{
    return std::visit(overloaded{
        [](const Temperature&) { return MsgType::Temperature; },
        [](const Voltage&)     { return MsgType::Voltage; },
        [](const Fault&)       { return MsgType::Fault; },
    }, m);
}

[[nodiscard]] constexpr std::string_view name_of(MsgType t) noexcept
{
    switch (t) {   // no `default:` so -Wswitch flags a missing enumerator
        case MsgType::Temperature: return "Temperature";
        case MsgType::Voltage:     return "Voltage";
        case MsgType::Fault:       return "Fault";
    }
    return "?";
}

[[nodiscard]] constexpr Frame encode(const Message& msg) noexcept
{
    Frame f;
    std::span out{f.bytes};                       // CTAD: span<std::byte, kMaxFrame>
    auto payload = out.subspan<kHeaderSize, kMaxPayload>();

    // Each lambda writes its payload and returns its length.
    const std::size_t len = std::visit(overloaded{
        [&](const Temperature& t) -> std::size_t {
            payload[0] = std::byte{t.sensor_id};
            store_le(t.centi_c, payload.subspan<1, 2>());
            return 3;
        },
        [&](const Voltage& v) -> std::size_t {
            payload[0] = std::byte{v.rail};
            store_le(v.millivolts, payload.subspan<1, 2>());
            return 3;
        },
        [&](const Fault& flt) -> std::size_t {
            store_le(flt.code, payload.subspan<0, 2>());
            return 2;
        },
    }, msg);

    out[0] = kSync;
    out[1] = std::byte{std::to_underlying(type_of(msg))};
    out[2] = static_cast<std::byte>(len);
    // CRC covers type, len, and payload; not the sync byte.
    out[kHeaderSize + len] = std::byte{crc8(out.subspan(1, 2 + len))};
    f.size = kHeaderSize + len + kCrcSize;
    return f;
}

} // namespace tlm
