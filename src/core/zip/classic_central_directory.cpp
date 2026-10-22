#include "classic_central_directory.hpp"
#include "classic_central_directory_internal.hpp"
#include "zip64_end_records.hpp"

#include <aural/core/range.hpp>

#include <limits>
#include <new>
#include <span>
#include <utility>

namespace aural::core::zip {
namespace {

constexpr std::uint32_t central_header_signature = UINT32_C(0x02014b50);
constexpr std::uint64_t central_header_fixed_size = 46;
constexpr std::uint64_t local_header_fixed_size = 30;

std::uint16_t read_u16(const std::span<const std::byte> bytes,
                       const std::size_t offset) noexcept
{
    return static_cast<std::uint16_t>(
        std::to_integer<std::uint8_t>(bytes[offset])
        | (static_cast<std::uint16_t>(
               std::to_integer<std::uint8_t>(bytes[offset + 1U]))
           << 8U));
}

std::uint32_t read_u32(const std::span<const std::byte> bytes,
                       const std::size_t offset) noexcept
{
    return static_cast<std::uint32_t>(
        std::to_integer<std::uint8_t>(bytes[offset])
        | (static_cast<std::uint32_t>(
               std::to_integer<std::uint8_t>(bytes[offset + 1U]))
           << 8U)
        | (static_cast<std::uint32_t>(
               std::to_integer<std::uint8_t>(bytes[offset + 2U]))
           << 16U)
        | (static_cast<std::uint32_t>(
               std::to_integer<std::uint8_t>(bytes[offset + 3U]))
           << 24U));
}

std::uint64_t read_u64(const std::span<const std::byte> bytes,
                       const std::size_t offset) noexcept
{
    return static_cast<std::uint64_t>(read_u32(bytes, offset))
        | (static_cast<std::uint64_t>(read_u32(bytes, offset + 4U)) << 32U);
}

CentralDirectoryInspection invalid(const CentralDirectoryErrorCode code,
                                   const std::uint64_t offset)
{
    return {CentralDirectoryStatus::invalid_structure,
            std::nullopt,
            CentralDirectoryError{code, offset},
            std::nullopt,
            std::nullopt,
            std::nullopt};
}

CentralDirectoryInspection file_failure(const FileError error)
{
    return {CentralDirectoryStatus::file_failure,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            error};
}

CentralDirectoryInspection allocation_failure()
{
    return {CentralDirectoryStatus::resource_failure,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            CentralDirectoryResource::allocation_failure,
            std::nullopt};
}

struct ResolvedCentralValues final {
    std::uint64_t compressed_size;
    std::uint64_t uncompressed_size;
    std::uint64_t local_header_offset;
    std::uint32_t disk_start;
    bool uses_zip64;
};

std::uint64_t first_required_field_offset(const bool uncompressed_required,
                                          const bool compressed_required,
                                          const bool offset_required,
                                          const std::uint64_t absolute) noexcept
{
    if (uncompressed_required) return absolute + 24U;
    if (compressed_required) return absolute + 20U;
    if (offset_required) return absolute + 42U;
    return absolute + 34U;
}

std::optional<CentralDirectoryInspection> resolve_entry_values(
    const std::span<const std::byte> bytes,
    const std::size_t cursor,
    const std::uint64_t absolute,
    const std::uint16_t name_length,
    const std::uint16_t extra_length,
    ResolvedCentralValues& resolved)
{
    const std::uint32_t declared_compressed = read_u32(bytes, cursor + 20U);
    const std::uint32_t declared_uncompressed = read_u32(bytes, cursor + 24U);
    const std::uint16_t declared_disk = read_u16(bytes, cursor + 34U);
    const std::uint32_t declared_offset = read_u32(bytes, cursor + 42U);
    const bool uncompressed_required = declared_uncompressed == UINT32_MAX;
    const bool compressed_required = declared_compressed == UINT32_MAX;
    const bool offset_required = declared_offset == UINT32_MAX;
    const bool disk_required = declared_disk == UINT16_MAX;
    const bool zip64_required = uncompressed_required || compressed_required
        || offset_required || disk_required;

    resolved = {declared_compressed, declared_uncompressed, declared_offset,
                declared_disk, false};

    std::uint64_t expected_size = 0U;
    if (uncompressed_required) expected_size += 8U;
    if (compressed_required) expected_size += 8U;
    if (offset_required) expected_size += 8U;
    if (disk_required) expected_size += 4U;

    const std::size_t extra_begin = cursor + central_header_fixed_size
        + static_cast<std::size_t>(name_length);
    const std::size_t extra_end = extra_begin + extra_length;
    std::size_t extra_cursor = extra_begin;
    bool found_zip64 = false;
    while (extra_cursor < extra_end) {
        const std::uint64_t extra_absolute = absolute
            + static_cast<std::uint64_t>(extra_cursor - cursor);
        if (extra_end - extra_cursor < 4U) {
            return invalid(CentralDirectoryErrorCode::extra_field_header_truncated,
                           extra_absolute);
        }
        const std::uint16_t header_id = read_u16(bytes, extra_cursor);
        const std::uint16_t data_size = read_u16(bytes, extra_cursor + 2U);
        const std::size_t data_begin = extra_cursor + 4U;
        if (static_cast<std::size_t>(data_size) > extra_end - data_begin) {
            return invalid(CentralDirectoryErrorCode::extra_field_data_out_of_bounds,
                           extra_absolute + 2U);
        }
        if (header_id != UINT16_C(0x0001)) {
            return invalid(CentralDirectoryErrorCode::prohibited_extra_field,
                           extra_absolute);
        }
        if (found_zip64) {
            return invalid(CentralDirectoryErrorCode::duplicate_zip64_extra_field,
                           extra_absolute);
        }
        if (!zip64_required) {
            return invalid(CentralDirectoryErrorCode::unnecessary_zip64_extra_field,
                           extra_absolute);
        }
        found_zip64 = true;
        if (data_size < expected_size) {
            return invalid(
                CentralDirectoryErrorCode::zip64_extra_required_value_truncated,
                extra_absolute + 4U + data_size);
        }
        if (data_size > expected_size) {
            return invalid(CentralDirectoryErrorCode::zip64_extra_trailing_data,
                           extra_absolute + 4U + expected_size);
        }

        std::size_t value_cursor = data_begin;
        if (uncompressed_required) {
            resolved.uncompressed_size = read_u64(bytes, value_cursor);
            value_cursor += 8U;
        }
        if (compressed_required) {
            resolved.compressed_size = read_u64(bytes, value_cursor);
            value_cursor += 8U;
        }
        if (offset_required) {
            resolved.local_header_offset = read_u64(bytes, value_cursor);
            value_cursor += 8U;
        }
        if (disk_required) {
            resolved.disk_start = read_u32(bytes, value_cursor);
            if (resolved.disk_start != 0U) {
                return invalid(CentralDirectoryErrorCode::zip64_disk_start_multidisk,
                               extra_absolute
                                   + static_cast<std::uint64_t>(value_cursor
                                                                - extra_cursor));
            }
        }
        resolved.uses_zip64 = true;
        extra_cursor = data_begin + data_size;
    }

    if (zip64_required && !found_zip64) {
        return invalid(CentralDirectoryErrorCode::missing_zip64_extra_field,
                       first_required_field_offset(uncompressed_required,
                                                   compressed_required,
                                                   offset_required, absolute));
    }
    return std::nullopt;
}

CentralDirectoryInspection inspect_open_file(const FileReader& reader,
                                             const ClassicZipLimits& limits,
                                             const bool defer_local_overlap_checks)
{
    auto end_inspection = inspect_zip_end_records_open(reader, limits);
    if (!end_inspection.structurally_valid()) {
        switch (end_inspection.status) {
        case ZipEndRecordStatus::invalid_structure:
            return invalid(end_inspection.error->code,
                           end_inspection.error->offset);
        case ZipEndRecordStatus::resource_failure:
            return allocation_failure();
        case ZipEndRecordStatus::file_failure:
            return file_failure(*end_inspection.file_error);
        case ZipEndRecordStatus::structurally_valid:
            break;
        }
    }
    const auto& end_records = *end_inspection.records;

    const std::uint64_t file_size = end_records.file_size;
    const std::uint64_t directory_offset =
        end_records.central_directory_offset;
    const std::uint64_t directory_size = end_records.central_directory_size;

    std::vector<std::byte> bytes(static_cast<std::size_t>(directory_size));
    const auto read = reader.read_at(directory_offset, bytes);
    if (!read) return file_failure(read.error());

    std::vector<CentralDirectoryEntry> entries;
    entries.reserve(static_cast<std::size_t>(end_records.entry_count));
    std::size_t cursor = 0;
    while (cursor < bytes.size()) {
        const auto absolute_value = checked_add(directory_offset, cursor);
        if (!absolute_value) {
            return invalid(CentralDirectoryErrorCode::central_entry_length_overflow,
                           directory_offset);
        }
        const std::uint64_t absolute = *absolute_value;
        if (static_cast<std::uint64_t>(entries.size())
            >= limits.maximum_entry_count) {
            return invalid(CentralDirectoryErrorCode::entry_count_limit_exceeded,
                           absolute);
        }
        const std::size_t remaining = bytes.size() - cursor;
        if (remaining < central_header_fixed_size) {
            return invalid(CentralDirectoryErrorCode::central_header_truncated,
                           absolute);
        }
        if (read_u32(bytes, cursor) != central_header_signature) {
            return invalid(
                CentralDirectoryErrorCode::invalid_central_header_signature,
                absolute);
        }
        const std::uint16_t name_length = read_u16(bytes, cursor + 28U);
        const std::uint16_t extra_length = read_u16(bytes, cursor + 30U);
        const std::uint16_t comment_length = read_u16(bytes, cursor + 32U);
        const auto variable_length = checked_add(
            name_length, static_cast<std::uint64_t>(extra_length));
        const auto complete_variable_length = variable_length
            ? checked_add(*variable_length, comment_length)
            : std::nullopt;
        const auto record_length = complete_variable_length
            ? checked_add(central_header_fixed_size, *complete_variable_length)
            : std::nullopt;
        if (!record_length) {
            return invalid(
                CentralDirectoryErrorCode::central_entry_length_overflow,
                absolute + 28U);
        }
        if (*record_length > remaining) {
            return invalid(CentralDirectoryErrorCode::central_entry_out_of_bounds,
                           absolute + 28U);
        }

        ResolvedCentralValues resolved{};
        if (auto failure = resolve_entry_values(bytes, cursor, absolute,
                                                name_length, extra_length,
                                                resolved)) {
            return std::move(*failure);
        }
        if (resolved.disk_start != 0U) {
            return invalid(CentralDirectoryErrorCode::multidisk_unsupported,
                           absolute + 34U);
        }
        const std::uint64_t local_offset = resolved.local_header_offset;
        const auto local_fixed_end = checked_add(local_offset,
                                                 local_header_fixed_size);
        if (!local_fixed_end) {
            return invalid(
                CentralDirectoryErrorCode::local_header_offset_out_of_bounds,
                absolute + 42U);
        }
        if (!defer_local_overlap_checks
            && (local_offset >= directory_offset
                || *local_fixed_end > directory_offset)) {
            return invalid(
                CentralDirectoryErrorCode::local_header_range_overlaps_directory,
                absolute + 42U);
        }
        if (!defer_local_overlap_checks) {
            for (const auto& previous : entries) {
                const std::uint64_t previous_end =
                    previous.local_header_offset + local_header_fixed_size;
                if (local_offset < previous_end
                    && previous.local_header_offset < *local_fixed_end) {
                    return invalid(
                        CentralDirectoryErrorCode::local_header_ranges_overlap,
                        absolute + 42U);
                }
            }
        }

        CentralDirectoryEntry entry{
            absolute,
            read_u16(bytes, cursor + 4U),
            read_u16(bytes, cursor + 6U),
            read_u16(bytes, cursor + 8U),
            read_u16(bytes, cursor + 10U),
            read_u16(bytes, cursor + 12U),
            read_u16(bytes, cursor + 14U),
            read_u32(bytes, cursor + 16U),
            read_u32(bytes, cursor + 20U),
            read_u32(bytes, cursor + 24U),
            read_u16(bytes, cursor + 34U),
            read_u32(bytes, cursor + 42U),
            resolved.compressed_size,
            resolved.uncompressed_size,
            resolved.disk_start,
            read_u16(bytes, cursor + 36U),
            read_u32(bytes, cursor + 38U),
            local_offset,
            resolved.uses_zip64,
            {},
            extra_length,
            comment_length};
        const auto name_begin = bytes.begin()
            + static_cast<std::ptrdiff_t>(cursor + central_header_fixed_size);
        entry.file_name.assign(name_begin, name_begin + name_length);
        entries.push_back(std::move(entry));
        cursor += static_cast<std::size_t>(*record_length);
    }

    if (entries.size() != end_records.entry_count) {
        return invalid(CentralDirectoryErrorCode::entry_count_mismatch,
                       end_records.uses_zip64
                           ? *end_records.zip64_eocd_offset + 32U
                           : end_records.eocd_offset + 10U);
    }

    ClassicCentralDirectory directory{
        file_size,
        directory_offset,
        directory_size,
        end_records.eocd_offset,
        end_records.eocd_size,
        end_records.archive_comment_length,
        end_records.uses_zip64,
        end_records.zip64_eocd_offset,
        end_records.zip64_locator_offset,
        std::move(entries)};
    return {CentralDirectoryStatus::structurally_valid,
            std::move(directory),
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt};
}

} // namespace

CentralDirectoryInspection inspect_classic_central_directory_open(
    const FileReader& reader,
    const ClassicZipLimits limits,
    const bool defer_local_header_overlap_checks)
{
    try {
        return inspect_open_file(reader, limits,
                                 defer_local_header_overlap_checks);
    } catch (const std::bad_alloc&) {
        return allocation_failure();
    }
}

CentralDirectoryInspection inspect_classic_central_directory(
    const std::filesystem::path& path,
    const ClassicZipLimits limits)
{
    try {
        auto opened = FileReader::open(path);
        if (!opened) return file_failure(opened.error());
        return inspect_classic_central_directory_open(opened.value(), limits);
    } catch (const std::bad_alloc&) {
        return allocation_failure();
    }
}

std::string_view central_directory_error_message(
    const CentralDirectoryErrorCode code) noexcept
{
    switch (code) {
    case CentralDirectoryErrorCode::archive_too_small:
        return "archive is too small for a classic EOCD record";
    case CentralDirectoryErrorCode::archive_size_limit_exceeded:
        return "archive exceeds the configured physical-size limit";
    case CentralDirectoryErrorCode::eocd_not_found:
        return "terminal classic EOCD signature was not found";
    case CentralDirectoryErrorCode::eocd_record_truncated:
        return "EOCD record or declared comment is truncated";
    case CentralDirectoryErrorCode::eocd_not_terminal:
        return "bytes follow the declared EOCD record";
    case CentralDirectoryErrorCode::zip64_locator_missing:
        return "required ZIP64 EOCD locator is missing";
    case CentralDirectoryErrorCode::zip64_locator_truncated:
        return "required ZIP64 EOCD locator is truncated";
    case CentralDirectoryErrorCode::invalid_zip64_locator_signature:
        return "ZIP64 EOCD locator signature is invalid";
    case CentralDirectoryErrorCode::zip64_locator_offset_out_of_bounds:
        return "ZIP64 EOCD locator references an impossible record offset";
    case CentralDirectoryErrorCode::zip64_locator_multidisk:
        return "ZIP64 EOCD locator declares a multidisk archive";
    case CentralDirectoryErrorCode::zip64_eocd_truncated:
        return "ZIP64 EOCD record is truncated";
    case CentralDirectoryErrorCode::invalid_zip64_eocd_signature:
        return "ZIP64 EOCD record signature is invalid";
    case CentralDirectoryErrorCode::invalid_zip64_eocd_size:
        return "ZIP64 EOCD record is not the required version-1 size";
    case CentralDirectoryErrorCode::invalid_zip64_version_needed:
        return "ZIP64 EOCD version-needed field is not 4.5";
    case CentralDirectoryErrorCode::zip64_eocd_out_of_bounds:
        return "ZIP64 EOCD record is outside the physical archive";
    case CentralDirectoryErrorCode::zip64_eocd_end_mismatch:
        return "ZIP64 EOCD record does not end at its locator";
    case CentralDirectoryErrorCode::zip64_eocd_multidisk:
        return "ZIP64 EOCD record declares a multidisk archive";
    case CentralDirectoryErrorCode::classic_zip64_value_mismatch:
        return "classic EOCD and ZIP64 EOCD values contradict each other";
    case CentralDirectoryErrorCode::multidisk_unsupported:
        return "multidisk ZIP structure is unsupported";
    case CentralDirectoryErrorCode::central_directory_size_limit_exceeded:
        return "central directory exceeds the configured byte limit";
    case CentralDirectoryErrorCode::entry_count_limit_exceeded:
        return "central directory exceeds the configured entry limit";
    case CentralDirectoryErrorCode::central_directory_out_of_bounds:
        return "central-directory range is outside the physical file";
    case CentralDirectoryErrorCode::central_directory_overlaps_eocd:
        return "central directory overlaps the EOCD";
    case CentralDirectoryErrorCode::central_directory_end_mismatch:
        return "central directory does not end at the EOCD";
    case CentralDirectoryErrorCode::central_header_truncated:
        return "central-directory fixed header is truncated";
    case CentralDirectoryErrorCode::invalid_central_header_signature:
        return "central-directory header signature is invalid";
    case CentralDirectoryErrorCode::central_entry_length_overflow:
        return "central-directory variable lengths overflow";
    case CentralDirectoryErrorCode::central_entry_out_of_bounds:
        return "central-directory entry exceeds the declared directory";
    case CentralDirectoryErrorCode::extra_field_header_truncated:
        return "central extra-field header is truncated";
    case CentralDirectoryErrorCode::extra_field_data_out_of_bounds:
        return "central extra-field data exceeds the declared extra region";
    case CentralDirectoryErrorCode::prohibited_extra_field:
        return "central extra-field ID is prohibited by AURAL";
    case CentralDirectoryErrorCode::duplicate_zip64_extra_field:
        return "central entry contains duplicate ZIP64 extra fields";
    case CentralDirectoryErrorCode::missing_zip64_extra_field:
        return "central sentinel requires a ZIP64 extra field";
    case CentralDirectoryErrorCode::unnecessary_zip64_extra_field:
        return "central ZIP64 extra field has no corresponding sentinel";
    case CentralDirectoryErrorCode::zip64_extra_required_value_truncated:
        return "central ZIP64 extra field omits a required value";
    case CentralDirectoryErrorCode::zip64_extra_trailing_data:
        return "central ZIP64 extra field contains prohibited trailing data";
    case CentralDirectoryErrorCode::zip64_disk_start_multidisk:
        return "central ZIP64 disk-start value declares a multidisk archive";
    case CentralDirectoryErrorCode::entry_count_mismatch:
        return "EOCD entry count does not match parsed records";
    case CentralDirectoryErrorCode::local_header_offset_out_of_bounds:
        return "declared local-header offset cannot be represented";
    case CentralDirectoryErrorCode::local_header_range_overlaps_directory:
        return "declared local-header fixed range overlaps the directory";
    case CentralDirectoryErrorCode::local_header_ranges_overlap:
        return "declared local-header fixed ranges overlap";
    }
    return "unknown central-directory error";
}

} // namespace aural::core::zip
