#ifndef AURAL_CORE_JSON_JSON_STRING_DECODER_HPP
#define AURAL_CORE_JSON_JSON_STRING_DECODER_HPP

#include "json_limits.hpp"

#include <aural/core/utf8.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace aural::core::json {

struct JsonStringLimits final {
    // Callers may lower either limit but may not raise it above the frozen
    // manifest ceiling. The source limit includes both quotation marks.
    std::size_t max_source_bytes = max_manifest_bytes;
    std::size_t max_decoded_bytes = max_manifest_bytes;
};

enum class JsonStringErrorCode : std::uint8_t {
    invalid_limits,
    source_limit_exceeded,
    decoded_limit_exceeded,
    missing_opening_quote,
    missing_closing_quote,
    unescaped_control_character,
    dangling_escape,
    unsupported_escape,
    incomplete_unicode_escape,
    invalid_unicode_hex_digit,
    unpaired_high_surrogate,
    unpaired_low_surrogate,
    invalid_literal_utf8,
    unicode_noncharacter,
};

struct JsonStringError final {
    JsonStringErrorCode code;

    // Byte offset in the supplied span. Missing bytes are reported at the
    // end offset. Scalar-policy errors point to the scalar or escape start.
    std::size_t offset;

    // Present when invalid literal UTF-8 caused the failure. Its offset is
    // also relative to the supplied JSON token span.
    std::optional<Utf8Error> utf8_error;
};

struct DecodedJsonString final {
    // Borrowed view of the exact token bytes, including quotation marks.
    // It remains valid only while the caller's input storage remains valid.
    std::span<const std::byte> source;

    // UTF-8 encoding of decoded Unicode scalar values. No normalization is
    // performed. Escaped NUL is represented by an embedded zero byte.
    std::u8string value;
    std::size_t source_bytes_consumed;
};

struct JsonStringDecodeResult final {
    std::optional<DecodedJsonString> decoded;
    std::optional<JsonStringError> error;

    [[nodiscard]] bool valid() const noexcept
    {
        return decoded.has_value();
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return valid();
    }
};

// Decodes exactly one JSON string token at input[0]. Trailing bytes are left
// unconsumed. Expected validation failures are returned; std::bad_alloc from
// the owned decoded value is deliberately not converted into validation.
[[nodiscard]] JsonStringDecodeResult decode_json_string(
    std::span<const std::byte> input,
    JsonStringLimits limits = {});

} // namespace aural::core::json

#endif
