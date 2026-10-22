#ifndef AURAL_CORE_ZIP_CLASSIC_LOCAL_LAYOUT_HPP
#define AURAL_CORE_ZIP_CLASSIC_LOCAL_LAYOUT_HPP

#include "classic_central_directory.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace aural::core::zip {

struct ClassicEntryPhysicalLayout final {
    std::uint64_t central_header_offset;
    std::uint64_t local_header_offset;
    std::uint64_t local_header_size;
    std::uint64_t payload_offset;
    std::uint64_t payload_size;
    std::uint64_t payload_end;
    std::vector<std::byte> file_name;
    std::vector<std::byte> extra_field;
};

struct ClassicArchivePhysicalLayout final {
    std::uint64_t file_size;
    std::uint64_t central_directory_offset;
    std::uint64_t central_directory_size;
    std::uint64_t eocd_offset;
    std::uint64_t eocd_size;
    // Entries are owned and ordered by physical local-header offset.
    std::vector<ClassicEntryPhysicalLayout> entries;
};

enum class LocalLayoutStatus : std::uint8_t {
    profile_valid,
    invalid_structure,
    unsupported_archive_feature,
    resource_failure,
    file_failure,
};

enum class LocalLayoutErrorCode : std::uint8_t {
    central_directory_invalid,
    local_header_offset_out_of_bounds,
    local_header_truncated,
    invalid_local_header_signature,
    local_record_length_overflow,
    local_variable_fields_out_of_bounds,
    local_extra_field_header_truncated,
    local_extra_field_data_out_of_bounds,
    prohibited_local_extra_field,
    duplicate_local_zip64_extra_field,
    missing_local_zip64_extra_field,
    unnecessary_local_zip64_extra_field,
    local_zip64_size_sentinels_incomplete,
    local_zip64_extra_required_value_truncated,
    local_zip64_extra_trailing_data,
    version_needed_mismatch,
    flags_mismatch,
    compression_method_mismatch,
    modification_time_mismatch,
    modification_date_mismatch,
    crc32_mismatch,
    compressed_size_mismatch,
    uncompressed_size_mismatch,
    filename_mismatch,
    stored_size_mismatch,
    entry_size_limit_exceeded,
    duplicate_local_header_reference,
    local_structures_overlap,
    header_payload_overlap,
    payloads_overlap,
    entry_intersects_central_directory,
    entry_intersects_eocd,
    payload_truncated,
    archive_prefix_prohibited,
    archive_gap_prohibited,
};

enum class UnsupportedArchiveFeature : std::uint8_t {
    data_descriptor,
    general_purpose_flag,
    compression_method,
    entry_comment,
    archive_comment,
    version_needed,
};

enum class LocalLayoutResource : std::uint8_t {
    allocation_failure,
};

struct LocalLayoutError final {
    LocalLayoutErrorCode code;
    std::uint64_t offset;
};

struct UnsupportedFeatureError final {
    UnsupportedArchiveFeature feature;
    std::uint64_t offset;
};

struct LocalLayoutInspection final {
    LocalLayoutStatus status;
    std::optional<ClassicArchivePhysicalLayout> layout;
    std::optional<LocalLayoutError> error;
    std::optional<UnsupportedFeatureError> unsupported;
    std::optional<CentralDirectoryError> central_error;
    std::optional<CentralDirectoryResource> central_resource;
    std::optional<LocalLayoutResource> resource;
    std::optional<FileError> file_error;

    [[nodiscard]] bool profile_valid() const noexcept
    {
        return status == LocalLayoutStatus::profile_valid;
    }
};

// Validates classic or ZIP64 local records and the Draft 0.2 physical profile.
// Success is not full AURAL verification: payload CRC/SHA-256, manifest,
// inventory/name policy, WAVE identity, and file-type metadata remain separate.
[[nodiscard]] LocalLayoutInspection inspect_classic_local_layout(
    const std::filesystem::path& path,
    ClassicZipLimits limits = {});

[[nodiscard]] std::string_view local_layout_error_message(
    LocalLayoutErrorCode code) noexcept;

} // namespace aural::core::zip

#endif
