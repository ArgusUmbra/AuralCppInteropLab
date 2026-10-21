#include "json_value_parser.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace aural::core::json {
namespace {

constexpr std::uint8_t byte_value(const std::byte value) noexcept
{
    return std::to_integer<std::uint8_t>(value);
}

constexpr bool is_whitespace(const std::uint8_t value) noexcept
{
    return value == static_cast<std::uint8_t>(' ') ||
           value == static_cast<std::uint8_t>('\t') ||
           value == static_cast<std::uint8_t>('\n') ||
           value == static_cast<std::uint8_t>('\r');
}

void skip_whitespace(
    const std::span<const std::byte> input,
    std::size_t& offset) noexcept
{
    while (offset < input.size() && is_whitespace(byte_value(input[offset]))) {
        ++offset;
    }
}

JsonValueParseResult failure(
    const JsonValueErrorCode code,
    const std::size_t offset,
    std::optional<JsonStringError> string_error = std::nullopt,
    std::optional<JsonPrimitiveError> primitive_error = std::nullopt)
{
    return {std::nullopt,
            JsonValueError{code, offset, std::move(string_error),
                           std::move(primitive_error)}};
}

struct ArrayFrame final {
    std::size_t source_begin;
    JsonValue::Array elements;
    bool first = true;
    bool after_comma = false;
};

} // namespace

JsonValueParseResult parse_json_value(
    const std::span<const std::byte> input,
    const JsonValueLimits limits,
    const bool require_complete)
{
    if (limits.max_source_bytes > max_manifest_bytes ||
        limits.max_depth > max_json_container_depth ||
        limits.max_values > max_manifest_bytes ||
        limits.max_decoded_string_bytes > max_manifest_bytes) {
        return failure(JsonValueErrorCode::invalid_limits, 0);
    }
    if (input.size() > limits.max_source_bytes) {
        return failure(JsonValueErrorCode::source_limit_exceeded,
                       limits.max_source_bytes);
    }

    std::size_t offset = 0;
    skip_whitespace(input, offset);
    if (offset == input.size()) {
        return failure(JsonValueErrorCode::empty_input, offset);
    }

    std::vector<ArrayFrame> frames;
    frames.reserve(limits.max_depth);
    std::optional<JsonValue> root;
    std::size_t value_count = 0;
    std::size_t decoded_string_bytes = 0;
    bool need_value = true;

    const auto account_value = [&]() -> bool {
        if (value_count >= limits.max_values) {
            return false;
        }
        ++value_count;
        return true;
    };

    const auto deliver = [&](JsonValue value) {
        if (frames.empty()) {
            root.emplace(std::move(value));
        } else {
            auto& frame = frames.back();
            frame.elements.emplace_back(std::move(value));
            frame.first = false;
            frame.after_comma = false;
            need_value = false;
        }
    };

    while (!root.has_value()) {
        skip_whitespace(input, offset);

        if (need_value) {
            if (offset == input.size()) {
                if (!frames.empty() && frames.back().after_comma) {
                    return failure(JsonValueErrorCode::expected_value, offset);
                }
                return failure(JsonValueErrorCode::missing_closing_bracket, offset);
            }

            const auto current = byte_value(input[offset]);
            if (current == static_cast<std::uint8_t>(']')) {
                if (frames.empty()) {
                    return failure(JsonValueErrorCode::unexpected_closing_bracket,
                                   offset);
                }
                auto& frame = frames.back();
                if (frame.after_comma) {
                    return failure(JsonValueErrorCode::trailing_comma, offset);
                }
                if (!frame.first) {
                    return failure(JsonValueErrorCode::expected_value, offset);
                }
                ++offset;
                JsonValue array{std::move(frame.elements),
                                JsonSourceRange{frame.source_begin, offset}};
                frames.pop_back();
                deliver(std::move(array));
                continue;
            }
            if (current == static_cast<std::uint8_t>('{')) {
                return failure(JsonValueErrorCode::unsupported_object, offset);
            }
            if (current == static_cast<std::uint8_t>('[')) {
                if (frames.size() >= limits.max_depth) {
                    return failure(JsonValueErrorCode::nesting_limit_exceeded,
                                   offset);
                }
                if (!account_value()) {
                    return failure(JsonValueErrorCode::value_limit_exceeded, offset);
                }
                frames.push_back(ArrayFrame{offset, {}});
                ++offset;
                need_value = true;
                continue;
            }
            if (current == static_cast<std::uint8_t>(',')) {
                return failure(JsonValueErrorCode::expected_value, offset);
            }
            if (!account_value()) {
                return failure(JsonValueErrorCode::value_limit_exceeded, offset);
            }

            const auto source_begin = offset;
            const auto remaining = input.subspan(offset);
            if (current == static_cast<std::uint8_t>('"')) {
                const auto remaining_decoded =
                    limits.max_decoded_string_bytes - decoded_string_bytes;
                auto decoded = decode_json_string(
                    remaining,
                    JsonStringLimits{remaining.size(), remaining_decoded});
                if (!decoded) {
                    auto detail = *decoded.error;
                    const auto absolute = offset + detail.offset;
                    const auto code =
                        detail.code == JsonStringErrorCode::decoded_limit_exceeded
                            ? JsonValueErrorCode::decoded_string_limit_exceeded
                            : JsonValueErrorCode::string_error;
                    detail.offset = absolute;
                    if (detail.utf8_error.has_value()) {
                        detail.utf8_error->offset += offset;
                    }
                    return failure(code, absolute, std::move(detail));
                }
                const auto consumed = decoded.decoded->source_bytes_consumed;
                const auto decoded_size = decoded.decoded->value.size();
                decoded_string_bytes += decoded_size;
                offset += consumed;
                deliver(JsonValue{std::move(decoded.decoded->value),
                                  JsonSourceRange{source_begin, offset}});
                continue;
            }

            JsonLiteralParseResult literal;
            bool is_literal = true;
            if (current == static_cast<std::uint8_t>('t')) {
                literal = parse_json_true(remaining,
                                          JsonPrimitiveLimits{remaining.size()});
            } else if (current == static_cast<std::uint8_t>('f')) {
                literal = parse_json_false(remaining,
                                           JsonPrimitiveLimits{remaining.size()});
            } else if (current == static_cast<std::uint8_t>('n')) {
                literal = parse_json_null(remaining,
                                          JsonPrimitiveLimits{remaining.size()});
            } else {
                is_literal = false;
            }

            if (is_literal) {
                if (!literal) {
                    auto detail = *literal.error;
                    const auto absolute = offset + detail.offset;
                    detail.offset = absolute;
                    return failure(JsonValueErrorCode::primitive_error, absolute,
                                   std::nullopt, detail);
                }
                offset += literal.parsed->source_bytes_consumed;
                JsonValue::Storage storage = nullptr;
                if (literal.parsed->value == JsonLiteral::true_value) {
                    storage = true;
                } else if (literal.parsed->value == JsonLiteral::false_value) {
                    storage = false;
                }
                deliver(JsonValue{std::move(storage),
                                  JsonSourceRange{source_begin, offset}});
                continue;
            }

            if (current == static_cast<std::uint8_t>('-') ||
                current == static_cast<std::uint8_t>('+') ||
                (current >= static_cast<std::uint8_t>('0') &&
                 current <= static_cast<std::uint8_t>('9'))) {
                const auto integer = parse_json_integer(
                    remaining, JsonPrimitiveLimits{remaining.size()});
                if (!integer) {
                    auto detail = *integer.error;
                    const auto absolute = offset + detail.offset;
                    detail.offset = absolute;
                    return failure(JsonValueErrorCode::primitive_error, absolute,
                                   std::nullopt, detail);
                }
                offset += integer.parsed->source_bytes_consumed;
                deliver(JsonValue{integer.parsed->value,
                                  JsonSourceRange{source_begin, offset}});
                continue;
            }
            return failure(JsonValueErrorCode::expected_value, offset);
        }

        if (frames.empty()) {
            break;
        }
        if (offset == input.size()) {
            return failure(JsonValueErrorCode::missing_closing_bracket, offset);
        }
        const auto current = byte_value(input[offset]);
        if (current == static_cast<std::uint8_t>(',')) {
            ++offset;
            frames.back().after_comma = true;
            need_value = true;
            continue;
        }
        if (current == static_cast<std::uint8_t>(']')) {
            ++offset;
            auto& frame = frames.back();
            JsonValue array{std::move(frame.elements),
                            JsonSourceRange{frame.source_begin, offset}};
            frames.pop_back();
            deliver(std::move(array));
            continue;
        }
        return failure(JsonValueErrorCode::expected_comma_or_closing_bracket,
                       offset);
    }

    if (!root.has_value()) {
        return failure(JsonValueErrorCode::expected_value, offset);
    }
    if (require_complete) {
        skip_whitespace(input, offset);
        if (offset != input.size()) {
            return failure(JsonValueErrorCode::unexpected_extra_token, offset);
        }
    }
    return {ParsedJsonValue{std::move(*root), offset}, std::nullopt};
}

