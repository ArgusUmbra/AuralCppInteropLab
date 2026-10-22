#include "classic_local_layout.hpp"

#include "classic_central_directory_internal.hpp"

#include <aural/core/range.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <numeric>
#include <span>
#include <utility>

namespace aural::core::zip {
namespace {

constexpr std::uint32_t local_header_signature = UINT32_C(0x04034b50);
constexpr std::uint64_t local_header_fixed_size = 30U;
constexpr std::uint16_t utf8_name_flag = UINT16_C(0x0800);
constexpr std::uint16_t data_descriptor_flag = UINT16_C(0x0008);

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

LocalLayoutInspection invalid(const LocalLayoutErrorCode code,
                              const std::uint64_t offset,
                              const std::optional<CentralDirectoryError> central =
                                  std::nullopt)
{
    return {LocalLayoutStatus::invalid_structure,
            std::nullopt,
            LocalLayoutError{code, offset},
            std::nullopt,
            std::nullopt,
            central,
            std::nullopt,
            std::nullopt,
            std::nullopt};
}

LocalLayoutInspection unsupported(const UnsupportedArchiveFeature feature,
                                  const std::uint64_t offset)
{
    return {LocalLayoutStatus::unsupported_archive_feature,
            std::nullopt,
            std::nullopt,
            UnsupportedFeatureError{feature, offset},
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt};
}

LocalLayoutInspection needs_zip64(const LocalZip64RequiredField field,
                                  const std::uint64_t offset)
{
    return {LocalLayoutStatus::zip64_support_required,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            LocalZip64Required{field, offset},
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt};
}

LocalLayoutInspection entry_resolution_pending(const std::uint64_t offset)
{
    auto result = needs_zip64(LocalZip64RequiredField::central_directory,
                              offset);
    result.status = LocalLayoutStatus::zip64_entry_resolution_pending;
    return result;
}

LocalLayoutInspection file_failure(const FileError error)
{
    LocalLayoutInspection result{};
    result.status = LocalLayoutStatus::file_failure;
    result.file_error = error;
    return result;
}

LocalLayoutInspection allocation_failure()
{
    LocalLayoutInspection result{};
    result.status = LocalLayoutStatus::resource_failure;
    result.resource = LocalLayoutResource::allocation_failure;
    return result;
}

bool overlaps(const std::uint64_t first_begin,
              const std::uint64_t first_end,
              const std::uint64_t second_begin,
              const std::uint64_t second_end) noexcept
{
    return first_begin < second_end && second_begin < first_end;
}

std::optional<LocalLayoutInspection> map_central_failure(
    const CentralDirectoryInspection& central)
{
    switch (central.status) {
    case CentralDirectoryStatus::structurally_valid:
        return std::nullopt;
    case CentralDirectoryStatus::invalid_structure:
        return invalid(LocalLayoutErrorCode::central_directory_invalid,
                       central.error->offset, central.error);
    case CentralDirectoryStatus::zip64_entry_resolution_pending:
        return entry_resolution_pending(central.zip64->offset);
    case CentralDirectoryStatus::resource_failure: {
        auto result = allocation_failure();
        result.central_resource = central.resource;
        return result;
    }
    case CentralDirectoryStatus::file_failure:
        return file_failure(*central.file_error);
    }
    return invalid(LocalLayoutErrorCode::central_directory_invalid, 0U);
}

std::optional<LocalLayoutInspection> compare_local_and_central(
    const std::span<const std::byte> fixed,
    const CentralDirectoryEntry& central,
    const std::uint64_t offset)
{
    if (read_u16(fixed, 4U) != central.version_needed) {
        return invalid(LocalLayoutErrorCode::version_needed_mismatch, offset + 4U);
    }
    if (read_u16(fixed, 6U) != central.flags) {
        return invalid(LocalLayoutErrorCode::flags_mismatch, offset + 6U);
    }
    if (read_u16(fixed, 8U) != central.compression_method) {
        return invalid(LocalLayoutErrorCode::compression_method_mismatch,
                       offset + 8U);
    }
    if (read_u16(fixed, 10U) != central.modification_time) {
        return invalid(LocalLayoutErrorCode::modification_time_mismatch,
                       offset + 10U);
    }
    if (read_u16(fixed, 12U) != central.modification_date) {
        return invalid(LocalLayoutErrorCode::modification_date_mismatch,
                       offset + 12U);
    }
    if (read_u32(fixed, 14U) != central.declared_crc32) {
        return invalid(LocalLayoutErrorCode::crc32_mismatch, offset + 14U);
    }
    if (read_u32(fixed, 18U) != central.declared_compressed_size) {
        return invalid(LocalLayoutErrorCode::compressed_size_mismatch,
                       offset + 18U);
    }
    if (read_u32(fixed, 22U) != central.declared_uncompressed_size) {
        return invalid(LocalLayoutErrorCode::uncompressed_size_mismatch,
                       offset + 22U);
    }
    return std::nullopt;
}

std::optional<LocalLayoutInspection> validate_profile_fields(
    const CentralDirectoryEntry& central,
    const std::uint64_t local_offset,
    const std::uint16_t local_extra_length)
{
    if ((central.flags & data_descriptor_flag) != 0U) {
        return unsupported(UnsupportedArchiveFeature::data_descriptor,
                           local_offset + 6U);
    }
    if ((central.flags & static_cast<std::uint16_t>(~utf8_name_flag)) != 0U) {
        return unsupported(UnsupportedArchiveFeature::general_purpose_flag,
                           local_offset + 6U);
    }
    if (central.compression_method != 0U) {
        return unsupported(UnsupportedArchiveFeature::compression_method,
                           local_offset + 8U);
    }
    if (central.version_needed != 10U && central.version_needed != 20U
        && central.version_needed != 45U) {
        return unsupported(UnsupportedArchiveFeature::version_needed,
                           local_offset + 4U);
    }
    if (central.compressed_size != central.uncompressed_size) {
        return invalid(LocalLayoutErrorCode::stored_size_mismatch,
                       central.header_offset + 20U);
    }
    if (local_extra_length != 0U) {
        return unsupported(UnsupportedArchiveFeature::local_extra_field,
                           local_offset + 28U);
    }
    if (central.extra_field_length != 0U) {
        return unsupported(UnsupportedArchiveFeature::central_extra_field,
                           central.header_offset + 30U);
    }
    if (central.comment_length != 0U) {
        return unsupported(UnsupportedArchiveFeature::entry_comment,
                           central.header_offset + 32U);
    }
    return std::nullopt;
}

LocalLayoutInspection inspect_open_file(const FileReader& reader,
                                        const ClassicZipLimits limits)
{
    auto central = inspect_classic_central_directory_open(reader, limits, true);
    if (auto failure = map_central_failure(central)) return std::move(*failure);
    const auto& directory = *central.directory;

    if (directory.archive_comment_length != 0U) {
        return unsupported(UnsupportedArchiveFeature::archive_comment,
                           directory.eocd_offset + 20U);
    }

    // Central ZIP64 values are resolved, but local ZIP64 extra fields are a
    // later milestone. Do not compare classic local fields against expanded
    // central values or claim a complete physical layout yet.
    for (const auto& entry : directory.entries) {
        if (entry.uses_zip64) {
            std::uint64_t offset = entry.header_offset + 24U;
            if (entry.declared_uncompressed_size != UINT32_MAX) {
                offset = entry.declared_compressed_size == UINT32_MAX
                    ? entry.header_offset + 20U
                    : entry.declared_local_header_offset == UINT32_MAX
                        ? entry.header_offset + 42U
                        : entry.header_offset + 34U;
            }
            return entry_resolution_pending(offset);
        }
    }

    std::vector<ClassicEntryPhysicalLayout> entries;
    entries.reserve(directory.entries.size());
    std::vector<std::size_t> central_order(directory.entries.size());
    std::iota(central_order.begin(), central_order.end(), std::size_t{0});
    std::sort(central_order.begin(), central_order.end(),
              [&directory](const std::size_t left, const std::size_t right) {
                  const auto& left_entry = directory.entries[left];
                  const auto& right_entry = directory.entries[right];
                  if (left_entry.local_header_offset
                      != right_entry.local_header_offset) {
                      return left_entry.local_header_offset
                          < right_entry.local_header_offset;
                  }
                  return left_entry.header_offset < right_entry.header_offset;
              });
    for (const auto central_index : central_order) {
        const auto& entry = directory.entries[central_index];
        const std::uint64_t offset = entry.local_header_offset;
        if (offset >= reader.size()) {
            return invalid(
                LocalLayoutErrorCode::local_header_offset_out_of_bounds,
                entry.header_offset + 42U);
        }
        if (offset >= directory.central_directory_offset) {
            return invalid(
                LocalLayoutErrorCode::entry_intersects_central_directory,
                offset);
        }
        if (directory.central_directory_offset - offset
            < local_header_fixed_size) {
            return invalid(LocalLayoutErrorCode::local_header_truncated,
                           offset);
        }
        const auto fixed_range =
            range_within_file(offset, local_header_fixed_size, reader.size());
        if (!fixed_range) {
            return invalid(LocalLayoutErrorCode::local_header_truncated, offset);
        }

        std::array<std::byte, local_header_fixed_size> fixed{};
        const auto fixed_read = reader.read_at(offset, fixed);
        if (!fixed_read) return file_failure(fixed_read.error());
        if (read_u32(fixed, 0U) != local_header_signature) {
            return invalid(LocalLayoutErrorCode::invalid_local_header_signature,
                           offset);
        }
        if (read_u32(fixed, 18U) == UINT32_MAX) {
            return needs_zip64(LocalZip64RequiredField::local_compressed_size,
                               offset + 18U);
        }
        if (read_u32(fixed, 22U) == UINT32_MAX) {
            return needs_zip64(LocalZip64RequiredField::local_uncompressed_size,
                               offset + 22U);
        }

        if (auto failure = compare_local_and_central(fixed, entry, offset)) {
            return std::move(*failure);
        }

        const std::uint16_t name_length = read_u16(fixed, 26U);
        const std::uint16_t extra_length = read_u16(fixed, 28U);
        const auto variable_length = checked_add(name_length, extra_length);
        const auto header_size = variable_length
            ? checked_add(local_header_fixed_size, *variable_length)
            : std::nullopt;
        const auto header_end = header_size
            ? checked_add(offset, *header_size)
            : std::nullopt;
        if (!header_size || !header_end) {
            return invalid(LocalLayoutErrorCode::local_record_length_overflow,
                           offset + 26U);
        }
        if (*header_end > reader.size()) {
            return invalid(
                LocalLayoutErrorCode::local_variable_fields_out_of_bounds,
                offset + 26U);
        }
        if (*header_end > directory.central_directory_offset) {
            return invalid(
                LocalLayoutErrorCode::entry_intersects_central_directory,
                offset + 26U);
        }

        std::vector<std::byte> variable(
            static_cast<std::size_t>(*variable_length));
        const auto variable_read =
            reader.read_at(offset + local_header_fixed_size, variable);
        if (!variable_read) return file_failure(variable_read.error());
        const auto name_end = variable.begin() + name_length;
        std::vector<std::byte> name(variable.begin(), name_end);
        if (name != entry.file_name) {
            return invalid(LocalLayoutErrorCode::filename_mismatch,
                           offset + local_header_fixed_size);
        }

        if (auto failure = validate_profile_fields(entry, offset, extra_length)) {
            return std::move(*failure);
        }

        const auto payload_end = checked_add(*header_end, entry.compressed_size);
        if (!payload_end) {
            return invalid(LocalLayoutErrorCode::payload_truncated, *header_end);
        }
        if (*payload_end > reader.size()) {
            return invalid(LocalLayoutErrorCode::payload_truncated, *header_end);
        }
        const auto eocd_end =
            checked_add(directory.eocd_offset, directory.eocd_size);
        if (!eocd_end) {
            return invalid(LocalLayoutErrorCode::entry_intersects_eocd,
                           directory.eocd_offset);
        }
        if (overlaps(*header_end, *payload_end, directory.eocd_offset,
                     *eocd_end)) {
            return invalid(LocalLayoutErrorCode::entry_intersects_eocd,
                           *header_end);
        }
        const auto directory_end = checked_add(
            directory.central_directory_offset,
            directory.central_directory_size);
        if (!directory_end) {
            return invalid(
                LocalLayoutErrorCode::entry_intersects_central_directory,
                directory.central_directory_offset);
        }
        if (overlaps(offset, *payload_end, directory.central_directory_offset,
                     *directory_end)) {
            return invalid(
                LocalLayoutErrorCode::entry_intersects_central_directory,
                *header_end);
        }

        std::vector<std::byte> extra(name_end, variable.end());
        entries.push_back(ClassicEntryPhysicalLayout{
            entry.header_offset,
            offset,
            *header_size,
            *header_end,
            entry.compressed_size,
            *payload_end,
            std::move(name),
            std::move(extra)});
    }

    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& left = entries[index];
        const std::uint64_t left_header_end = left.payload_offset;
        for (std::size_t other_index = index + 1U;
             other_index < entries.size(); ++other_index) {
            const auto& right = entries[other_index];
            const std::uint64_t right_header_end = right.payload_offset;
            if (left.local_header_offset == right.local_header_offset) {
                return invalid(
                    LocalLayoutErrorCode::duplicate_local_header_reference,
                    right.central_header_offset + 42U);
            }
            if (overlaps(left.local_header_offset, left_header_end,
                         right.local_header_offset, right_header_end)) {
                return invalid(LocalLayoutErrorCode::local_structures_overlap,
                               right.local_header_offset);
            }
            if (overlaps(left.payload_offset, left.payload_end,
                         right.payload_offset, right.payload_end)) {
                return invalid(LocalLayoutErrorCode::payloads_overlap,
                               right.payload_offset);
            }
            if (overlaps(left.payload_offset, left.payload_end,
                         right.local_header_offset, right_header_end)
                || overlaps(right.payload_offset, right.payload_end,
                            left.local_header_offset, left_header_end)) {
                return invalid(LocalLayoutErrorCode::header_payload_overlap,
                               right.local_header_offset);
            }
        }
    }

