#include "canonical_json_string.hpp"

#include "../utf8_internal.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace aural::core::json {
namespace {

CanonicalStringResult failure(
    const CanonicalStringErrorCode code,
    const std::size_t offset,
    std::optional<Utf8Error> utf8_error = std::nullopt)
{
    return {std::nullopt,
            CanonicalStringError{code, offset, std::move(utf8_error)}};
}

std::span<const std::byte> bytes(const std::u8string_view input) noexcept
{
    return std::as_bytes(std::span{input.data(), input.size()});
}

std::size_t encoded_width(const char32_t scalar) noexcept
{
    if (scalar == U'"' || scalar == U'\\' || scalar == U'\b' ||
        scalar == U'\t' || scalar == U'\n' || scalar == U'\f' ||
        scalar == U'\r') {
        return 2U;
    }
    if (scalar < 0x20U) {
        return 6U;
    }
    if (scalar <= 0x7FU) {
        return 1U;
    }
    if (scalar <= 0x7FFU) {
        return 2U;
    }
    if (scalar <= 0xFFFFU) {
        return 3U;
    }
    return 4U;
}

bool can_add(
    const std::size_t current,
    const std::size_t additional,
    const std::size_t maximum) noexcept
{
    return current <= maximum && additional <= maximum - current;
}

void append_control_escape(std::u8string& output, const char32_t scalar)
{
    switch (scalar) {
    case U'\b': output.append(u8"\\b"); return;
    case U'\t': output.append(u8"\\t"); return;
    case U'\n': output.append(u8"\\n"); return;
    case U'\f': output.append(u8"\\f"); return;
    case U'\r': output.append(u8"\\r"); return;
    default: break;
    }

    constexpr std::array<char8_t, 16> hex{
        u8'0', u8'1', u8'2', u8'3', u8'4', u8'5', u8'6', u8'7',
        u8'8', u8'9', u8'a', u8'b', u8'c', u8'd', u8'e', u8'f'};
    output.append(u8"\\u00");
    const auto value = static_cast<std::uint8_t>(scalar);
    output.push_back(hex[value >> 4U]);
    output.push_back(hex[value & UINT8_C(0x0F)]);
}

} // namespace

CanonicalStringResult serialize_canonical_json_string(
    const std::u8string_view input,
    const CanonicalStringLimits limits)
{
    if (limits.max_input_bytes > max_manifest_bytes ||
        limits.max_output_bytes > max_canonical_string_bytes) {
        return failure(CanonicalStringErrorCode::invalid_limits, 0U);
    }
    if (input.size() > limits.max_input_bytes) {
        return failure(CanonicalStringErrorCode::input_limit_exceeded,
                       limits.max_input_bytes);
    }
    if (limits.max_output_bytes < 2U) {
        return failure(CanonicalStringErrorCode::output_limit_exceeded, 0U);
    }

    const auto input_bytes = bytes(input);
    std::size_t output_size = 2U;
    std::size_t offset = 0U;
    while (offset < input_bytes.size()) {
        const auto decoded = detail::decode_utf8_scalar(input_bytes, offset);
        if (decoded.error.has_value()) {
            return failure(CanonicalStringErrorCode::invalid_utf8,
                           decoded.error->offset, decoded.error);
        }
        const auto scalar = *decoded.scalar;
        if (detail::is_unicode_noncharacter(scalar.value)) {
            return failure(CanonicalStringErrorCode::unicode_noncharacter,
                           offset);
        }
        const auto additional = encoded_width(scalar.value);
        if (!can_add(output_size, additional, limits.max_output_bytes)) {
            return failure(CanonicalStringErrorCode::output_limit_exceeded,
                           offset);
        }
        output_size += additional;
        offset += static_cast<std::size_t>(scalar.width);
    }

    std::u8string output;
    output.reserve(output_size);
    output.push_back(u8'"');
    offset = 0U;
    while (offset < input_bytes.size()) {
        const auto scalar = *detail::decode_utf8_scalar(input_bytes, offset).scalar;
        if (scalar.value < 0x20U) {
            append_control_escape(output, scalar.value);
        } else if (scalar.value == U'"') {
            output.append(u8"\\\"");
        } else if (scalar.value == U'\\') {
            output.append(u8"\\\\");
        } else {
            output.append(input.substr(offset, scalar.width));
        }
        offset += static_cast<std::size_t>(scalar.width);
    }
    output.push_back(u8'"');
    return {std::move(output), std::nullopt};
}

} // namespace aural::core::json
