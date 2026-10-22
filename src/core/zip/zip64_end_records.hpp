#ifndef AURAL_CORE_ZIP_ZIP64_END_RECORDS_HPP
#define AURAL_CORE_ZIP_ZIP64_END_RECORDS_HPP

#include "classic_central_directory.hpp"

#include <aural/core/file_reader.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>

namespace aural::core::zip {

struct ResolvedZipEndRecords final {
    std::uint64_t file_size;
    std::uint64_t central_directory_offset;
    std::uint64_t central_directory_size;
    std::uint64_t entry_count;
    std::uint64_t eocd_offset;
    std::uint64_t eocd_size;
    std::uint16_t archive_comment_length;
    bool uses_zip64;
    std::optional<std::uint64_t> zip64_eocd_offset;
    std::optional<std::uint64_t> zip64_locator_offset;
};

enum class ZipEndRecordStatus : std::uint8_t {
    structurally_valid,
    invalid_structure,
    resource_failure,
    file_failure,
};

struct ZipEndRecordInspection final {
    ZipEndRecordStatus status;
    std::optional<ResolvedZipEndRecords> records;
    std::optional<CentralDirectoryError> error;
    std::optional<CentralDirectoryResource> resource;
    std::optional<FileError> file_error;

    [[nodiscard]] bool structurally_valid() const noexcept
    {
        return status == ZipEndRecordStatus::structurally_valid;
    }
};

// Internal ZIP foundation entry points. They resolve archive-level ZIP64
// records; per-entry fields are resolved by the central and local stages.
[[nodiscard]] ZipEndRecordInspection inspect_zip_end_records(
    const std::filesystem::path& path,
    ClassicZipLimits limits = {});

[[nodiscard]] ZipEndRecordInspection inspect_zip_end_records_open(
    const FileReader& reader,
    ClassicZipLimits limits = {});

} // namespace aural::core::zip

#endif
