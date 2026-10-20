#include "json_primitive_parser.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace aural::core::json {
namespace {

constexpr std::uint8_t byte_value(const std::byte value) noexcept
{
    return std::to_integer<std::uint8_t>(value);
}

constexpr bool is_digit(const std::uint8_t value) noexcept
{
    return value >= static_cast<std::uint8_t>('0') &&
           value <= static_cast<std::uint8_t>('9');
}

constexpr bool is_value_terminator(const std::uint8_t value) noexcept
{
    return value == static_cast<std::uint8_t>(' ') ||
           value == static_cast<std::uint8_t>('\t') ||
           value == static_cast<std::uint8_t>('\n') ||
           value == static_cast<std::uint8_t>('\r') ||
           value == static_cast<std::uint8_t>(',') ||
           value == static_cast<std::uint8_t>(']') ||
           value == static_cast<std::uint8_t>('}');
}

std::optional<JsonPrimitiveError> validate_limits(
    const std::span<const std::byte> input,
    const JsonPrimitiveLimits limits) noexcept
{
    if (limits.max_source_bytes > max_manifest_bytes) {
        return JsonPrimitiveError{JsonPrimitiveErrorCode::invalid_limits, 0};
    }
    if (input.size() > limits.max_source_bytes) {
        return JsonPrimitiveError{JsonPrimitiveErrorCode::source_limit_exceeded,
                                  limits.max_source_bytes};
    }
    return std::nullopt;
}

std::optional<JsonPrimitiveError> validate_boundary(
    const std::span<const std::byte> input,
    const std::size_t consumed) noexcept
{
    if (consumed < input.size() && !is_value_terminator(byte_value(input[consumed]))) {
        return JsonPrimitiveError{JsonPrimitiveErrorCode::invalid_token_boundary,
                                  consumed};
    }
    return std::nullopt;
}

JsonIntegerParseResult integer_failure(
    const JsonPrimitiveErrorCode code,
    const std::size_t offset) noexcept
{
    return {std::nullopt, JsonPrimitiveError{code, offset}};
}

JsonLiteralParseResult literal_failure(
    const JsonPrimitiveErrorCode code,
    const std::size_t offset) noexcept
{
    return {std::nullopt, JsonPrimitiveError{code, offset}};
}

template <std::size_t Size>
JsonLiteralParseResult parse_literal(
    const std::span<const std::byte> input,
    const std::array<std::uint8_t, Size>& spelling,
    const JsonLiteral value,
    const JsonPrimitiveLimits limits) noexcept
{
    if (const auto error = validate_limits(input, limits)) {
        return {std::nullopt, error};
    }

    for (std::size_t offset = 0; offset < spelling.size(); ++offset) {
        if (offset >= input.size()) {
            return literal_failure(JsonPrimitiveErrorCode::truncated_literal, offset);
        }
        if (byte_value(input[offset]) != spelling[offset]) {
            return literal_failure(JsonPrimitiveErrorCode::literal_mismatch, offset);
        }
    }

    if (const auto error = validate_boundary(input, spelling.size())) {
        return {std::nullopt, error};
    }
    return {ParsedJsonLiteral{value, input.first(spelling.size()), spelling.size()},
            std::nullopt};
}

} // namespace

JsonIntegerParseResult parse_json_integer(
    const std::span<const std::byte> input,
    const JsonPrimitiveLimits limits) noexcept
{
    if (const auto error = validate_limits(input, limits)) {
        return {std::nullopt, error};
    }
    if (input.empty()) {
        return integer_failure(JsonPrimitiveErrorCode::empty_input, 0);
    }

    std::size_t offset = 0;
    bool negative = false;
    const auto first = byte_value(input[0]);
    if (first == static_cast<std::uint8_t>('+')) {
        return integer_failure(JsonPrimitiveErrorCode::leading_plus_sign, 0);
    }
    if (first == static_cast<std::uint8_t>('-')) {
        negative = true;
        ++offset;
        if (offset >= input.size() || !is_digit(byte_value(input[offset]))) {
            return integer_failure(JsonPrimitiveErrorCode::missing_integer_digit,
                                   offset);
        }
    } else if (!is_digit(first)) {
        return integer_failure(JsonPrimitiveErrorCode::expected_integer, 0);
    }

    if (byte_value(input[offset]) == static_cast<std::uint8_t>('0')) {
        ++offset;
        if (offset < input.size() && is_digit(byte_value(input[offset]))) {
            return integer_failure(JsonPrimitiveErrorCode::leading_zero, offset);
        }
        if (offset < input.size()) {
            const auto next = byte_value(input[offset]);
            if (next == static_cast<std::uint8_t>('.')) {
                return integer_failure(JsonPrimitiveErrorCode::fraction_not_permitted,
                                       offset);
            }
            if (next == static_cast<std::uint8_t>('e') ||
                next == static_cast<std::uint8_t>('E')) {
                return integer_failure(JsonPrimitiveErrorCode::exponent_not_permitted,
                                       offset);
            }
        }
        if (const auto error = validate_boundary(input, offset)) {
            return {std::nullopt, error};
        }
        if (negative) {
            return integer_failure(JsonPrimitiveErrorCode::negative_zero, 0);
        }
        return {ParsedJsonInteger{0, input.first(offset), offset}, std::nullopt};
    }

    constexpr auto maximum = static_cast<std::uint64_t>(max_aural_json_integer);
    std::uint64_t magnitude = 0;
    while (offset < input.size() && is_digit(byte_value(input[offset]))) {
        const auto digit = static_cast<std::uint64_t>(
            byte_value(input[offset]) - static_cast<std::uint8_t>('0'));
        if (magnitude > (maximum - digit) / 10U) {
            return integer_failure(JsonPrimitiveErrorCode::integer_out_of_range,
                                   offset);
        }
        magnitude = magnitude * 10U + digit;
        ++offset;
    }

    if (offset < input.size()) {
        const auto next = byte_value(input[offset]);
        if (next == static_cast<std::uint8_t>('.')) {
            return integer_failure(JsonPrimitiveErrorCode::fraction_not_permitted,
                                   offset);
        }
        if (next == static_cast<std::uint8_t>('e') ||
            next == static_cast<std::uint8_t>('E')) {
            return integer_failure(JsonPrimitiveErrorCode::exponent_not_permitted,
                                   offset);
        }
    }
    if (const auto error = validate_boundary(input, offset)) {
        return {std::nullopt, error};
    }
    const auto signed_magnitude = static_cast<std::int64_t>(magnitude);
    const auto value = negative ? -signed_magnitude : signed_magnitude;
    return {ParsedJsonInteger{value, input.first(offset), offset}, std::nullopt};
}

