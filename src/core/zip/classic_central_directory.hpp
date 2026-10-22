#ifndef AURAL_CORE_ZIP_CLASSIC_CENTRAL_DIRECTORY_HPP
#define AURAL_CORE_ZIP_CLASSIC_CENTRAL_DIRECTORY_HPP

#include <aural/core/file_reader.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace aural::core::zip {

inline constexpr std::uint64_t maximum_aural_archive_size = UINT64_C(8606711808);
inline constexpr std::uint64_t maximum_aural_central_directory_size =
    UINT64_C(4194304);
inline constexpr std::uint64_t maximum_aural_entry_count = UINT64_C(2);

struct ClassicZipLimits final {
    std::uint64_t maximum_archive_size = maximum_aural_archive_size;
    std::uint64_t maximum_central_directory_size =
        maximum_aural_central_directory_size;
    std::uint64_t maximum_entry_count = maximum_aural_entry_count;
};

struct CentralDirectoryEntry final {
    std::uint64_t header_offset;
    std::uint16_t version_made_by;
    std::uint16_t version_needed;
    std::uint16_t flags;
    std::uint16_t compression_method;
    std::uint16_t modification_time;
    std::uint16_t modification_date;
    std::uint32_t declared_crc32;
    std::uint32_t declared_compressed_size;
    std::uint32_t declared_uncompressed_size;
    std::uint16_t declared_disk_start;
    std::uint32_t declared_local_header_offset;
    std::uint64_t compressed_size;
    std::uint64_t uncompressed_size;
    std::uint32_t disk_start;
    std::uint16_t internal_attributes;
    std::uint32_t external_attributes;
    std::uint64_t local_header_offset;
    bool uses_zip64;
    std::vector<std::byte> file_name;
    std::uint16_t extra_field_length;
    std::uint16_t comment_length;
};

struct ClassicCentralDirectory final {
    std::uint64_t file_size;
    std::uint64_t central_directory_offset;
    std::uint64_t central_directory_size;
    std::uint64_t eocd_offset;
    std::uint64_t eocd_size;
    std::uint16_t archive_comment_length;
    bool uses_zip64;
    std::optional<std::uint64_t> zip64_eocd_offset;
    std::optional<std::uint64_t> zip64_locator_offset;
    std::vector<CentralDirectoryEntry> entries;
};

enum class CentralDirectoryStatus : std::uint8_t {
    structurally_valid,
    invalid_structure,
    zip64_entry_resolution_pending,
    resource_failure,
    file_failure,
};

enum class CentralDirectoryErrorCode : std::uint8_t {
    archive_too_small,
    archive_size_limit_exceeded,
    eocd_not_found,
    eocd_record_truncated,
    eocd_not_terminal,
    zip64_locator_missing,
    zip64_locator_truncated,
    invalid_zip64_locator_signature,
    zip64_locator_offset_out_of_bounds,
    zip64_locator_multidisk,
    zip64_eocd_truncated,
    invalid_zip64_eocd_signature,
    invalid_zip64_eocd_size,
    invalid_zip64_version_needed,
    zip64_eocd_out_of_bounds,
    zip64_eocd_end_mismatch,
    zip64_eocd_multidisk,
    classic_zip64_value_mismatch,
    multidisk_unsupported,
    central_directory_size_limit_exceeded,
    entry_count_limit_exceeded,
    central_directory_out_of_bounds,
    central_directory_overlaps_eocd,
    central_directory_end_mismatch,
    central_header_truncated,
    invalid_central_header_signature,
    central_entry_length_overflow,
    central_entry_out_of_bounds,
    extra_field_header_truncated,
    extra_field_data_out_of_bounds,
    prohibited_extra_field,
    duplicate_zip64_extra_field,
    missing_zip64_extra_field,
    unnecessary_zip64_extra_field,
    zip64_extra_required_value_truncated,
    zip64_extra_trailing_data,
    zip64_disk_start_multidisk,
    entry_count_mismatch,
    local_header_offset_out_of_bounds,
    local_header_range_overlaps_directory,
    local_header_ranges_overlap,
};

enum class Zip64RequiredField : std::uint8_t {
    eocd_disk_number,
    eocd_central_directory_disk,
    eocd_entries_on_disk,
    eocd_total_entries,
    eocd_central_directory_size,
    eocd_central_directory_offset,
    entry_disk_number,
    entry_compressed_size,
    entry_uncompressed_size,
    entry_local_header_offset,
};

enum class CentralDirectoryResource : std::uint8_t {
    allocation_failure,
};

struct CentralDirectoryError final {
    CentralDirectoryErrorCode code;
    std::uint64_t offset;
};

struct Zip64Required final {
    Zip64RequiredField field;
    std::uint64_t offset;
};

struct CentralDirectoryInspection final {
    CentralDirectoryStatus status;
    std::optional<ClassicCentralDirectory> directory;
    std::optional<CentralDirectoryError> error;
    std::optional<Zip64Required> zip64;
    std::optional<CentralDirectoryResource> resource;
    std::optional<FileError> file_error;

    [[nodiscard]] bool structurally_valid() const noexcept
    {
        return status == CentralDirectoryStatus::structurally_valid;
    }
};

// Opens one stable regular file read-only and validates only its classic EOCD
// and central-directory records. Success does not establish AURAL package
// validity and says nothing about local-header agreement or entry data.
[[nodiscard]] CentralDirectoryInspection inspect_classic_central_directory(
    const std::filesystem::path& path,
    ClassicZipLimits limits = {});

[[nodiscard]] std::string_view central_directory_error_message(
    CentralDirectoryErrorCode code) noexcept;

} // namespace aural::core::zip

#endif