    if (!entries.empty()) {
        if (entries.front().local_header_offset != 0U) {
            return invalid(LocalLayoutErrorCode::archive_prefix_prohibited, 0U);
        }
        for (std::size_t index = 1U; index < entries.size(); ++index) {
            if (entries[index].local_header_offset
                != entries[index - 1U].payload_end) {
                return invalid(LocalLayoutErrorCode::archive_gap_prohibited,
                               entries[index - 1U].payload_end);
            }
        }
        if (entries.back().payload_end
            != directory.central_directory_offset) {
            return invalid(LocalLayoutErrorCode::archive_gap_prohibited,
                           entries.back().payload_end);
        }
    }

    ClassicArchivePhysicalLayout layout{
        directory.file_size,
        directory.central_directory_offset,
        directory.central_directory_size,
        directory.eocd_offset,
        directory.eocd_size,
        std::move(entries)};
    return {LocalLayoutStatus::profile_valid,
            std::move(layout),
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt};
}

} // namespace

LocalLayoutInspection inspect_classic_local_layout(
    const std::filesystem::path& path,
    const ClassicZipLimits limits)
{
    try {
        auto opened = FileReader::open(path);
        if (!opened) return file_failure(opened.error());
        return inspect_open_file(opened.value(), limits);
    } catch (const std::bad_alloc&) {
        return allocation_failure();
    }
}

