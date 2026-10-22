#include "payload_sha256.hpp"

#include "classic_local_layout_internal.hpp"

#include <aural/core/range.hpp>

#include <openssl/evp.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace aural::core::zip {
namespace {

using Buffer = std::array<std::byte,
                          static_cast<std::size_t>(sha256_stream_buffer_size)>;

static_assert(sha256_stream_buffer_size
              <= static_cast<std::uint64_t>(std::numeric_limits<uInt>::max()));

struct EvpContextDeleter final {
    void operator()(EVP_MD_CTX* context) const noexcept
    {
        EVP_MD_CTX_free(context);
    }
};

using EvpContext = std::unique_ptr<EVP_MD_CTX, EvpContextDeleter>;

[[nodiscard]] Sha256CalculationResult resource_failure() noexcept
{
    return {Sha256CalculationStatus::resource_failure,
            {}, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
            std::nullopt, Sha256Resource::allocation_failure, std::nullopt};
}

[[nodiscard]] Sha256CalculationResult prerequisite_failure(
    const Sha256IntegrityErrorCode code,
    const ClassicEntryPhysicalLayout* entry,
    const std::uint64_t failure_offset,
    const std::uint64_t remaining)
{
    Sha256IntegrityError error{
        code,
        entry == nullptr ? std::vector<std::byte>{} : entry->file_name,
        entry == nullptr ? 0U : entry->central_header_offset,
        entry == nullptr ? 0U : entry->payload_offset,
        failure_offset,
        remaining,
        entry == nullptr ? 0U : entry->declared_crc32,
        0U};
    return {Sha256CalculationStatus::invalid_prerequisite_structure,
            {}, std::move(error), std::nullopt, std::nullopt, std::nullopt,
            std::nullopt, std::nullopt, std::nullopt};
}

[[nodiscard]] Sha256CalculationResult integrity_failure(
    const Sha256CalculationStatus status,
    const Sha256IntegrityErrorCode code,
    const ClassicEntryPhysicalLayout& entry,
    const std::uint64_t failure_offset,
    const std::uint64_t remaining,
    const std::uint32_t calculated,
    const std::optional<FileError> file_error = std::nullopt)
{
    Sha256IntegrityError error{
        code,
        entry.file_name,
        entry.central_header_offset,
        entry.payload_offset,
        failure_offset,
        remaining,
        entry.declared_crc32,
        calculated};
    return {status, {}, std::move(error), std::nullopt, std::nullopt,
            std::nullopt, std::nullopt, std::nullopt, file_error};
}

[[nodiscard]] Sha256CalculationResult provider_failure(
    const Sha256ProviderErrorCode code,
    const ClassicEntryPhysicalLayout& entry,
    const std::uint64_t failure_offset)
{
    Sha256ProviderError error{
        code,
        entry.file_name,
        entry.central_header_offset,
        entry.payload_offset,
        failure_offset};
    return {Sha256CalculationStatus::cryptographic_provider_failure,
            {}, std::nullopt, std::move(error), std::nullopt, std::nullopt,
            std::nullopt, std::nullopt, std::nullopt};
}

[[nodiscard]] Sha256CalculationResult map_layout_failure(
    LocalLayoutInspection inspection)
{
    switch (inspection.status) {
    case LocalLayoutStatus::invalid_structure:
        return {Sha256CalculationStatus::invalid_prerequisite_structure,
                {}, std::nullopt, std::nullopt, inspection.error,
                inspection.central_error, inspection.unsupported, std::nullopt,
                inspection.file_error};
    case LocalLayoutStatus::unsupported_archive_feature:
        return {Sha256CalculationStatus::unsupported_operation,
                {}, std::nullopt, std::nullopt, inspection.error,
                inspection.central_error, inspection.unsupported, std::nullopt,
                inspection.file_error};
    case LocalLayoutStatus::resource_failure:
        return {Sha256CalculationStatus::resource_failure,
                {}, std::nullopt, std::nullopt, inspection.error,
                inspection.central_error, inspection.unsupported,
                Sha256Resource::allocation_failure, inspection.file_error};
    case LocalLayoutStatus::file_failure:
        return {Sha256CalculationStatus::payload_read_failure,
                {}, std::nullopt, std::nullopt, inspection.error,
                inspection.central_error, inspection.unsupported, std::nullopt,
                inspection.file_error};
    case LocalLayoutStatus::profile_valid:
        break;
    }
    return {Sha256CalculationStatus::invalid_prerequisite_structure,
            {}, std::nullopt, std::nullopt, inspection.error,
            inspection.central_error, inspection.unsupported, std::nullopt,
            inspection.file_error};
}

Sha256CalculationResult calculate_open_impl(
    const FileReader& reader,
    const ClassicArchivePhysicalLayout& layout)
{
    if (layout.file_size != reader.size()) {
        return prerequisite_failure(
            Sha256IntegrityErrorCode::layout_file_size_mismatch,
            nullptr, 0U, layout.file_size);
    }
    for (const auto& entry : layout.entries) {
        const auto end = checked_add(entry.payload_offset, entry.payload_size);
        if (!end) {
            return prerequisite_failure(
                Sha256IntegrityErrorCode::payload_range_overflow,
                &entry, entry.payload_offset, entry.payload_size);
        }
        if (*end != entry.payload_end) {
            return prerequisite_failure(
                Sha256IntegrityErrorCode::payload_end_mismatch,
                &entry, entry.payload_offset, entry.payload_size);
        }
        if (!range_within_file(
                entry.payload_offset, entry.payload_size, reader.size())) {
            return prerequisite_failure(
                Sha256IntegrityErrorCode::payload_range_out_of_bounds,
                &entry, entry.payload_offset, entry.payload_size);
        }
    }

    Buffer buffer{};
    std::vector<Sha256EntryResult> calculated_entries;
    calculated_entries.reserve(layout.entries.size());

    for (const auto& entry : layout.entries) {
        EvpContext context{EVP_MD_CTX_new()};
        if (!context) return resource_failure();
        if (EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
            return provider_failure(
                Sha256ProviderErrorCode::initialization_failed,
                entry, entry.payload_offset);
        }

        uLong crc = crc32(0L, Z_NULL, 0U);
        std::uint64_t position = entry.payload_offset;
        std::uint64_t remaining = entry.payload_size;
        while (remaining != 0U) {
            const auto chunk64 = std::min<std::uint64_t>(
                remaining, sha256_stream_buffer_size);
            const auto chunk = static_cast<std::size_t>(chunk64);
            const auto read = reader.read_at(
                position, std::span<std::byte>{buffer}.first(chunk));
            if (!read) {
                return integrity_failure(
                    Sha256CalculationStatus::payload_read_failure,
                    Sha256IntegrityErrorCode::file_read_failure,
                    entry, position, remaining,
                    static_cast<std::uint32_t>(crc), read.error());
            }
            if (read.value() != chunk) {
                return integrity_failure(
                    Sha256CalculationStatus::payload_read_failure,
                    Sha256IntegrityErrorCode::short_read,
                    entry, position, remaining,
                    static_cast<std::uint32_t>(crc));
            }
            crc = crc32(
                crc,
                reinterpret_cast<const Bytef*>(buffer.data()),
                static_cast<uInt>(chunk));
            if (EVP_DigestUpdate(context.get(), buffer.data(), chunk) != 1) {
                return provider_failure(
                    Sha256ProviderErrorCode::update_failed, entry, position);
            }
            const auto next = checked_add(position, chunk64);
            if (!next) {
                return prerequisite_failure(
                    Sha256IntegrityErrorCode::payload_range_overflow,
                    &entry, position, remaining);
            }
            position = *next;
            remaining -= chunk64;
        }

        const auto calculated_crc = static_cast<std::uint32_t>(crc);
        if (calculated_crc != entry.declared_crc32) {
            return integrity_failure(
                Sha256CalculationStatus::crc32_prerequisite_failure,
                Sha256IntegrityErrorCode::crc32_mismatch,
                entry, position, 0U, calculated_crc);
        }

        Sha256Digest digest{};
        unsigned int digest_size = 0U;
        if (EVP_DigestFinal_ex(
                context.get(),
                reinterpret_cast<unsigned char*>(digest.data()),
                &digest_size) != 1) {
            return provider_failure(
                Sha256ProviderErrorCode::finalization_failed, entry, position);
        }
        if (digest_size != sha256_digest_size) {
            return provider_failure(
                Sha256ProviderErrorCode::unexpected_digest_size,
                entry, position);
        }
        calculated_entries.push_back(Sha256EntryResult{
            entry.file_name,
            entry.central_header_offset,
            entry.payload_offset,
            entry.payload_size,
            entry.payload_end,
            entry.declared_crc32,
            calculated_crc,
            digest});
    }

    return {Sha256CalculationStatus::digest_calculated,
            std::move(calculated_entries), std::nullopt, std::nullopt,
            std::nullopt, std::nullopt, std::nullopt, std::nullopt,
            std::nullopt};
}

} // namespace

