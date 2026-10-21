#ifndef AURAL_CORE_JSON_JSON_VALUE_PARSER_HPP
#define AURAL_CORE_JSON_JSON_VALUE_PARSER_HPP

#include "json_limits.hpp"
#include "json_primitive_parser.hpp"
#include "json_string_decoder.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace aural::core::json {

inline constexpr std::size_t max_json_container_depth = 32U;

struct JsonSourceRange final {
    std::size_t begin;
    std::size_t end;
};

struct JsonObjectMember;

struct JsonValue final {
    using Array = std::vector<JsonValue>;
    using Object = std::vector<JsonObjectMember>;
    using Storage = std::variant<std::nullptr_t, bool, std::int64_t,
                                 std::u8string, Array, Object>;

    Storage storage;
    // Half-open byte range of this value in the original input. Leading and
    // trailing whitespace are excluded; a container range includes delimiters.
    JsonSourceRange source;
};

struct JsonObjectMember final {
    // Decoded UTF-8 property name. Escape spelling is deliberately discarded;
    // Unicode normalization and case folding are deliberately not performed.
    std::u8string name;
    JsonSourceRange name_source;
    JsonValue value;
    // From the opening quote of the name through the end of the value.
    JsonSourceRange source;
};

struct JsonValueLimits final {
    std::size_t max_source_bytes = max_manifest_bytes;
    std::size_t max_depth = max_json_container_depth;
    // Counts primitives, strings, arrays, and objects. Every object member has
    // exactly one value, so this also bounds the aggregate member count.
    std::size_t max_values = max_manifest_bytes;
    // Aggregate decoded bytes across values and object property names.
    std::size_t max_decoded_string_bytes = max_manifest_bytes;
};

enum class JsonValueErrorCode : std::uint8_t {
    invalid_limits,
    source_limit_exceeded,
    empty_input,
    leading_bom,
    expected_value,
    expected_property_name,
    expected_colon,
    duplicate_property_name,
    unexpected_closing_bracket,
    unexpected_closing_brace,
    missing_closing_bracket,
    missing_closing_brace,
    expected_comma_or_closing_bracket,
    expected_comma_or_closing_brace,
    trailing_comma,
    nesting_limit_exceeded,
    value_limit_exceeded,
    decoded_string_limit_exceeded,
    string_error,
    primitive_error,
    unexpected_extra_token,
};

struct JsonValueError final {
    JsonValueErrorCode code;
    // Absolute byte offset in the supplied document span.
    std::size_t offset;
    std::optional<JsonStringError> string_error;
    std::optional<JsonPrimitiveError> primitive_error;
};

struct ParsedJsonValue final {
    JsonValue value;
    // Bytes consumed through the value and, for complete parsing, trailing
    // JSON whitespace. No bytes are borrowed by the returned value.
    std::size_t source_bytes_consumed;
};

struct JsonValueParseResult final {
    std::optional<ParsedJsonValue> parsed;
    std::optional<JsonValueError> error;

    [[nodiscard]] bool valid() const noexcept { return parsed.has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
};

// Parses one supported JSON value after optional leading whitespace. When
// require_complete is true, only trailing JSON whitespace may follow it.
// Expected validation failures do not expose a partial tree. Allocation
// failure remains an exceptional resource failure rather than validation.
[[nodiscard]] JsonValueParseResult parse_json_value(
    std::span<const std::byte> input,
    JsonValueLimits limits = {},
    bool require_complete = true);

// Parses exactly one complete JSON document. This is the preferred internal
// entry point for untrusted document input; it never accepts prefix parses.
[[nodiscard]] JsonValueParseResult parse_json_document(
    std::span<const std::byte> input,
    JsonValueLimits limits = {});

[[nodiscard]] std::string_view json_value_error_message(
    JsonValueErrorCode code) noexcept;

} // namespace aural::core::json

#endif
