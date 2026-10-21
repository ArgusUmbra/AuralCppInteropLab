#include "json_string_decoder.hpp"

#include "../utf8_internal.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace aural::core::json {
namespace {

constexpr std::uint8_t byte_value(const std::byte value) noexcept
{
    return std::to_integer<std::uint8_t>(value);
}

JsonStringDecodeResult failure(
    const JsonStringErrorCode code,
    const std::size_t offset,
    std::optional<Utf8Error> utf8_error = std::nullopt)
{
    return {std::nullopt, JsonStringError{code, offset, std::move(utf8_error)}};
}

std::optional<std::uint8_t> hex_value(const std::uint8_t value) noexcept
{
    if (value >= static_cast<std::uint8_t>('0') &&
        value <= static_cast<std::uint8_t>('9')) {
        return static_cast<std::uint8_t>(value - static_cast<std::uint8_t>('0'));
    }
    if (value >= static_cast<std::uint8_t>('A') &&
        value <= static_cast<std::uint8_t>('F')) {
        return static_cast<std::uint8_t>(value - static_cast<std::uint8_t>('A') + 10U);
    }
    if (value >= static_cast<std::uint8_t>('a') &&
        value <= static_cast<std::uint8_t>('f')) {
        return static_cast<std::uint8_t>(value - static_cast<std::uint8_t>('a') + 10U);
    }
    return std::nullopt;
}

struct HexQuadResult final {
    std::optional<std::uint16_t> value;
    std::optional<JsonStringError> error;
};

HexQuadResult decode_hex_quad(
    const std::span<const std::byte> input,
    const std::size_t first_digit) noexcept
{
    std::uint16_t value = 0;
    for (std::size_t index = 0; index < 4U; ++index) {
        if (first_digit > input.size() || index >= input.size() - first_digit) {
            return {std::nullopt,
                    JsonStringError{JsonStringErrorCode::incomplete_unicode_escape,
                                    input.size(), std::nullopt}};
        }
        const auto digit = hex_value(byte_value(input[first_digit + index]));
        if (!digit.has_value()) {
            return {std::nullopt,
                    JsonStringError{JsonStringErrorCode::invalid_unicode_hex_digit,
                                    first_digit + index, std::nullopt}};
        }
        value = static_cast<std::uint16_t>((value << 4U) | *digit);
    }
    return {value, std::nullopt};
}

std::array<char8_t, 4> encode_utf8(
    const char32_t scalar,
    std::uint8_t& width) noexcept
{
    std::array<char8_t, 4> bytes{};
    if (scalar <= 0x7FU) {
        width = 1;
        bytes[0] = static_cast<char8_t>(scalar);
    } else if (scalar <= 0x7FFU) {
        width = 2;
        bytes[0] = static_cast<char8_t>(0xC0U | (scalar >> 6U));
        bytes[1] = static_cast<char8_t>(0x80U | (scalar & 0x3FU));
    } else if (scalar <= 0xFFFFU) {
        width = 3;
        bytes[0] = static_cast<char8_t>(0xE0U | (scalar >> 12U));
        bytes[1] = static_cast<char8_t>(0x80U | ((scalar >> 6U) & 0x3FU));
        bytes[2] = static_cast<char8_t>(0x80U | (scalar & 0x3FU));
    } else {
        width = 4;
        bytes[0] = static_cast<char8_t>(0xF0U | (scalar >> 18U));
        bytes[1] = static_cast<char8_t>(0x80U | ((scalar >> 12U) & 0x3FU));
        bytes[2] = static_cast<char8_t>(0x80U | ((scalar >> 6U) & 0x3FU));
        bytes[3] = static_cast<char8_t>(0x80U | (scalar & 0x3FU));
    }
    return bytes;
}

bool can_append(
    const std::size_t current,
    const std::size_t additional,
    const std::size_t maximum) noexcept
{
    return current <= maximum && additional <= maximum - current;
}

std::optional<JsonStringDecodeResult> append_scalar(
    std::u8string& output,
    const char32_t scalar,
    const std::size_t source_offset,
    const std::size_t maximum)
{
    if (detail::is_unicode_noncharacter(scalar)) {
        return failure(JsonStringErrorCode::unicode_noncharacter, source_offset);
    }

    std::uint8_t width = 0;
    const auto bytes = encode_utf8(scalar, width);
    if (!can_append(output.size(), width, maximum)) {
        return failure(JsonStringErrorCode::decoded_limit_exceeded, source_offset);
    }
    output.append(bytes.data(), width);
    return std::nullopt;
}

} // namespace