JsonLiteralParseResult parse_json_true(
    const std::span<const std::byte> input,
    const JsonPrimitiveLimits limits) noexcept
{
    constexpr std::array<std::uint8_t, 4> spelling{
        static_cast<std::uint8_t>('t'), static_cast<std::uint8_t>('r'),
        static_cast<std::uint8_t>('u'), static_cast<std::uint8_t>('e')};
    return parse_literal(input, spelling, JsonLiteral::true_value, limits);
}

JsonLiteralParseResult parse_json_false(
    const std::span<const std::byte> input,
    const JsonPrimitiveLimits limits) noexcept
{
    constexpr std::array<std::uint8_t, 5> spelling{
        static_cast<std::uint8_t>('f'), static_cast<std::uint8_t>('a'),
        static_cast<std::uint8_t>('l'), static_cast<std::uint8_t>('s'),
        static_cast<std::uint8_t>('e')};
    return parse_literal(input, spelling, JsonLiteral::false_value, limits);
}

JsonLiteralParseResult parse_json_null(
    const std::span<const std::byte> input,
    const JsonPrimitiveLimits limits) noexcept
{
    constexpr std::array<std::uint8_t, 4> spelling{
        static_cast<std::uint8_t>('n'), static_cast<std::uint8_t>('u'),
        static_cast<std::uint8_t>('l'), static_cast<std::uint8_t>('l')};
    return parse_literal(input, spelling, JsonLiteral::null_value, limits);
}

std::string_view json_primitive_error_message(
    const JsonPrimitiveErrorCode code) noexcept
{
    switch (code) {
    case JsonPrimitiveErrorCode::invalid_limits:
        return "source limit exceeds the AURAL manifest ceiling";
    case JsonPrimitiveErrorCode::source_limit_exceeded:
        return "input span exceeds the configured primitive source limit";
    case JsonPrimitiveErrorCode::empty_input: return "expected an integer";
    case JsonPrimitiveErrorCode::expected_integer: return "expected an integer digit";
    case JsonPrimitiveErrorCode::leading_plus_sign:
        return "a JSON number cannot begin with a plus sign";
    case JsonPrimitiveErrorCode::missing_integer_digit:
        return "a minus sign must be followed by an integer digit";
    case JsonPrimitiveErrorCode::leading_zero:
        return "a multi-digit JSON integer cannot begin with zero";
    case JsonPrimitiveErrorCode::negative_zero:
        return "negative zero is outside the AURAL numeric profile";
    case JsonPrimitiveErrorCode::fraction_not_permitted:
        return "fractions are outside the AURAL integer-only profile";
    case JsonPrimitiveErrorCode::exponent_not_permitted:
        return "exponents are outside the AURAL integer-only profile";
    case JsonPrimitiveErrorCode::integer_out_of_range:
        return "integer is outside the AURAL safe-integer range";
    case JsonPrimitiveErrorCode::truncated_literal: return "JSON literal is truncated";
    case JsonPrimitiveErrorCode::literal_mismatch:
        return "JSON literal must use its exact lowercase spelling";
    case JsonPrimitiveErrorCode::invalid_token_boundary:
        return "primitive is not followed by a valid JSON value terminator";
    }
    return "unknown JSON primitive error";
}

} // namespace aural::core::json
