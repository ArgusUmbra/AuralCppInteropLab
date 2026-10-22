#ifndef AURAL_CORE_ZIP_PAYLOAD_CRC32_HPP
#define AURAL_CORE_ZIP_PAYLOAD_CRC32_HPP

#include "classic_local_layout.hpp"

#include <aural/core/file_reader.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace aural::core::zip {

inline constexpr std::uint64_t crc32_stream_buffer_size = UINT64_C(65536);

enum class Crc32VerificationStatus : std::uint8_t {
    crc32_verified,
    crc32_mismatch,
    invalid_prerequisite_structure,
    payload_read_failure,
    resource_failure,
    unsupported_operation,
};

enum class Crc32IntegrityErrorCode : std::uint8_t {
    mismatch,
    layout_file_size_mismatch,
    payload_range_overflow,
    payload_range_out_of_bounds,
    payload_end_mismatch,
    short_read,
    file_read_failure,
};

enum class Crc32Resource : std::uint8_t {
    allocation_failure,
};

struct Crc32EntryResult final {
    std::vector<std::byte> file_name;
    std::uint64_t central_header_offset;
    std::uint64_t payload_offset;
    std::uint64_t payload_size;
    std::uint64_t payload_end;
    std::uint32_t declared_crc32;
    std::uint32_t calculated_crc32;
};

struct Crc32IntegrityError final {
    Crc32IntegrityErrorCode code;
    std::vector<std::byte> file_name;
    std::uint64_t central_header_offset;
    std::uint64_t payload_offset;
    std::uint64_t failure_offset;
    std::uint64_t remaining_length;
    std::uint32_t declared_crc32;
    std::uint32_t calculated_crc32;
};

struct Crc32VerificationResult final {
    Crc32VerificationStatus status;
    // Populated only when every entry is verified.
    std::vector<Crc32EntryResult> entries;
    std::optional<Crc32IntegrityError> integrity_error;
    std::optional<LocalLayoutError> prerequisite_error;
    std::optional<CentralDirectoryError> central_error;
    std::optional<UnsupportedFeatureError> unsupported;
    std::optional<Crc32Resource> resource;
    std::optional<FileError> file_error;

    [[nodiscard]] bool verified() const noexcept
    {
        return status == Crc32VerificationStatus::crc32_verified;
    }
};

// Opens one read-only handle, validates the complete classic/ZIP64 physical
// layout, and then verifies every stored payload on that same handle.
// Success is payload CRC32 verification only, not AURAL package acceptance.
[[nodiscard]] Crc32VerificationResult verify_stored_payload_crc32(
    const std::filesystem::path& path,
    ClassicZipLimits limits = {});

// Internal entry point for later same-handle integrity stages and focused
// boundary testing. The supplied layout must come from structural inspection
// of this reader; it is defensively range-checked before any payload read.
[[nodiscard]] Crc32VerificationResult verify_stored_payload_crc32_open(
    const FileReader& reader,
    const ClassicArchivePhysicalLayout& layout);

[[nodiscard]] std::string_view crc32_integrity_error_message(
    Crc32IntegrityErrorCode code) noexcept;

} // namespace aural::core::zip

#endif