Sha256CalculationResult calculate_stored_payload_sha256_open(
    const FileReader& reader,
    const ClassicArchivePhysicalLayout& layout)
{
    try {
        return calculate_open_impl(reader, layout);
    } catch (const std::bad_alloc&) {
        return resource_failure();
    }
}

Sha256CalculationResult calculate_stored_payload_sha256(
    const std::filesystem::path& path,
    const ClassicZipLimits limits)
{
    try {
        auto opened = FileReader::open(path);
        if (!opened) {
            return {Sha256CalculationStatus::payload_read_failure,
                    {}, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                    std::nullopt, std::nullopt, opened.error()};
        }
        auto layout = inspect_classic_local_layout_open(opened.value(), limits);
        if (!layout.profile_valid()) {
            return map_layout_failure(std::move(layout));
        }
        return calculate_open_impl(opened.value(), *layout.layout);
    } catch (const std::bad_alloc&) {
        return resource_failure();
    }
}

std::string sha256_lowercase_hex(
    const std::span<const std::byte, sha256_digest_size> digest)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result(sha256_digest_size * 2U, '0');
    for (std::size_t index = 0; index < digest.size(); ++index) {
        const auto value = std::to_integer<unsigned int>(digest[index]);
        result[index * 2U] = digits[value >> 4U];
        result[index * 2U + 1U] = digits[value & 0x0fU];
    }
    return result;
}

