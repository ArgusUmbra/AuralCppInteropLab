#include "payload_crc32.hpp"

#include "classic_local_layout_internal.hpp"

#include <aural/core/range.hpp>

#include <zlib.h>

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <span>
#include <utility>

namespace aural::core::zip {
namespace {

using Buffer = std::array<std::byte,
                          static_cast<std::size_t>(crc32_stream_buffer_size)>;

static_assert(crc32_stream_buffer_size
              <= static_cast<std::uint64_t>(std::numeric_limits<uInt>::max()));

[[nodiscard]] Crc32VerificationResult resource_failure() noexcept
{
    return {Crc32VerificationStatus::resource_failure,
            {},
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            Crc32Resource::allocation_failure,
            std::nullopt};
}

[[nodiscard]] Crc32VerificationResult prerequisite_failure(
    const Crc32IntegrityErrorCode code,
    const ClassicEntryPhysicalLayout* entry,
    const std::uint64_t failure_offset,
    const std::uint64_t remaining)
{
    Crc32IntegrityError error{
        code,
        entry == nullptr ? std::vector<std::byte>{} : entry->file_name,
        entry == nullptr ? 0U : entry->central_header_offset,
        entry == nullptr ? 0U : entry->payload_offset,
        failure_offset,
        remaining,
        entry == nullptr ? 0U : entry->declared_crc32,
        0U};
    return {Crc32VerificationStatus::invalid_prerequisite_structure,
            {},
            std::move(error),
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt};
}

[[nodiscard]] Crc32VerificationResult entry_failure(
    const Crc32VerificationStatus status,
    const Crc32IntegrityErrorCode code,
    const ClassicEntryPhysicalLayout& entry,
    const std::uint64_t failure_offset,
    const std::uint64_t remaining,
    const std::uint32_t calculated,
    const std::optional<FileError> file_error = std::nullopt)
{
    Crc32IntegrityError error{
        code,
        entry.file_name,
        entry.central_header_offset,
        entry.payload_offset,
        failure_offset,
        remaining,
        entry.declared_crc32,
        calculated};
    return {status,
            {},
            std::move(error),
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            file_error};
}

[[nodiscard]] Crc32VerificationResult map_layout_failure(
    LocalLayoutInspection inspection)
{
    switch (inspection.status) {
    case LocalLayoutStatus::invalid_structure:
        return {Crc32VerificationStatus::invalid_prerequisite_structure,
                {}, std::nullopt, inspection.error, inspection.central_error,
                inspection.unsupported, std::nullopt, inspection.file_error};
    case LocalLayoutStatus::unsupported_archive_feature:
        return {Crc32VerificationStatus::unsupported_operation,
                {}, std::nullopt, inspection.error, inspection.central_error,
                inspection.unsupported, std::nullopt, inspection.file_error};
    case LocalLayoutStatus::resource_failure:
        return {Crc32VerificationStatus::resource_failure,
                {}, std::nullopt, inspection.error, inspection.central_error,
                inspection.unsupported, Crc32Resource::allocation_failure,
                inspection.file_error};
    case LocalLayoutStatus::file_failure:
        return {Crc32VerificationStatus::payload_read_failure,
                {}, std::nullopt, inspection.error, inspection.central_error,
                inspection.unsupported, std::nullopt, inspection.file_error};
    case LocalLayoutStatus::profile_valid:
        return {Crc32VerificationStatus::invalid_prerequisite_structure,
                {}, std::nullopt, inspection.error, inspection.central_error,
                inspection.unsupported, std::nullopt, inspection.file_error};
    }
    return {Crc32VerificationStatus::invalid_prerequisite_structure,
            {}, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
            std::nullopt, std::nullopt};
}

Crc32VerificationResult verify_open_impl(
    const FileReader& reader,
    const ClassicArchivePhysicalLayout& layout)
{
    if (layout.file_size != reader.size()) {
        return prerequisite_failure(
            Crc32IntegrityErrorCode::layout_file_size_mismatch,
            nullptr,
            0U,
            layout.file_size);
    }

    for (const auto& entry : layout.entries) {
        const auto end = checked_add(entry.payload_offset, entry.payload_size);
        if (!end) {
            return prerequisite_failure(
                Crc32IntegrityErrorCode::payload_range_overflow,
                &entry,
                entry.payload_offset,
                entry.payload_size);
        }
        if (*end != entry.payload_end) {
            return prerequisite_failure(
                Crc32IntegrityErrorCode::payload_end_mismatch,
                &entry,
                entry.payload_offset,
                entry.payload_size);
        }
        if (!range_within_file(
                entry.payload_offset, entry.payload_size, reader.size())) {
            return prerequisite_failure(
                Crc32IntegrityErrorCode::payload_range_out_of_bounds,
                &entry,
                entry.payload_offset,
                entry.payload_size);
        }
    }

    Buffer buffer{};
    std::vector<Crc32EntryResult> verified_entries;
    verified_entries.reserve(layout.entries.size());

    for (const auto& entry : layout.entries) {
        uLong crc = crc32(0L, Z_NULL, 0U);
        std::uint64_t position = entry.payload_offset;
        std::uint64_t remaining = entry.payload_size;
        while (remaining != 0U) {
            const auto chunk64 = std::min<std::uint64_t>(
                remaining, crc32_stream_buffer_size);
            const auto chunk = static_cast<std::size_t>(chunk64);
            const auto read = reader.read_at(
                position, std::span<std::byte>{buffer}.first(chunk));
            if (!read) {
                return entry_failure(
                    Crc32VerificationStatus::payload_read_failure,
                    Crc32IntegrityErrorCode::file_read_failure,
                    entry,
                    position,
                    remaining,
                    static_cast<std::uint32_t>(crc),
                    read.error());
            }
            if (read.value() != chunk) {
                return entry_failure(
                    Crc32VerificationStatus::payload_read_failure,
                    Crc32IntegrityErrorCode::short_read,
                    entry,
                    position,
                    remaining,
                    static_cast<std::uint32_t>(crc));
            }
            crc = crc32(
                crc,
                reinterpret_cast<const Bytef*>(buffer.data()),
                static_cast<uInt>(chunk));
            const auto next = checked_add(position, chunk64);
            if (!next) {
                return prerequisite_failure(
                    Crc32IntegrityErrorCode::payload_range_overflow,
                    &entry,
                    position,
                    remaining);
            }
            position = *next;
            remaining -= chunk64;
        }

        const auto calculated = static_cast<std::uint32_t>(crc);
        if (calculated != entry.declared_crc32) {
            return entry_failure(
                Crc32VerificationStatus::crc32_mismatch,
                Crc32IntegrityErrorCode::mismatch,
                entry,
                position,
                0U,
                calculated);
        }
        verified_entries.push_back(Crc32EntryResult{
            entry.file_name,
            entry.central_header_offset,
            entry.payload_offset,
            entry.payload_size,
            entry.payload_end,
            entry.declared_crc32,
            calculated});
    }

    return {Crc32VerificationStatus::crc32_verified,
            std::move(verified_entries),
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt};
}

} // namespace

Crc32VerificationResult verify_stored_payload_crc32_open(
    const FileReader& reader,
    const ClassicArchivePhysicalLayout& layout)
{
    try {
        return verify_open_impl(reader, layout);
    } catch (const std::bad_alloc&) {
        return resource_failure();
    }
}

Crc32VerificationResult verify_stored_payload_crc32(
    const std::filesystem::path& path,
    const ClassicZipLimits limits)
{
    try {
        auto opened = FileReader::open(path);
        if (!opened) {
            return {Crc32VerificationStatus::payload_read_failure,
                    {},
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    opened.error()};
        }
        auto layout = inspect_classic_local_layout_open(opened.value(), limits);
        if (!layout.profile_valid()) {
            return map_layout_failure(std::move(layout));
        }
        return verify_open_impl(opened.value(), *layout.layout);
    } catch (const std::bad_alloc&) {
        return resource_failure();
    }
}

std::string_view crc32_integrity_error_message(
    const Crc32IntegrityErrorCode code) noexcept
{
    switch (code) {
    case Crc32IntegrityErrorCode::mismatch:
        return "stored payload CRC32 does not match its ZIP declaration";
    case Crc32IntegrityErrorCode::layout_file_size_mismatch:
        return "validated layout file size does not match the open file";
    case Crc32IntegrityErrorCode::payload_range_overflow:
        return "payload offset plus length overflows the 64-bit range";
    case Crc32IntegrityErrorCode::payload_range_out_of_bounds:
        return "payload range is outside the open file";
    case Crc32IntegrityErrorCode::payload_end_mismatch:
        return "payload end is inconsistent with its offset and length";
    case Crc32IntegrityErrorCode::short_read:
        return "payload read returned fewer bytes than requested";
    case Crc32IntegrityErrorCode::file_read_failure:
        return "native payload read failed";
    }
    return "unknown CRC32 integrity error";
}

} // namespace aural::core::zip
