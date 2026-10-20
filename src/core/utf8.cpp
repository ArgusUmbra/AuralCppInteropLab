#include "utf8_internal.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace aural::core::detail {
namespace {

[[nodiscard]] std::uint8_t byte_value(const std::byte value) noexcept
{
    return std::to_integer<std::uint8_t>(value);
}

[[nodiscard]] ScalarDecodeResult failure(
    const Utf8ErrorCode code,
    const std::size_t offset,
    const std::optional<std::uint8_t> expected_length = std::nullopt,
    const std::optional<std::uint8_t> observed_length = std::nullopt,
    const std::optional<std::uint8_t> observed_byte = std::nullopt) noexcept
{
    return {
        std::nullopt,
        Utf8Error{code, offset, expected_length, observed_length, observed_byte},
    };
}

[[nodiscard]] ScalarDecodeResult success(
    const char32_t value,
    const std::uint8_t width) noexcept
{
    return {DecodedScalar{value, width}, std::nullopt};
}

[[nodiscard]] bool is_continuation(const std::uint8_t value) noexcept
{
    return (value & UINT8_C(0xC0)) == UINT8_C(0x80);
}

} // namespace

ScalarDecodeResult decode_utf8_scalar(
    const std::span<const std::byte> input,
    const std::size_t offset) noexcept
{
    if (offset >= input.size()) {
        return failure(Utf8ErrorCode::truncated_sequence,
                       input.size(),
                       UINT8_C(1),
                       UINT8_C(0));
    }
    const auto lead = byte_value(input[offset]);
    if (lead <= UINT8_C(0x7F)) {
        return success(static_cast<char32_t>(lead), 1);
    }
    if (lead <= UINT8_C(0xBF)) {
        return failure(Utf8ErrorCode::unexpected_continuation,
                       offset,
                       std::nullopt,
                       std::nullopt,
                       lead);
    }
    if (lead <= UINT8_C(0xC1)) {
        return failure(Utf8ErrorCode::overlong_encoding,
                       offset,
                       std::nullopt,
                       std::nullopt,
                       lead);
    }

    std::uint8_t width = 0;
    std::uint32_t value = 0;
    if (lead <= UINT8_C(0xDF)) {
        width = 2;
        value = static_cast<std::uint32_t>(lead & UINT8_C(0x1F));
    } else if (lead <= UINT8_C(0xEF)) {
        width = 3;
        value = static_cast<std::uint32_t>(lead & UINT8_C(0x0F));
    } else if (lead <= UINT8_C(0xF4)) {
        width = 4;
        value = static_cast<std::uint32_t>(lead & UINT8_C(0x07));
    } else {
        return failure(Utf8ErrorCode::invalid_leading_byte,
                       offset,
                       std::nullopt,
                       std::nullopt,
                       lead);
    }

    const auto remaining = input.size() - offset;
    for (std::uint8_t index = 1; index < width; ++index) {
        if (static_cast<std::size_t>(index) >= remaining) {
            return failure(Utf8ErrorCode::truncated_sequence,
                           input.size(),
                           width,
                           static_cast<std::uint8_t>(remaining));
        }
        const auto current_offset = offset + static_cast<std::size_t>(index);
        const auto current = byte_value(input[current_offset]);
        if (!is_continuation(current)) {
            return failure(Utf8ErrorCode::invalid_continuation_byte,
                           current_offset,
                           width,
                           index,
                           current);
        }
        value = static_cast<std::uint32_t>(
            (value << 6U) | static_cast<std::uint32_t>(current & UINT8_C(0x3F)));
    }

    constexpr std::uint32_t minimum_by_width[] = {0, 0, 0x80, 0x800, 0x10000};
    if (value < minimum_by_width[width]) {
        return failure(Utf8ErrorCode::overlong_encoding, offset, width, width);
    }
    if (value >= UINT32_C(0xD800) && value <= UINT32_C(0xDFFF)) {
        return failure(Utf8ErrorCode::surrogate_code_point, offset, width, width);
    }
    if (value > UINT32_C(0x10FFFF)) {
        return failure(Utf8ErrorCode::code_point_out_of_range, offset, width, width);
    }

    return success(static_cast<char32_t>(value), width);
}

bool is_unicode_noncharacter(const char32_t value) noexcept
{
    const auto scalar = static_cast<std::uint32_t>(value);
    const bool contiguous_range =
        scalar >= UINT32_C(0xFDD0) && scalar <= UINT32_C(0xFDEF);
    const auto plane_offset = scalar & UINT32_C(0xFFFF);
    const bool plane_ending = plane_offset >= UINT32_C(0xFFFE);
    return contiguous_range || plane_ending;
}

} // namespace aural::core::detail

namespace aural::core {

Utf8ValidationResult validate_utf8(
    const std::span<const std::byte> input) noexcept
{
    std::size_t offset = 0;
    while (offset < input.size()) {
        const auto decoded = detail::decode_utf8_scalar(input, offset);
        if (decoded.error) {
            return {decoded.error};
        }
        const auto scalar = *decoded.scalar;
        if (detail::is_unicode_noncharacter(scalar.value)) {
            return {Utf8Error{
                Utf8ErrorCode::noncharacter,
                offset,
                scalar.width,
                scalar.width,
                std::nullopt,
            }};
        }
        offset += static_cast<std::size_t>(scalar.width);
    }
    return {std::nullopt};
}

} // namespace aural::core
