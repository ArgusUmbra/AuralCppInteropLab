#ifndef AURAL_CORE_ZIP_PAYLOAD_SHA256_HPP
#define AURAL_CORE_ZIP_PAYLOAD_SHA256_HPP

#include "classic_local_layout.hpp"

#include <aural/core/file_reader.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aural::core::zip {

inline constexpr std::uint64_t sha256_stream_buffer_size = UINT64_C(65536);
inline constexpr std::size_t sha256_digest_size = 32U;
using Sha256Digest = std::array<std::byte, sha256_digest_size>;

enum class Sha256CalculationStatus : std::uint8_t {
    digest_calculated,
    invalid_prerequisite_structure,
    crc32_prerequisite_failure,
    payload_read_failure,
    resource_failure,
    cryptographic_provider_failure,
    unsupported_operation,
};

enum class Sha256IntegrityErrorCode : std::uint8_t {
    crc32_mismatch,
    layout_file_size_mismatch,
    payload_range_overflow,
    payload_range_out_of_bounds,
    payload_end_mismatch,
    short_read,
    file_read_failure,
};

enum class Sha256ProviderErrorCode : std::uint8_t {
    initialization_failed,
    update_failed,
    finalization_failed,
    unexpected_digest_size,
};

enum class Sha256Resource : std::uint8_t {
    allocation_failure,
};

struct Sha256EntryResult final {
    std::vector<std::byte> file_name;
    std::uint64_t central_header_offset;
    std::uint64_t payload_offset;
    std::uint64_t payload_size;
    std::uint64_t payload_end;
    std::uint32_t declared_crc32;
    std::uint32_t calculated_crc32;
    Sha256Digest digest;
};

struct Sha256IntegrityError final {
    Sha256IntegrityErrorCode code;
    std::vector<std::byte> file_name;
    std::uint64_t central_header_offset;
    std::uint64_t payload_offset;
    std::uint64_t failure_offset;
    std::uint64_t remaining_length;
    std::uint32_t declared_crc32;
    std::uint32_t calculated_crc32;
};

struct Sha256ProviderError final {
    Sha256ProviderErrorCode code;
    std::vector<std::byte> file_name;
    std::uint64_t central_header_offset;
    std::uint64_t payload_offset;
    std::uint64_t failure_offset;
};

struct Sha256CalculationResult final {
    Sha256CalculationStatus status;
    // Populated only when every entry's CRC32 prerequisite and SHA-256
    // calculation succeed.
    std::vector<Sha256EntryResult> entries;
    std::optional<Sha256IntegrityError> integrity_error;
    std::optional<Sha256ProviderError> provider_error;
    std::optional<LocalLayoutError> prerequisite_error;
    std::optional<CentralDirectoryError> central_error;
    std::optional<UnsupportedFeatureError> unsupported;
    std::optional<Sha256Resource> resource;
    std::optional<FileError> file_error;

    [[nodiscard]] bool calculated() const noexcept
    {
        return status == Sha256CalculationStatus::digest_calculated;
    }
};

// Opens one read-only handle, validates the complete classic/ZIP64 physical
// layout, verifies each stored payload's ZIP CRC32, and calculates SHA-256 on
// that same handle. Success is calculation only, not manifest correlation or
// AURAL package acceptance.
[[nodiscard]] Sha256CalculationResult calculate_stored_payload_sha256(
    const std::filesystem::path& path,
    ClassicZipLimits limits = {});

// Internal same-handle entry point. The supplied layout must originate from
// this reader and is defensively range-checked before payload reads.
[[nodiscard]] Sha256CalculationResult calculate_stored_payload_sha256_open(
    const FileReader& reader,
    const ClassicArchivePhysicalLayout& layout);

[[nodiscard]] std::string sha256_lowercase_hex(
    std::span<const std::byte, sha256_digest_size> digest);

[[nodiscard]] std::string_view sha256_integrity_error_message(
    Sha256IntegrityErrorCode code) noexcept;

[[nodiscard]] std::string_view sha256_provider_error_message(
    Sha256ProviderErrorCode code) noexcept;

} // namespace aural::core::zip

#endif
