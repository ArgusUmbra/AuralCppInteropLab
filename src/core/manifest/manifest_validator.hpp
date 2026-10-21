#ifndef AURAL_CORE_MANIFEST_MANIFEST_VALIDATOR_HPP
#define AURAL_CORE_MANIFEST_MANIFEST_VALIDATOR_HPP

#include "../json/canonical_json_document.hpp"
#include "../json/json_value_parser.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace aural::core::manifest {

enum class JsonFieldType : std::uint8_t {
    null_value,
    boolean,
    integer,
    string,
    array,
    object,
};

enum class ManifestStructureStatus : std::uint8_t {
    structurally_valid,
    invalid_structure,
    resource_failure,
};

enum class ManifestErrorCode : std::uint8_t {
    missing_required_field,
    wrong_field_type,
    unsupported_version,
    invalid_fixed_value,
    unknown_field,
    invalid_field_constraint,
    invalid_extension_structure,
    unsupported_critical_extension,
};

enum class ManifestConstraint : std::uint8_t {
    none,
    format_identifier,
    original_path,
    asset_count,
    asset_path,
    asset_role,
    asset_media_type,
    sha256_syntax,
    asset_size_range,
    title_scalar_limit,
    recorded_at_timestamp,
    extension_identifier,
    duplicate_critical_extension,
    critical_extension_missing_value,
};

enum class ManifestResource : std::uint8_t {
    allocation_failure,
};

struct ManifestError final {
    ManifestErrorCode code;
    // Safe JSON Pointer display path. An empty path identifies the document
    // root. RFC 6901 '~' and '/' escaping is used, with ASCII controls further
    // percent-encoded to prevent diagnostic injection. Rejected values are
    // never included.
    std::u8string path;
    ManifestConstraint constraint = ManifestConstraint::none;
    std::optional<JsonFieldType> expected_type;
    std::optional<JsonFieldType> actual_type;
};

struct ManifestStructureResult final {
    ManifestStructureStatus status;
    std::optional<ManifestError> error;
    std::optional<ManifestResource> resource;

    [[nodiscard]] bool structurally_valid() const noexcept
    {
        return status == ManifestStructureStatus::structurally_valid;
    }
};

// Validates only the Draft 0.2 manifest data-model shape and local field
// constraints. The caller retains ownership; the already-owned tree is read
// through const access and is never changed. This does not validate package
// membership, declared bytes, hashes, WAVE identity, or full package validity.
[[nodiscard]] ManifestStructureResult validate_manifest_structure(
    const json::JsonValue& document) noexcept;

enum class ManifestDocumentStatus : std::uint8_t {
    structurally_valid,
    malformed_json,
    noncanonical_json,
    invalid_manifest_structure,
    resource_failure,
};

enum class ManifestDocumentResource : std::uint8_t {
    input_limit_exceeded,
    nesting_limit_exceeded,
    value_limit_exceeded,
    decoded_string_limit_exceeded,
    canonical_output_limit_exceeded,
    allocation_failure,
    internal_serialization_failure,
};

struct ManifestDocumentResult final {
    ManifestDocumentStatus status;
    std::optional<std::size_t> mismatch_offset;
    std::optional<json::JsonValueError> parse_error;
    std::optional<json::CanonicalDocumentError> serialization_error;
    std::optional<ManifestError> manifest_error;
    std::optional<ManifestDocumentResource> resource;

    [[nodiscard]] bool structurally_valid() const noexcept
    {
        return status == ManifestDocumentStatus::structurally_valid;
    }
};

// Combined first-layer entry point. It parses once into an owned tree, checks
// exact JCS bytes, then checks Draft 0.2 structure. Success deliberately means
// only canonical JSON plus valid manifest structure, never package validity.
[[nodiscard]] ManifestDocumentResult validate_canonical_manifest_structure(
    std::span<const std::byte> input) noexcept;

[[nodiscard]] std::string_view manifest_error_message(
    ManifestErrorCode code) noexcept;
[[nodiscard]] std::string_view manifest_constraint_message(
    ManifestConstraint constraint) noexcept;
[[nodiscard]] std::string_view manifest_document_status_message(
    ManifestDocumentStatus status) noexcept;

} // namespace aural::core::manifest

#endif
