#ifndef AURAL_CORE_JSON_CANONICAL_JSON_DOCUMENT_HPP
#define AURAL_CORE_JSON_CANONICAL_JSON_DOCUMENT_HPP

#include "canonical_json_string.hpp"
#include "json_property_order.hpp"
#include "json_value_parser.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace aural::core::json {

// This is a temporary serialization ceiling, not the maximum permitted size
// of an on-disk AURAL manifest. It bounds canonicalization of an independently
// constructed in-memory value whose strings may expand during escaping.
inline constexpr std::size_t max_canonical_document_bytes =
    max_manifest_bytes * 32U;

struct CanonicalDocumentLimits final {
    std::size_t max_depth = max_json_container_depth;
    std::size_t max_values = max_manifest_bytes;
    std::size_t max_decoded_string_bytes = max_manifest_bytes;
    std::size_t max_output_bytes = max_canonical_document_bytes;
};

enum class CanonicalDocumentErrorCode : std::uint8_t {
    invalid_limits,
    unsupported_internal_value,
    nesting_limit_exceeded,
    value_limit_exceeded,
    decoded_string_limit_exceeded,
    output_limit_exceeded,
    integer_out_of_range,
    invalid_string,
    invalid_property_name,
    duplicate_property_name,
    internal_serialization_failure,
};

struct CanonicalDocumentError final {
    CanonicalDocumentErrorCode code;
    // Deterministic zero-based visitation index in the source-ordered tree.
    std::size_t value_index;
    // Number of containing containers; a root container is reported as 1.
    std::size_t depth;
    std::optional<std::size_t> member_index;
    // Bytes measured before the operation that failed.
    std::size_t output_bytes;
    std::optional<CanonicalStringError> string_error;
    std::optional<CanonicalPropertyOrderError> property_order_error;
};

struct CanonicalDocumentResult final {
    std::optional<std::u8string> serialized;
    std::optional<CanonicalDocumentError> error;

    [[nodiscard]] bool valid() const noexcept
    {
        return serialized.has_value();
    }

    [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
};

// Serializes the closed internal JSON value model as RFC 8785 canonical JSON,
// narrowed to AURAL's safe-integer profile. Validation and exact measurement
// complete before output allocation, and the input tree is never modified.
[[nodiscard]] CanonicalDocumentResult serialize_canonical_json_document(
    const JsonValue& value,
    CanonicalDocumentLimits limits = {});

[[nodiscard]] std::string_view canonical_document_error_message(
    CanonicalDocumentErrorCode code) noexcept;

} // namespace aural::core::json

#endif