std::string_view local_layout_error_message(
    const LocalLayoutErrorCode code) noexcept
{
    switch (code) {
    case LocalLayoutErrorCode::central_directory_invalid:
        return "central-directory inspection failed";
    case LocalLayoutErrorCode::local_header_offset_out_of_bounds:
        return "local-header offset is outside the archive";
    case LocalLayoutErrorCode::local_header_truncated:
        return "local file header is truncated";
    case LocalLayoutErrorCode::invalid_local_header_signature:
        return "local file header signature is invalid";
    case LocalLayoutErrorCode::local_record_length_overflow:
        return "local variable-field lengths overflow";
    case LocalLayoutErrorCode::local_variable_fields_out_of_bounds:
        return "local filename or extra field is truncated";
    case LocalLayoutErrorCode::version_needed_mismatch:
        return "local and central version-needed fields differ";
    case LocalLayoutErrorCode::flags_mismatch:
        return "local and central general-purpose flags differ";
    case LocalLayoutErrorCode::compression_method_mismatch:
        return "local and central compression methods differ";
    case LocalLayoutErrorCode::modification_time_mismatch:
        return "local and central modification times differ";
    case LocalLayoutErrorCode::modification_date_mismatch:
        return "local and central modification dates differ";
    case LocalLayoutErrorCode::crc32_mismatch:
        return "local and central CRC32 declarations differ";
    case LocalLayoutErrorCode::compressed_size_mismatch:
        return "local and central compressed sizes differ";
    case LocalLayoutErrorCode::uncompressed_size_mismatch:
        return "local and central uncompressed sizes differ";
    case LocalLayoutErrorCode::filename_mismatch:
        return "local and central filename bytes differ";
    case LocalLayoutErrorCode::stored_size_mismatch:
        return "stored compressed and uncompressed sizes differ";
    case LocalLayoutErrorCode::duplicate_local_header_reference:
        return "multiple central entries reference one local header";
    case LocalLayoutErrorCode::local_structures_overlap:
        return "local header structures overlap";
    case LocalLayoutErrorCode::header_payload_overlap:
        return "an entry payload overlaps a local header";
    case LocalLayoutErrorCode::payloads_overlap:
        return "entry payload ranges overlap";
    case LocalLayoutErrorCode::entry_intersects_central_directory:
        return "a local entry intersects the central directory";
    case LocalLayoutErrorCode::entry_intersects_eocd:
        return "a local entry intersects the EOCD";
    case LocalLayoutErrorCode::payload_truncated:
        return "declared payload extends beyond the archive";
    case LocalLayoutErrorCode::archive_prefix_prohibited:
        return "AURAL prohibits bytes before the first local header";
    case LocalLayoutErrorCode::archive_gap_prohibited:
        return "AURAL prohibits gaps between ZIP regions";
    }
    return "unknown local-layout error";
}

} // namespace aural::core::zip
