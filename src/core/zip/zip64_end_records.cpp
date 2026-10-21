#include "zip64_end_records.hpp"

#include <aural/core/range.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <span>
#include <vector>

namespace aural::core::zip {
namespace {

constexpr std::uint32_t eocd_signature = UINT32_C(0x06054b50);
constexpr std::uint32_t zip64_eocd_signature = UINT32_C(0x06064b50);
constexpr std::uint32_t zip64_locator_signature = UINT32_C(0x07064b50);
constexpr std::uint64_t eocd_fixed_size = 22U;
constexpr std::uint64_t maximum_zip_comment_size = 65535U;
constexpr std::uint64_t zip64_locator_size = 20U;
constexpr std::uint64_t zip64_eocd_total_size = 56U;
constexpr std::uint64_t zip64_eocd_declared_size = 44U;

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

ZipEndRecordInspection invalid(const CentralDirectoryErrorCode code,
                               const std::uint64_t offset)
{
    return {ZipEndRecordStatus::invalid_structure,
            std::nullopt,
            CentralDirectoryError{code, offset},
            std::nullopt,
            std::nullopt};
}

ZipEndRecordInspection file_failure(const FileError error)
{
    return {ZipEndRecordStatus::file_failure,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            error};
}

ZipEndRecordInspection allocation_failure()
{
    return {ZipEndRecordStatus::resource_failure,
            std::nullopt,
            std::nullopt,
            CentralDirectoryResource::allocation_failure,
            std::nullopt};
}

struct ClassicEocd final {
    std::uint64_t offset;
    std::uint64_t size;
    std::uint16_t disk_number;
    std::uint16_t central_directory_disk;
    std::uint16_t entries_on_disk;
    std::uint16_t total_entries;
    std::uint32_t central_directory_size;
    std::uint32_t central_directory_offset;
    std::uint16_t comment_length;
};

struct EocdSearch final {
    std::optional<ClassicEocd> eocd;
    std::optional<ZipEndRecordInspection> failure;
};

EocdSearch find_eocd(const FileReader& reader)
{
    const std::uint64_t file_size = reader.size();
    if (file_size < eocd_fixed_size) {
        return {std::nullopt,
                invalid(CentralDirectoryErrorCode::archive_too_small,
                        file_size)};
    }

    const std::uint64_t search_size = std::min(
        file_size, eocd_fixed_size + maximum_zip_comment_size);
    const std::uint64_t search_offset = file_size - search_size;
    std::vector<std::byte> tail(static_cast<std::size_t>(search_size));
    const auto read = reader.read_at(search_offset, tail);
    if (!read) return {std::nullopt, file_failure(read.error())};

    std::optional<std::uint64_t> truncated_signature;
    std::optional<std::uint64_t> nonterminal_signature;
    for (std::size_t index = tail.size() - 4U;; --index) {
        if (read_u32(tail, index) == eocd_signature) {
            const std::uint64_t absolute = search_offset + index;
            const std::uint64_t remaining = tail.size() - index;
            if (remaining < eocd_fixed_size) {
                truncated_signature = absolute;
            } else {
                const std::uint16_t comment_length = read_u16(tail, index + 20U);
                const auto record_end = checked_add(
                    absolute, eocd_fixed_size + comment_length);
                if (record_end && *record_end == file_size) {
                    return {ClassicEocd{absolute,
                                        eocd_fixed_size + comment_length,
                                        read_u16(tail, index + 4U),
                                        read_u16(tail, index + 6U),
                                        read_u16(tail, index + 8U),
                                        read_u16(tail, index + 10U),
                                        read_u32(tail, index + 12U),
                                        read_u32(tail, index + 16U),
                                        comment_length},
                            std::nullopt};
                }
                if (record_end && *record_end < file_size) {
                    nonterminal_signature = absolute;
                } else {
                    truncated_signature = absolute;
                }
            }
        }
        if (index == 0U) break;
    }

    if (nonterminal_signature) {
        return {std::nullopt,
                invalid(CentralDirectoryErrorCode::eocd_not_terminal,
                        *nonterminal_signature)};
    }
    if (truncated_signature) {
        return {std::nullopt,
                invalid(CentralDirectoryErrorCode::eocd_record_truncated,
                        *truncated_signature)};
    }
    return {std::nullopt,
            invalid(CentralDirectoryErrorCode::eocd_not_found, search_offset)};
}

bool requires_zip64(const ClassicEocd& eocd) noexcept
{
    return eocd.disk_number == UINT16_MAX
        || eocd.central_directory_disk == UINT16_MAX
        || eocd.entries_on_disk == UINT16_MAX
        || eocd.total_entries == UINT16_MAX
        || eocd.central_directory_size == UINT32_MAX
        || eocd.central_directory_offset == UINT32_MAX;
}

template<typename Classic, typename Resolved>
bool compatible(const Classic classic_value,
                const Classic sentinel,
                const Resolved resolved_value) noexcept
{
    return classic_value == sentinel
        || static_cast<std::uint64_t>(classic_value) == resolved_value;
}

std::optional<ZipEndRecordInspection> validate_directory(
    const ResolvedZipEndRecords& records,
    const ClassicZipLimits& limits,
    const std::uint64_t size_field_offset,
    const std::uint64_t count_field_offset,
    const std::uint64_t directory_offset_field_offset,
    const std::uint64_t expected_directory_end)
{
    if (records.central_directory_size
        > limits.maximum_central_directory_size) {
        return invalid(
            CentralDirectoryErrorCode::central_directory_size_limit_exceeded,
            size_field_offset);
    }
    if (records.entry_count > limits.maximum_entry_count) {
        return invalid(CentralDirectoryErrorCode::entry_count_limit_exceeded,
                       count_field_offset);
    }
    const auto range = range_within_file(records.central_directory_offset,
                                         records.central_directory_size,
                                         records.file_size);
    if (!range) {
        return invalid(CentralDirectoryErrorCode::central_directory_out_of_bounds,
                       directory_offset_field_offset);
    }
    if (range->end() > expected_directory_end) {
        return invalid(CentralDirectoryErrorCode::central_directory_overlaps_eocd,
                       records.central_directory_offset);
    }
    if (range->end() != expected_directory_end) {
        return invalid(CentralDirectoryErrorCode::central_directory_end_mismatch,
                       range->end());
    }
    return std::nullopt;
}

ZipEndRecordInspection inspect_open_file(const FileReader& reader,
                                         const ClassicZipLimits& limits)
{
    if (reader.size() > limits.maximum_archive_size) {
        return invalid(CentralDirectoryErrorCode::archive_size_limit_exceeded,
                       limits.maximum_archive_size);
    }

    auto searched = find_eocd(reader);
    if (!searched.eocd) return std::move(*searched.failure);
    const ClassicEocd classic = *searched.eocd;

    if (!requires_zip64(classic)) {
        if (classic.disk_number != 0U || classic.central_directory_disk != 0U
            || classic.entries_on_disk != classic.total_entries) {
            return invalid(CentralDirectoryErrorCode::multidisk_unsupported,
                           classic.offset + 4U);
        }
        ResolvedZipEndRecords records{reader.size(),
                                      classic.central_directory_offset,
                                      classic.central_directory_size,
                                      classic.total_entries,
                                      classic.offset,
                                      classic.size,
                                      classic.comment_length,
                                      false,
                                      std::nullopt,
                                      std::nullopt};
        if (auto failure = validate_directory(
                records, limits, classic.offset + 12U, classic.offset + 10U,
                classic.offset + 16U, classic.offset)) {
            return std::move(*failure);
        }
        return {ZipEndRecordStatus::structurally_valid,
                std::move(records),
                std::nullopt,
                std::nullopt,
                std::nullopt};
    }

    if (classic.central_directory_offset != UINT32_MAX
        && classic.central_directory_size != UINT32_MAX) {
        const auto classic_directory_end = checked_add(
            classic.central_directory_offset,
            classic.central_directory_size);
        if (classic_directory_end && *classic_directory_end <= classic.offset) {
            const std::uint64_t available =
                classic.offset - *classic_directory_end;
            if (available < zip64_locator_size) {
                return invalid(
                    available == 0U
                        ? CentralDirectoryErrorCode::zip64_locator_missing
                        : CentralDirectoryErrorCode::zip64_locator_truncated,
                    *classic_directory_end);
            }
        }
    }
    if (classic.offset < zip64_locator_size) {
        return invalid(CentralDirectoryErrorCode::zip64_locator_truncated, 0U);
    }
    const std::uint64_t locator_offset = classic.offset - zip64_locator_size;
    std::array<std::byte, zip64_locator_size> locator{};
    const auto locator_read = reader.read_at(locator_offset, locator);
    if (!locator_read) return file_failure(locator_read.error());
    if (read_u32(locator, 0U) != zip64_locator_signature) {
        return invalid(CentralDirectoryErrorCode::invalid_zip64_locator_signature,
                       locator_offset);
    }
    if (read_u32(locator, 4U) != 0U || read_u32(locator, 16U) != 1U) {
        return invalid(CentralDirectoryErrorCode::zip64_locator_multidisk,
                       read_u32(locator, 4U) != 0U ? locator_offset + 4U
                                                  : locator_offset + 16U);
    }

    const std::uint64_t zip64_offset = read_u64(locator, 8U);
    if (zip64_offset > locator_offset) {
        return invalid(CentralDirectoryErrorCode::zip64_locator_offset_out_of_bounds,
                       locator_offset + 8U);
    }
    if (locator_offset - zip64_offset < zip64_eocd_total_size) {
        return invalid(CentralDirectoryErrorCode::zip64_eocd_truncated,
                       zip64_offset);
    }
    const auto zip64_range = range_within_file(
        zip64_offset, zip64_eocd_total_size, reader.size());
    if (!zip64_range) {
        return invalid(CentralDirectoryErrorCode::zip64_eocd_out_of_bounds,
                       zip64_offset);
    }
    std::array<std::byte, zip64_eocd_total_size> zip64{};
    const auto zip64_read = reader.read_at(zip64_offset, zip64);
    if (!zip64_read) return file_failure(zip64_read.error());
    if (read_u32(zip64, 0U) != zip64_eocd_signature) {
        return invalid(CentralDirectoryErrorCode::invalid_zip64_eocd_signature,
                       zip64_offset);
    }
    if (read_u64(zip64, 4U) != zip64_eocd_declared_size) {
        return invalid(CentralDirectoryErrorCode::invalid_zip64_eocd_size,
                       zip64_offset + 4U);
    }
    if (read_u16(zip64, 14U) != 45U) {
        return invalid(CentralDirectoryErrorCode::invalid_zip64_version_needed,
                       zip64_offset + 14U);
    }
    if (zip64_range->end() != locator_offset) {
        return invalid(CentralDirectoryErrorCode::zip64_eocd_end_mismatch,
                       zip64_range->end());
    }

    const std::uint32_t disk_number = read_u32(zip64, 16U);
    const std::uint32_t directory_disk = read_u32(zip64, 20U);
    const std::uint64_t entries_on_disk = read_u64(zip64, 24U);
    const std::uint64_t total_entries = read_u64(zip64, 32U);
    const std::uint64_t directory_size = read_u64(zip64, 40U);
    const std::uint64_t directory_offset = read_u64(zip64, 48U);
    if (disk_number != 0U || directory_disk != 0U
        || entries_on_disk != total_entries) {
        return invalid(CentralDirectoryErrorCode::zip64_eocd_multidisk,
                       disk_number != 0U ? zip64_offset + 16U
                       : directory_disk != 0U ? zip64_offset + 20U
                                             : zip64_offset + 24U);
    }

    if (!compatible(classic.disk_number,
                    std::numeric_limits<std::uint16_t>::max(), disk_number)) {
        return invalid(CentralDirectoryErrorCode::classic_zip64_value_mismatch,
                       classic.offset + 4U);
    }
    if (!compatible(classic.central_directory_disk,
                    std::numeric_limits<std::uint16_t>::max(),
                    directory_disk)) {
        return invalid(CentralDirectoryErrorCode::classic_zip64_value_mismatch,
                       classic.offset + 6U);
    }
    if (!compatible(classic.entries_on_disk,
                    std::numeric_limits<std::uint16_t>::max(),
                    entries_on_disk)) {
        return invalid(CentralDirectoryErrorCode::classic_zip64_value_mismatch,
                       classic.offset + 8U);
    }
    if (!compatible(classic.total_entries,
                    std::numeric_limits<std::uint16_t>::max(),
                    total_entries)) {
        return invalid(CentralDirectoryErrorCode::classic_zip64_value_mismatch,
                       classic.offset + 10U);
    }
    if (!compatible(classic.central_directory_size,
                    std::numeric_limits<std::uint32_t>::max(),
                    directory_size)) {
        return invalid(CentralDirectoryErrorCode::classic_zip64_value_mismatch,
                       classic.offset + 12U);
    }
    if (!compatible(classic.central_directory_offset,
                    std::numeric_limits<std::uint32_t>::max(),
                    directory_offset)) {
        return invalid(CentralDirectoryErrorCode::classic_zip64_value_mismatch,
                       classic.offset + 16U);
    }

    ResolvedZipEndRecords records{reader.size(),
                                  directory_offset,
                                  directory_size,
                                  total_entries,
                                  classic.offset,
                                  classic.size,
                                  classic.comment_length,
                                  true,
                                  zip64_offset,
                                  locator_offset};
    if (auto failure = validate_directory(
            records, limits, zip64_offset + 40U, zip64_offset + 32U,
            zip64_offset + 48U, zip64_offset)) {
        return std::move(*failure);
    }
    return {ZipEndRecordStatus::structurally_valid,
            std::move(records),
            std::nullopt,
            std::nullopt,
            std::nullopt};
}

} // namespace

ZipEndRecordInspection inspect_zip_end_records_open(
    const FileReader& reader,
    const ClassicZipLimits limits)
{
    try {
        return inspect_open_file(reader, limits);
    } catch (const std::bad_alloc&) {
        return allocation_failure();
    }
}

ZipEndRecordInspection inspect_zip_end_records(
    const std::filesystem::path& path,
    const ClassicZipLimits limits)
{
    try {
        auto opened = FileReader::open(path);
        if (!opened) return file_failure(opened.error());
        return inspect_zip_end_records_open(opened.value(), limits);
    } catch (const std::bad_alloc&) {
        return allocation_failure();
    }
}

} // namespace aural::core::zip
