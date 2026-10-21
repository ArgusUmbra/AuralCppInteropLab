#include "classic_central_directory.hpp"

#include <aural/core/range.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <span>
#include <utility>

namespace aural::core::zip {
namespace {

constexpr std::uint32_t eocd_signature = UINT32_C(0x06054b50);
constexpr std::uint32_t central_header_signature = UINT32_C(0x02014b50);
constexpr std::uint64_t eocd_fixed_size = 22;
constexpr std::uint64_t maximum_zip_comment_size = 65535;
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

CentralDirectoryInspection needs_zip64(const Zip64RequiredField field,
                                       const std::uint64_t offset)
{
    return {CentralDirectoryStatus::zip64_support_required,
            std::nullopt,
            std::nullopt,
            Zip64Required{field, offset},
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

struct Eocd final {
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
    std::optional<Eocd> eocd;
    std::optional<CentralDirectoryInspection> failure;
};

EocdSearch find_eocd(const FileReader& reader)
{
    const std::uint64_t file_size = reader.size();
    if (file_size < eocd_fixed_size) {
        return {std::nullopt,
                invalid(CentralDirectoryErrorCode::archive_too_small,
                        file_size)};
    }

    const std::uint64_t maximum_search =
        eocd_fixed_size + maximum_zip_comment_size;
    const std::uint64_t search_size = std::min(file_size, maximum_search);
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
                    return {Eocd{absolute,
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

std::optional<CentralDirectoryInspection> validate_eocd_fields(
    const Eocd& eocd,
    const ClassicZipLimits& limits)
{
    if (eocd.disk_number == UINT16_MAX) {
        return needs_zip64(Zip64RequiredField::eocd_disk_number,
                           eocd.offset + 4U);
    }
    if (eocd.central_directory_disk == UINT16_MAX) {
        return needs_zip64(Zip64RequiredField::eocd_central_directory_disk,
                           eocd.offset + 6U);
    }
    if (eocd.entries_on_disk == UINT16_MAX) {
        return needs_zip64(Zip64RequiredField::eocd_entries_on_disk,
                           eocd.offset + 8U);
    }
    if (eocd.total_entries == UINT16_MAX) {
        return needs_zip64(Zip64RequiredField::eocd_total_entries,
                           eocd.offset + 10U);
    }
    if (eocd.central_directory_size == UINT32_MAX) {
        return needs_zip64(Zip64RequiredField::eocd_central_directory_size,
                           eocd.offset + 12U);
    }
    if (eocd.central_directory_offset == UINT32_MAX) {
        return needs_zip64(Zip64RequiredField::eocd_central_directory_offset,
                           eocd.offset + 16U);
    }
    if (eocd.disk_number != 0U || eocd.central_directory_disk != 0U
        || eocd.entries_on_disk != eocd.total_entries) {
        return invalid(CentralDirectoryErrorCode::multidisk_unsupported,
                       eocd.offset + 4U);
    }
    if (eocd.central_directory_size > limits.maximum_central_directory_size) {
        return invalid(
            CentralDirectoryErrorCode::central_directory_size_limit_exceeded,
            eocd.offset + 12U);
    }
    if (eocd.total_entries > limits.maximum_entry_count) {
        return invalid(CentralDirectoryErrorCode::entry_count_limit_exceeded,
                       eocd.offset + 10U);
    }
    return std::nullopt;
}

std::optional<CentralDirectoryInspection> check_entry_sentinels(
    const std::span<const std::byte> bytes,
    const std::size_t cursor,
    const std::uint64_t absolute)
{
    if (read_u32(bytes, cursor + 20U) == UINT32_MAX) {
        return needs_zip64(Zip64RequiredField::entry_compressed_size,
                           absolute + 20U);
    }
    if (read_u32(bytes, cursor + 24U) == UINT32_MAX) {
        return needs_zip64(Zip64RequiredField::entry_uncompressed_size,
                           absolute + 24U);
    }
    if (read_u16(bytes, cursor + 34U) == UINT16_MAX) {
        return needs_zip64(Zip64RequiredField::entry_disk_number,
                           absolute + 34U);
    }
    if (read_u32(bytes, cursor + 42U) == UINT32_MAX) {
        return needs_zip64(Zip64RequiredField::entry_local_header_offset,
                           absolute + 42U);
    }
    return std::nullopt;
}

CentralDirectoryInspection inspect_open_file(FileReader reader,
                                             const ClassicZipLimits& limits)
{
    const std::uint64_t file_size = reader.size();
    if (file_size > limits.maximum_archive_size) {
        return invalid(CentralDirectoryErrorCode::archive_size_limit_exceeded,
                       limits.maximum_archive_size);
    }

    auto searched = find_eocd(reader);
    if (!searched.eocd) return std::move(*searched.failure);
    const Eocd eocd = *searched.eocd;
    if (auto failure = validate_eocd_fields(eocd, limits)) {
        return std::move(*failure);
    }

    const std::uint64_t directory_offset = eocd.central_directory_offset;
    const std::uint64_t directory_size = eocd.central_directory_size;
    const auto directory_range =
        range_within_file(directory_offset, directory_size, file_size);
    if (!directory_range) {
        return invalid(CentralDirectoryErrorCode::central_directory_out_of_bounds,
                       eocd.offset + 16U);
    }
    if (directory_range->overlaps(
            *ByteRange::from_bounds(eocd.offset, eocd.offset + eocd.size))) {
        return invalid(CentralDirectoryErrorCode::central_directory_overlaps_eocd,
                       directory_offset);
    }
    if (directory_range->end() != eocd.offset) {
        return invalid(CentralDirectoryErrorCode::central_directory_end_mismatch,
                       directory_range->end());
    }

    std::vector<std::byte> bytes(static_cast<std::size_t>(directory_size));
    const auto read = reader.read_at(directory_offset, bytes);
    if (!read) return file_failure(read.error());

    std::vector<CentralDirectoryEntry> entries;
    entries.reserve(eocd.total_entries);
    std::size_t cursor = 0;
    while (cursor < bytes.size()) {
        const std::uint64_t absolute = directory_offset + cursor;
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
        if (auto failure = check_entry_sentinels(bytes, cursor, absolute)) {
            return std::move(*failure);
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

        const std::uint16_t disk_start = read_u16(bytes, cursor + 34U);
        if (disk_start != 0U) {
            return invalid(CentralDirectoryErrorCode::multidisk_unsupported,
                           absolute + 34U);
        }
        const std::uint64_t local_offset = read_u32(bytes, cursor + 42U);
        const auto local_fixed_end = checked_add(local_offset,
                                                 local_header_fixed_size);
        if (!local_fixed_end) {
            return invalid(
                CentralDirectoryErrorCode::local_header_offset_out_of_bounds,
                absolute + 42U);
        }
        if (local_offset >= directory_offset || *local_fixed_end > directory_offset) {
            return invalid(
                CentralDirectoryErrorCode::local_header_range_overlaps_directory,
                absolute + 42U);
        }
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
            read_u16(bytes, cursor + 36U),
            read_u32(bytes, cursor + 38U),
            local_offset,
            {},
            extra_length,
            comment_length};
        const auto name_begin = bytes.begin()
            + static_cast<std::ptrdiff_t>(cursor + central_header_fixed_size);
        entry.file_name.assign(name_begin, name_begin + name_length);
        entries.push_back(std::move(entry));
        cursor += static_cast<std::size_t>(*record_length);
    }

    if (entries.size() != eocd.total_entries) {
        return invalid(CentralDirectoryErrorCode::entry_count_mismatch,
                       eocd.offset + 10U);
    }

    ClassicCentralDirectory directory{
        file_size,
        directory_offset,
        directory_size,
        eocd.offset,
        eocd.size,
        eocd.comment_length,
        std::move(entries)};
    return {CentralDirectoryStatus::structurally_valid,
            std::move(directory),
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt};
}

} // namespace

CentralDirectoryInspection inspect_classic_central_directory(
    const std::filesystem::path& path,
    const ClassicZipLimits limits)
{
    try {
        auto opened = FileReader::open(path);
        if (!opened) return file_failure(opened.error());
        return inspect_open_file(std::move(opened).value(), limits);
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
