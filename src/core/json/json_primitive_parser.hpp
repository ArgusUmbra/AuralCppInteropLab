#ifndef AURAL_CORE_JSON_JSON_PRIMITIVE_PARSER_HPP
#define AURAL_CORE_JSON_JSON_PRIMITIVE_PARSER_HPP

#include "json_limits.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace aural::core::json {

inline constexpr std::int64_t min_aural_json_integer = -9'007'199'254'740'991LL;
inline constexpr std::int64_t max_aural_json_integer = 9'007'199'254'740'991LL;

struct JsonPrimitiveLimits final {
    // A primitive is read from a view into the bounded manifest. Callers may
    // lower, but not raise, the frozen manifest ceiling.
    std::size_t max_source_bytes = max_manifest_bytes;
};

enum class JsonPrimitiveErrorCode : std::uint8_t {
    invalid_limits,
    source_limit_exceeded,
    empty_input,
    expected_integer,
    leading_plus_sign,
    missing_integer_digit,
    leading_zero,
    negative_zero,
    fraction_not_permitted,
    exponent_not_permitted,
    integer_out_of_range,
    truncated_literal,
    literal_mismatch,
    invalid_token_boundary,
};

struct JsonPrimitiveError final {
    JsonPrimitiveErrorCode code;
    // Byte offset in the supplied span. A missing byte is reported at the
    // end offset; a bad boundary points to the first byte after the token.
    std::size_t offset;
};

struct ParsedJsonInteger final {
    std::int64_t value;
    // Exact borrowed token spelling. Valid only while caller storage lives.
    std::span<const std::byte> source;
    std::size_t source_bytes_consumed;
};

struct JsonIntegerParseResult final {
    std::optional<ParsedJsonInteger> parsed;
    std::optional<JsonPrimitiveError> error;

    [[nodiscard]] bool valid() const noexcept { return parsed.has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
};

enum class JsonLiteral : std::uint8_t {
    true_value,
    false_value,
    null_value,
};

struct ParsedJsonLiteral final {
    JsonLiteral value;
    std::span<const std::byte> source;
    std::size_t source_bytes_consumed;
};

struct JsonLiteralParseResult final {
    std::optional<ParsedJsonLiteral> parsed;
    std::optional<JsonPrimitiveError> error;

    [[nodiscard]] bool valid() const noexcept { return parsed.has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
};

// Parses one token beginning at input[0]. Leading whitespace is deliberately
// not consumed. A following byte must be a JSON value terminator: whitespace,
// comma, ']', or '}'. The terminator remains unconsumed.
[[nodiscard]] JsonIntegerParseResult parse_json_integer(
    std::span<const std::byte> input,
    JsonPrimitiveLimits limits = {}) noexcept;

[[nodiscard]] JsonLiteralParseResult parse_json_true(
    std::span<const std::byte> input,
    JsonPrimitiveLimits limits = {}) noexcept;

[[nodiscard]] JsonLiteralParseResult parse_json_false(
    std::span<const std::byte> input,
    JsonPrimitiveLimits limits = {}) noexcept;

[[nodiscard]] JsonLiteralParseResult parse_json_null(
    std::span<const std::byte> input,
    JsonPrimitiveLimits limits = {}) noexcept;

[[nodiscard]] std::string_view json_primitive_error_message(
    JsonPrimitiveErrorCode code) noexcept;

} // namespace aural::core::json

#endif