std::string_view json_value_error_message(const JsonValueErrorCode code) noexcept
{
    switch (code) {
    case JsonValueErrorCode::invalid_limits:
        return "value parser limits exceed frozen AURAL ceilings";
    case JsonValueErrorCode::source_limit_exceeded:
        return "JSON input exceeds the configured source limit";
    case JsonValueErrorCode::empty_input: return "expected a JSON value";
    case JsonValueErrorCode::expected_value: return "expected a JSON value";
    case JsonValueErrorCode::unsupported_object:
        return "JSON objects are not supported by this milestone";
    case JsonValueErrorCode::unexpected_closing_bracket:
        return "unexpected array closing bracket";
    case JsonValueErrorCode::missing_closing_bracket:
        return "array is missing its closing bracket";
    case JsonValueErrorCode::expected_comma_or_closing_bracket:
        return "expected a comma or array closing bracket";
    case JsonValueErrorCode::trailing_comma:
        return "trailing commas are not permitted";
    case JsonValueErrorCode::nesting_limit_exceeded:
        return "JSON container depth exceeds 32";
    case JsonValueErrorCode::value_limit_exceeded:
        return "JSON value count exceeds the configured limit";
    case JsonValueErrorCode::decoded_string_limit_exceeded:
        return "aggregate decoded JSON string bytes exceed the configured limit";
    case JsonValueErrorCode::string_error: return "invalid JSON string";
    case JsonValueErrorCode::primitive_error: return "invalid JSON primitive";
    case JsonValueErrorCode::unexpected_extra_token:
        return "unexpected bytes follow the JSON value";
    }
    return "unknown JSON value error";
}

} // namespace aural::core::json