JsonStringDecodeResult decode_json_string(
    const std::span<const std::byte> input,
    const JsonStringLimits limits)
{
    if (limits.max_source_bytes > max_manifest_bytes ||
        limits.max_decoded_bytes > max_manifest_bytes) {
        return failure(JsonStringErrorCode::invalid_limits, 0);
    }
    if (input.size() > limits.max_source_bytes) {
        return failure(JsonStringErrorCode::source_limit_exceeded,
                       limits.max_source_bytes);
    }
    if (input.empty() || byte_value(input[0]) != static_cast<std::uint8_t>('"')) {
        return failure(JsonStringErrorCode::missing_opening_quote, 0);
    }

    std::u8string output;
    std::size_t offset = 1;
    while (offset < input.size()) {
        const auto current = byte_value(input[offset]);
        if (current == static_cast<std::uint8_t>('"')) {
            const auto consumed = offset + 1U;
            return {DecodedJsonString{input.first(consumed), std::move(output), consumed},
                    std::nullopt};
        }
        if (current < 0x20U) {
            return failure(JsonStringErrorCode::unescaped_control_character, offset);
        }
        if (current == static_cast<std::uint8_t>('\\')) {
            const auto escape_start = offset;
            ++offset;
            if (offset >= input.size()) {
                return failure(JsonStringErrorCode::dangling_escape, offset);
            }

            const auto escaped = byte_value(input[offset]);
            char32_t scalar = 0;
            bool has_scalar = true;
            switch (escaped) {
            case '"': scalar = U'"'; break;
            case '\\': scalar = U'\\'; break;
            case '/': scalar = U'/'; break;
            case 'b': scalar = U'\b'; break;
            case 'f': scalar = U'\f'; break;
            case 'n': scalar = U'\n'; break;
            case 'r': scalar = U'\r'; break;
            case 't': scalar = U'\t'; break;
            case 'u': {
                const auto first = decode_hex_quad(input, offset + 1U);
                if (first.error.has_value()) {
                    return {std::nullopt, first.error};
                }
                const auto unit = *first.value;
                offset += 5U;
                if (unit >= 0xDC00U && unit <= 0xDFFFU) {
                    return failure(JsonStringErrorCode::unpaired_low_surrogate,
                                   escape_start);
                }
                if (unit >= 0xD800U && unit <= 0xDBFFU) {
                    if (offset >= input.size()) {
                        return failure(JsonStringErrorCode::unpaired_high_surrogate,
                                       offset);
                    }
                    if (byte_value(input[offset]) != static_cast<std::uint8_t>('\\') ||
                        offset + 1U >= input.size() ||
                        byte_value(input[offset + 1U]) != static_cast<std::uint8_t>('u')) {
                        return failure(JsonStringErrorCode::unpaired_high_surrogate,
                                       offset);
                    }
                    const auto second_escape = offset;
                    const auto second = decode_hex_quad(input, offset + 2U);
                    if (second.error.has_value()) {
                        return {std::nullopt, second.error};
                    }
                    const auto low = *second.value;
                    if (low < 0xDC00U || low > 0xDFFFU) {
                        return failure(JsonStringErrorCode::unpaired_high_surrogate,
                                       second_escape);
                    }
                    scalar = static_cast<char32_t>(
                        0x10000U + ((static_cast<std::uint32_t>(unit) - 0xD800U) << 10U) +
                        (static_cast<std::uint32_t>(low) - 0xDC00U));
                    offset += 6U;
                } else {
                    scalar = static_cast<char32_t>(unit);
                }
                break;
            }
            default:
                has_scalar = false;
                break;
            }

            if (!has_scalar) {
                return failure(JsonStringErrorCode::unsupported_escape, offset);
            }
            if (const auto append_error = append_scalar(
                    output, scalar, escape_start, limits.max_decoded_bytes)) {
                return *append_error;
            }
            if (escaped != static_cast<std::uint8_t>('u')) {
                ++offset;
            }
            continue;
        }

        const auto decoded = detail::decode_utf8_scalar(input, offset);
        if (decoded.error.has_value()) {
            return failure(JsonStringErrorCode::invalid_literal_utf8,
                           decoded.error->offset, decoded.error);
        }
        const auto scalar = *decoded.scalar;
        if (detail::is_unicode_noncharacter(scalar.value)) {
            return failure(JsonStringErrorCode::unicode_noncharacter, offset);
        }
        if (!can_append(output.size(), scalar.width, limits.max_decoded_bytes)) {
            return failure(JsonStringErrorCode::decoded_limit_exceeded, offset);
        }
        const auto* begin = reinterpret_cast<const char8_t*>(input.data() + offset);
        output.append(begin, scalar.width);
        offset += scalar.width;
    }

    return failure(JsonStringErrorCode::missing_closing_quote, input.size());
}

} // namespace aural::core::json