std::string_view sha256_integrity_error_message(
    const Sha256IntegrityErrorCode code) noexcept
{
    switch (code) {
    case Sha256IntegrityErrorCode::crc32_mismatch:
        return "stored payload CRC32 prerequisite failed";
    case Sha256IntegrityErrorCode::layout_file_size_mismatch:
        return "validated layout file size does not match the open file";
    case Sha256IntegrityErrorCode::payload_range_overflow:
        return "payload offset plus length overflows the 64-bit range";
    case Sha256IntegrityErrorCode::payload_range_out_of_bounds:
        return "payload range is outside the open file";
    case Sha256IntegrityErrorCode::payload_end_mismatch:
        return "payload end is inconsistent with its offset and length";
    case Sha256IntegrityErrorCode::short_read:
        return "payload read returned fewer bytes than requested";
    case Sha256IntegrityErrorCode::file_read_failure:
        return "native payload read failed";
    }
    return "unknown SHA-256 payload integrity error";
}

std::string_view sha256_provider_error_message(
    const Sha256ProviderErrorCode code) noexcept
{
    switch (code) {
    case Sha256ProviderErrorCode::initialization_failed:
        return "SHA-256 provider initialization failed";
    case Sha256ProviderErrorCode::update_failed:
        return "SHA-256 provider update failed";
    case Sha256ProviderErrorCode::finalization_failed:
        return "SHA-256 provider finalization failed";
    case Sha256ProviderErrorCode::unexpected_digest_size:
        return "SHA-256 provider returned an unexpected digest size";
    }
    return "unknown SHA-256 provider error";
}

} // namespace aural::core::zip
