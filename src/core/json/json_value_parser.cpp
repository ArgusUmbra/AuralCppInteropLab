#include "json_value_parser.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
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

bool has_leading_bom(const std::span<const std::byte> input) noexcept
{
    return input.size() >= 3U && byte_value(input[0]) == 0xEFU &&
           byte_value(input[1]) == 0xBBU && byte_value(input[2]) == 0xBFU;
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

struct ObjectFrame final {
    std::size_t source_begin;
    JsonValue::Object members;
    // Open-addressed slots store member indices, not pointers. Member-vector
    // growth therefore cannot invalidate the duplicate-name index.
    std::vector<std::size_t> name_slots;
    std::optional<std::u8string> pending_name;
    JsonSourceRange pending_name_source{};
    bool first = true;
    bool after_comma = false;
};

using ContainerFrame = std::variant<ArrayFrame, ObjectFrame>;

constexpr std::size_t empty_name_slot =
    std::numeric_limits<std::size_t>::max();

std::size_t hash_name(const std::u8string_view name) noexcept
{
    // FNV-1a is sufficient for a bounded, non-cryptographic lookup index.
    std::size_t hash = sizeof(std::size_t) == 8U
                           ? static_cast<std::size_t>(14695981039346656037ULL)
                           : static_cast<std::size_t>(2166136261U);
    const std::size_t prime = sizeof(std::size_t) == 8U
                                  ? static_cast<std::size_t>(1099511628211ULL)
                                  : static_cast<std::size_t>(16777619U);
    for (const auto byte : name) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= prime;
    }
    return hash;
}

bool contains_name(
    const ObjectFrame& object,
    const std::u8string_view name) noexcept
{
    if (object.name_slots.empty()) {
        return false;
    }
    const auto mask = object.name_slots.size() - 1U;
    auto slot = hash_name(name) & mask;
    for (std::size_t probe = 0; probe < object.name_slots.size(); ++probe) {
        const auto member_index = object.name_slots[slot];
        if (member_index == empty_name_slot) {
            return false;
        }
        if (object.members[member_index].name == name) {
            return true;
        }
        slot = (slot + 1U) & mask;
    }
    return false;
}

void insert_member_index(
    std::vector<std::size_t>& slots,
    const JsonValue::Object& members,
    const std::size_t member_index) noexcept
{
    const auto mask = slots.size() - 1U;
    auto slot = hash_name(members[member_index].name) & mask;
    while (slots[slot] != empty_name_slot) {
        slot = (slot + 1U) & mask;
    }
    slots[slot] = member_index;
}

