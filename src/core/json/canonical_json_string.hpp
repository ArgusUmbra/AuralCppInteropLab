#ifndef AURAL_CORE_JSON_CANONICAL_JSON_STRING_HPP
#define AURAL_CORE_JSON_CANONICAL_JSON_STRING_HPP

#include "json_limits.hpp"

#include <aural/core/utf8.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace aural::core::json {

// A decoded string can expand by at most six output bytes per input byte
// (U+0000 -> "\\u0000"), plus the two surrounding quotation marks.
inline constexpr std::size_t max_canonical_string_bytes =
    max_manifest_bytes * 6U + 2U;

struct CanonicalStringLimits final {
    std::size_t max_input_bytes = max_manifest_bytes;
    std::size_t max_output_bytes = max_canonical_string_bytes;
};

enum class CanonicalStringErrorCode : std::uint8_t {
    invalid_limits,
    input_limit_exceeded,
    output_limit_exceeded,
    invalid_utf8,
    unicode_noncharacter,
};

struct CanonicalStringError final {
    CanonicalStringErrorCode code;
    std::size_t offset;
    std::optional<Utf8Error> utf8_error;
};

struct CanonicalStringResult final {
    std::optional<std::u8string> serialized;
    std::optional<CanonicalStringError> error;

    [[nodiscard]] bool valid() const noexcept
    {
        return serialized.has_value();
    }

    [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
};

// Serializes one already-decoded UTF-8 scalar sequence as an RFC 8785 JSON
// string. No normalization is performed. Expected validation failures are
// returned; allocation failure remains exceptional.
[[nodiscard]] CanonicalStringResult serialize_canonical_json_string(
    std::u8string_view input,
    CanonicalStringLimits limits = {});

} // namespace aural::core::json

#endif
