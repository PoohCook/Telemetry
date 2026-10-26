// Frame parser: bytes in, Message or ParseError out. No exceptions, no heap.
#pragma once

#include "byte_order.hpp"
#include "crc8.hpp"
#include "messages.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>     // std::expected (C++23)
#include <span>
#include <string_view>

namespace tlm {

enum class ParseError : std::uint8_t {
    TooShort,           // fewer bytes than the smallest possible frame
    BadSync,            // first byte isn't 0xA5
    BadLength,          // length byte disagrees with the frame size, or exceeds the max
    BadCrc,             // checksum mismatch
    UnknownType,        // valid frame, but a type byte we don't recognize
    BadPayloadSize,     // valid frame, known type, wrong payload length for that type
};

[[nodiscard]] constexpr std::string_view to_string(ParseError e) noexcept
{
    switch (e) {
        case ParseError::TooShort:       return "TooShort";
        case ParseError::BadSync:        return "BadSync";
        case ParseError::BadLength:      return "BadLength";
        case ParseError::BadCrc:         return "BadCrc";
        case ParseError::UnknownType:    return "UnknownType";
        case ParseError::BadPayloadSize: return "BadPayloadSize";
    }
    return "?";
}

using ParseResult = std::expected<Message, ParseError>;

// Checks run cheapest-first, and nothing is interpreted until the CRC passes:
// a corrupted type or length byte is reported as a framing/CRC error rather
// than decoded as garbage.
[[nodiscard]] constexpr ParseResult parse(std::span<const std::byte> frame) noexcept
{
    if (frame.size() < kHeaderSize + kCrcSize) {
        return std::unexpected(ParseError::TooShort);
    }
    if (frame[0] != kSync) {
        return std::unexpected(ParseError::BadSync);
    }

    const auto len = std::to_integer<std::size_t>(frame[2]);
    if (len > kMaxPayload || frame.size() != kHeaderSize + len + kCrcSize) {
        return std::unexpected(ParseError::BadLength);
    }

    const auto received_crc = std::to_integer<std::uint8_t>(frame[kHeaderSize + len]);
    if (crc8(frame.subspan(1, 2 + len)) != received_crc) {
        return std::unexpected(ParseError::BadCrc);
    }

    const auto payload = frame.subspan(kHeaderSize, len);

    // Any byte value is a valid MsgType object (its underlying type is uint8_t),
    // so this cast is fine; the switch decides whether we know it.
    switch (static_cast<MsgType>(std::to_integer<std::uint8_t>(frame[1]))) {
        case MsgType::Temperature:
            if (len != 3) return std::unexpected(ParseError::BadPayloadSize);
            return Temperature{std::to_integer<std::uint8_t>(payload[0]),
                               load_le<std::int16_t>(payload.subspan<1, 2>())};

        case MsgType::Voltage:
            if (len != 3) return std::unexpected(ParseError::BadPayloadSize);
            return Voltage{std::to_integer<std::uint8_t>(payload[0]),
                           load_le<std::uint16_t>(payload.subspan<1, 2>())};

        case MsgType::Fault:
            if (len != 2) return std::unexpected(ParseError::BadPayloadSize);
            return Fault{load_le<std::uint16_t>(payload.subspan<0, 2>())};
    }
    // Reached only for byte values that match no enumerator.
    return std::unexpected(ParseError::UnknownType);
}

} // namespace tlm