void prepare_name_slot(ObjectFrame& object, const std::size_t needed_members)
{
    auto capacity = object.name_slots.size();
    if (capacity != 0U && needed_members <= capacity / 2U) {
        return;
    }
    if (capacity == 0U) {
        capacity = 8U;
    }
    while (needed_members > capacity / 2U) {
        if (capacity > std::numeric_limits<std::size_t>::max() / 2U) {
            throw std::length_error("JSON object duplicate index is too large");
        }
        capacity *= 2U;
    }

    std::vector<std::size_t> replacement(capacity, empty_name_slot);
    for (std::size_t index = 0; index < object.members.size(); ++index) {
        insert_member_index(replacement, object.members, index);
    }
    object.name_slots = std::move(replacement);
}

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
    if (has_leading_bom(input)) {
        return failure(JsonValueErrorCode::leading_bom, 0);
    }

    std::size_t offset = 0;
    skip_whitespace(input, offset);
    if (offset == input.size()) {
        return failure(JsonValueErrorCode::empty_input, offset);
    }

    std::vector<ContainerFrame> frames;
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
            return;
        }

        auto& parent = frames.back();
        if (auto* array = std::get_if<ArrayFrame>(&parent)) {
            array->elements.emplace_back(std::move(value));
            array->first = false;
            array->after_comma = false;
        } else {
            auto& object = std::get<ObjectFrame>(parent);
            const auto member_begin = object.pending_name_source.begin;
            const auto member_end = value.source.end;
            if (object.members.size() ==
                std::numeric_limits<std::size_t>::max()) {
                throw std::length_error("JSON object has too many members");
            }
            const auto needed_members = object.members.size() + 1U;
            prepare_name_slot(object, needed_members);
            object.members.push_back(
                JsonObjectMember{std::move(*object.pending_name),
                                 object.pending_name_source, std::move(value),
                                 JsonSourceRange{member_begin, member_end}});
            insert_member_index(object.name_slots, object.members,
                                object.members.size() - 1U);
            object.pending_name.reset();
            object.first = false;
            object.after_comma = false;
        }
        need_value = false;
    };

    const auto string_failure = [&](JsonStringDecodeResult& decoded,
                                    const std::size_t token_begin) {
        auto detail = *decoded.error;
        const auto absolute = token_begin + detail.offset;
        const auto code =
            detail.code == JsonStringErrorCode::decoded_limit_exceeded
                ? JsonValueErrorCode::decoded_string_limit_exceeded
                : JsonValueErrorCode::string_error;
        detail.offset = absolute;
        if (detail.utf8_error.has_value()) {
            detail.utf8_error->offset += token_begin;
        }
        return failure(code, absolute, std::move(detail));
    };

    while (!root.has_value()) {
        skip_whitespace(input, offset);

        if (need_value) {
            if (offset == input.size()) {
                if (frames.empty()) {
                    return failure(JsonValueErrorCode::expected_value, offset);
                }
                if (const auto* array =
                        std::get_if<ArrayFrame>(&frames.back())) {
                    return failure(array->after_comma
                                       ? JsonValueErrorCode::expected_value
                                       : JsonValueErrorCode::missing_closing_bracket,
                                   offset);
                }
                return failure(JsonValueErrorCode::expected_value, offset);
            }

            const auto current = byte_value(input[offset]);
            if (current == static_cast<std::uint8_t>(']')) {
                if (frames.empty() ||
                    !std::holds_alternative<ArrayFrame>(frames.back())) {
                    return failure(JsonValueErrorCode::unexpected_closing_bracket,
                                   offset);
                }
                auto& frame = std::get<ArrayFrame>(frames.back());
                if (frame.after_comma) {
                    return failure(JsonValueErrorCode::trailing_comma, offset);
                }
                if (!frame.first) {
                    return failure(JsonValueErrorCode::expected_value, offset);
                }
                ++offset;
                const auto source_begin = frame.source_begin;
                auto elements = std::move(frame.elements);
                frames.pop_back();
                deliver(JsonValue{std::move(elements),
                                  JsonSourceRange{source_begin, offset}});
                continue;
            }
            if (current == static_cast<std::uint8_t>('}')) {
                return failure(JsonValueErrorCode::unexpected_closing_brace,
                               offset);
            }
            if (current == static_cast<std::uint8_t>('[') ||
                current == static_cast<std::uint8_t>('{')) {
                if (frames.size() >= limits.max_depth) {
                    return failure(JsonValueErrorCode::nesting_limit_exceeded,
                                   offset);
                }
                if (!account_value()) {
                    return failure(JsonValueErrorCode::value_limit_exceeded,
                                   offset);
                }
                if (current == static_cast<std::uint8_t>('[')) {
                    frames.emplace_back(ArrayFrame{offset, {}});
                    need_value = true;
                } else {
                    frames.emplace_back(ObjectFrame{offset, {}, {}, std::nullopt,
                                                    {}, true, false});
                    need_value = false;
                }
                ++offset;
                continue;
            }
            if (current == static_cast<std::uint8_t>(',') ||
                current == static_cast<std::uint8_t>(':')) {
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
                    return string_failure(decoded, source_begin);
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
        if (auto* array = std::get_if<ArrayFrame>(&frames.back())) {
            if (offset == input.size()) {
                return failure(JsonValueErrorCode::missing_closing_bracket,
                               offset);
            }
            const auto current = byte_value(input[offset]);
            if (current == static_cast<std::uint8_t>(',')) {
                ++offset;
                array->after_comma = true;
                need_value = true;
                continue;
            }
            if (current == static_cast<std::uint8_t>(']')) {
                ++offset;
                const auto source_begin = array->source_begin;
                auto elements = std::move(array->elements);
                frames.pop_back();
                deliver(JsonValue{std::move(elements),
                                  JsonSourceRange{source_begin, offset}});
                continue;
            }
            return failure(JsonValueErrorCode::expected_comma_or_closing_bracket,
                           offset);
        }

        auto& object = std::get<ObjectFrame>(frames.back());
        if (object.first || object.after_comma) {
            if (offset == input.size()) {
                return failure(JsonValueErrorCode::missing_closing_brace, offset);
            }
            const auto current = byte_value(input[offset]);
            if (current == static_cast<std::uint8_t>('}')) {
                if (object.after_comma) {
                    return failure(JsonValueErrorCode::trailing_comma, offset);
                }
                ++offset;
                const auto source_begin = object.source_begin;
                auto members = std::move(object.members);
                frames.pop_back();
                deliver(JsonValue{std::move(members),
                                  JsonSourceRange{source_begin, offset}});
                continue;
            }
            if (current != static_cast<std::uint8_t>('"')) {
                return failure(JsonValueErrorCode::expected_property_name,
                               offset);
            }

            const auto name_begin = offset;
            const auto remaining_decoded =
                limits.max_decoded_string_bytes - decoded_string_bytes;
            auto decoded = decode_json_string(
                input.subspan(offset),
                JsonStringLimits{input.size() - offset, remaining_decoded});
            if (!decoded) {
                return string_failure(decoded, name_begin);
            }
            const auto decoded_size = decoded.decoded->value.size();
            decoded_string_bytes += decoded_size;
            offset += decoded.decoded->source_bytes_consumed;
            const JsonSourceRange name_source{name_begin, offset};

            auto name = std::move(decoded.decoded->value);
            if (contains_name(object, name)) {
                return failure(JsonValueErrorCode::duplicate_property_name,
                               name_begin);
            }
            object.pending_name.emplace(std::move(name));
            object.pending_name_source = name_source;

            skip_whitespace(input, offset);
            if (offset == input.size() ||
                byte_value(input[offset]) != static_cast<std::uint8_t>(':')) {
                return failure(JsonValueErrorCode::expected_colon, offset);
            }
            ++offset;
            need_value = true;
            continue;
        }

        if (offset == input.size()) {
            return failure(JsonValueErrorCode::missing_closing_brace, offset);
        }
        const auto current = byte_value(input[offset]);
        if (current == static_cast<std::uint8_t>(',')) {
            ++offset;
            object.after_comma = true;
            continue;
        }
        if (current == static_cast<std::uint8_t>('}')) {
            ++offset;
            const auto source_begin = object.source_begin;
            auto members = std::move(object.members);
            frames.pop_back();
            deliver(JsonValue{std::move(members),
                              JsonSourceRange{source_begin, offset}});
            continue;
        }
        return failure(JsonValueErrorCode::expected_comma_or_closing_brace,
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

JsonValueParseResult parse_json_document(
    const std::span<const std::byte> input,
    const JsonValueLimits limits)
{
    return parse_json_value(input, limits, true);
}

std::string_view json_value_error_message(const JsonValueErrorCode code) noexcept
{
    switch (code) {
    case JsonValueErrorCode::invalid_limits:
        return "value parser limits exceed frozen AURAL ceilings";
    case JsonValueErrorCode::source_limit_exceeded:
        return "JSON input exceeds the configured source limit";
    case JsonValueErrorCode::empty_input: return "expected a JSON value";
    case JsonValueErrorCode::leading_bom:
        return "a leading UTF-8 BOM is not permitted";
    case JsonValueErrorCode::expected_value: return "expected a JSON value";
    case JsonValueErrorCode::expected_property_name:
        return "expected a quoted JSON object property name";
    case JsonValueErrorCode::expected_colon:
        return "expected a colon after the object property name";
    case JsonValueErrorCode::duplicate_property_name:
        return "duplicate decoded JSON object property name";
    case JsonValueErrorCode::unexpected_closing_bracket:
        return "unexpected array closing bracket";
    case JsonValueErrorCode::unexpected_closing_brace:
        return "unexpected object closing brace";
    case JsonValueErrorCode::missing_closing_bracket:
        return "array is missing its closing bracket";
    case JsonValueErrorCode::missing_closing_brace:
        return "object is missing its closing brace";
    case JsonValueErrorCode::expected_comma_or_closing_bracket:
        return "expected a comma or array closing bracket";
    case JsonValueErrorCode::expected_comma_or_closing_brace:
        return "expected a comma or object closing brace";
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
