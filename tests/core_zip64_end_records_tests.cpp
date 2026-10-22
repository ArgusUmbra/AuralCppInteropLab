#include "../src/core/zip/classic_central_directory.hpp"
#include "../src/core/zip/zip64_end_records.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using aural::core::zip::CentralDirectoryErrorCode;
using aural::core::zip::CentralDirectoryStatus;
using aural::core::zip::ZipEndRecordInspection;
using aural::core::zip::ZipEndRecordStatus;
using aural::core::zip::inspect_classic_central_directory;
using aural::core::zip::inspect_zip_end_records;

[[noreturn]] void fail(const std::string_view message)
{
    std::cerr << "FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

void require(const bool condition, const std::string_view message)
{
    if (!condition) fail(message);
}

class TemporaryDirectory final {
public:
    TemporaryDirectory()
    {
        const auto seed = std::chrono::steady_clock::now()
                              .time_since_epoch()
                              .count();
        for (unsigned int attempt = 0; attempt < 100U; ++attempt) {
            path_ = std::filesystem::temp_directory_path()
                / ("aural-zip64-tests-" + std::to_string(seed) + "-"
                   + std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) return;
        }
        throw std::runtime_error{"could not create ZIP64 test directory"};
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void append_u16(std::vector<std::byte>& output, const std::uint16_t value)
{
    output.push_back(static_cast<std::byte>(value & 0xffU));
    output.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
}

void append_u32(std::vector<std::byte>& output, const std::uint32_t value)
{
    append_u16(output, static_cast<std::uint16_t>(value & 0xffffU));
    append_u16(output, static_cast<std::uint16_t>(value >> 16U));
}

void append_u64(std::vector<std::byte>& output, const std::uint64_t value)
{
    append_u32(output, static_cast<std::uint32_t>(value & UINT32_MAX));
    append_u32(output, static_cast<std::uint32_t>(value >> 32U));
}

void set_u16(std::vector<std::byte>& output,
             const std::size_t offset,
             const std::uint16_t value)
{
    output[offset] = static_cast<std::byte>(value & 0xffU);
    output[offset + 1U] = static_cast<std::byte>((value >> 8U) & 0xffU);
}

void set_u32(std::vector<std::byte>& output,
             const std::size_t offset,
             const std::uint32_t value)
{
    set_u16(output, offset, static_cast<std::uint16_t>(value & 0xffffU));
    set_u16(output, offset + 2U, static_cast<std::uint16_t>(value >> 16U));
}

void set_u64(std::vector<std::byte>& output,
             const std::size_t offset,
             const std::uint64_t value)
{
    set_u32(output, offset, static_cast<std::uint32_t>(value & UINT32_MAX));
    set_u32(output, offset + 4U, static_cast<std::uint32_t>(value >> 32U));
}

struct ArchiveOffsets final {
    std::size_t central;
    std::size_t zip64;
    std::size_t locator;
    std::size_t eocd;
};

std::vector<std::byte> make_classic_empty()
{
    std::vector<std::byte> archive;
    append_u32(archive, UINT32_C(0x06054b50));
    for (unsigned int index = 0; index < 8U; ++index) append_u16(archive, 0U);
    append_u16(archive, 0U);
    return archive;
}

std::vector<std::byte> make_zip64_one(ArchiveOffsets& offsets)
{
    std::vector<std::byte> archive(30U);
    set_u32(archive, 0U, UINT32_C(0x04034b50));
    offsets.central = archive.size();
    append_u32(archive, UINT32_C(0x02014b50));
    append_u16(archive, 45U);
    append_u16(archive, 45U);
    for (unsigned int index = 0; index < 4U; ++index) append_u16(archive, 0U);
    append_u32(archive, 0U);
    append_u32(archive, 0U);
    append_u32(archive, 0U);
    append_u16(archive, 0U);
    append_u16(archive, 0U);
    append_u16(archive, 0U);
    append_u16(archive, 0U);
    append_u16(archive, 0U);
    append_u32(archive, 0U);
    append_u32(archive, 0U);
    require(archive.size() == offsets.central + 46U,
            "central test record has the APPNOTE fixed size");

    offsets.zip64 = archive.size();
    append_u32(archive, UINT32_C(0x06064b50));
    append_u64(archive, 44U);
    append_u16(archive, 45U);
    append_u16(archive, 45U);
    append_u32(archive, 0U);
    append_u32(archive, 0U);
    append_u64(archive, 1U);
    append_u64(archive, 1U);
    append_u64(archive, 46U);
    append_u64(archive, offsets.central);

    offsets.locator = archive.size();
    append_u32(archive, UINT32_C(0x07064b50));
    append_u32(archive, 0U);
    append_u64(archive, offsets.zip64);
    append_u32(archive, 1U);

    offsets.eocd = archive.size();
    append_u32(archive, UINT32_C(0x06054b50));
    append_u16(archive, 0U);
    append_u16(archive, 0U);
    append_u16(archive, UINT16_MAX);
    append_u16(archive, UINT16_MAX);
    append_u32(archive, 46U);
    append_u32(archive, static_cast<std::uint32_t>(offsets.central));
    append_u16(archive, 0U);
    return archive;
}

void write_bytes(const std::filesystem::path& path,
                 const std::span<const std::byte> bytes)
{
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    require(static_cast<bool>(output), "create ZIP64 test archive");
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(output), "write ZIP64 test archive");
}

std::vector<std::byte> read_bytes(const std::filesystem::path& path)
{
    std::ifstream input{path, std::ios::binary};
    std::vector<std::byte> result;
    char value = 0;
    while (input.get(value)) {
        result.push_back(
            static_cast<std::byte>(static_cast<unsigned char>(value)));
    }
    return result;
}

ZipEndRecordInspection inspect_bytes(const TemporaryDirectory& temporary,
                                     const std::string_view name,
                                     const std::span<const std::byte> bytes)
{
    const auto path = temporary.path() / name;
    write_bytes(path, bytes);
    const auto before = read_bytes(path);
    const auto result = inspect_zip_end_records(path);
    const auto repeated = inspect_zip_end_records(path);
    require(before == read_bytes(path), "inspection must not modify input");
    require(result.status == repeated.status,
            "repeated ZIP64 inspection status is deterministic");
    require(result.error.has_value() == repeated.error.has_value(),
            "repeated ZIP64 error shape is deterministic");
    if (result.error) {
        require(result.error->code == repeated.error->code
                    && result.error->offset == repeated.error->offset,
                "repeated ZIP64 error detail is deterministic");
    }
    return result;
}

void require_error(const ZipEndRecordInspection& result,
                   const CentralDirectoryErrorCode code,
                   const std::uint64_t offset,
                   const std::string_view context)
{
    require(result.status == ZipEndRecordStatus::invalid_structure,
            std::string{context} + ": invalid status");
    require(result.error && result.error->code == code,
            std::string{context} + ": wrong error code");
    require(result.error->offset == offset,
            std::string{context} + ": wrong error offset");
}

void test_valid_and_integration(const TemporaryDirectory& temporary)
{
    const auto classic = inspect_bytes(temporary, "classic.zip",
                                       make_classic_empty());
    require(classic.structurally_valid() && !classic.records->uses_zip64,
            "classic EOCD behavior is preserved");

    ArchiveOffsets offsets{};
    const auto archive = make_zip64_one(offsets);
    const auto result = inspect_bytes(temporary, "valid.zip64", archive);
    require(result.structurally_valid() && result.records->uses_zip64,
            "valid ZIP64 end-record chain is recognized");
    require(result.records->central_directory_offset == offsets.central
                && result.records->central_directory_size == 46U
                && result.records->entry_count == 1U
                && result.records->zip64_eocd_offset == offsets.zip64
                && result.records->zip64_locator_offset == offsets.locator,
            "resolved 64-bit metadata and record locations are retained");

    const auto path = temporary.path() / "valid.zip64";
    const auto central = inspect_classic_central_directory(path);
    require(central.structurally_valid()
                && central.directory->uses_zip64
                && central.directory->entries.size() == 1U,
            "central parser consumes resolved ZIP64 EOCD metadata");
}

void test_locator_failures(const TemporaryDirectory& temporary)
{
    auto missing = make_classic_empty();
    set_u16(missing, 10U, UINT16_MAX);
    require_error(inspect_bytes(temporary, "missing-locator.zip", missing),
                  CentralDirectoryErrorCode::zip64_locator_missing, 0U,
                  "missing locator");

    std::vector<std::byte> truncated(10U);
    const auto eocd = make_classic_empty();
    truncated.insert(truncated.end(), eocd.begin(), eocd.end());
    set_u16(truncated, 10U + 10U, UINT16_MAX);
    require_error(inspect_bytes(temporary, "truncated-locator.zip", truncated),
                  CentralDirectoryErrorCode::zip64_locator_truncated, 0U,
                  "truncated locator");

    ArchiveOffsets offsets{};
    const auto valid = make_zip64_one(offsets);
    auto bad_signature = valid;
    set_u32(bad_signature, offsets.locator, 0U);
    require_error(inspect_bytes(temporary, "bad-locator-signature.zip",
                                bad_signature),
                  CentralDirectoryErrorCode::invalid_zip64_locator_signature,
                  offsets.locator, "invalid locator signature");

    auto impossible = valid;
    set_u64(impossible, offsets.locator + 8U, UINT64_MAX);
    require_error(inspect_bytes(temporary, "impossible-offset.zip", impossible),
                  CentralDirectoryErrorCode::zip64_locator_offset_out_of_bounds,
                  offsets.locator + 8U, "impossible ZIP64 EOCD offset");

    auto disk = valid;
    set_u32(disk, offsets.locator + 4U, 1U);
    require_error(inspect_bytes(temporary, "locator-disk.zip", disk),
                  CentralDirectoryErrorCode::zip64_locator_multidisk,
                  offsets.locator + 4U, "locator disk number");

    auto disks = valid;
    set_u32(disks, offsets.locator + 16U, 2U);
    require_error(inspect_bytes(temporary, "locator-disks.zip", disks),
                  CentralDirectoryErrorCode::zip64_locator_multidisk,
                  offsets.locator + 16U, "locator disk count");

    auto zero_disks = valid;
    set_u32(zero_disks, offsets.locator + 16U, 0U);
    require_error(inspect_bytes(temporary, "locator-zero-disks.zip",
                                zero_disks),
                  CentralDirectoryErrorCode::zip64_locator_multidisk,
                  offsets.locator + 16U, "zero locator disk count");
}

void test_zip64_record_failures(const TemporaryDirectory& temporary)
{
    ArchiveOffsets offsets{};
    const auto valid = make_zip64_one(offsets);

    auto signature = valid;
    set_u32(signature, offsets.zip64, 0U);
    require_error(inspect_bytes(temporary, "bad-zip64-signature.zip", signature),
                  CentralDirectoryErrorCode::invalid_zip64_eocd_signature,
                  offsets.zip64, "invalid ZIP64 EOCD signature");

    auto size = valid;
    set_u64(size, offsets.zip64 + 4U, 43U);
    require_error(inspect_bytes(temporary, "bad-zip64-size.zip", size),
                  CentralDirectoryErrorCode::invalid_zip64_eocd_size,
                  offsets.zip64 + 4U, "incorrect ZIP64 EOCD size");

    auto truncated = valid;
    set_u64(truncated, offsets.locator + 8U, offsets.locator - 10U);
    require_error(inspect_bytes(temporary, "truncated-zip64.zip", truncated),
                  CentralDirectoryErrorCode::zip64_eocd_truncated,
                  offsets.locator - 10U, "truncated ZIP64 EOCD");

    auto version = valid;
    set_u16(version, offsets.zip64 + 14U, 20U);
    require_error(inspect_bytes(temporary, "zip64-version.zip", version),
                  CentralDirectoryErrorCode::invalid_zip64_version_needed,
                  offsets.zip64 + 14U, "insufficient ZIP64 version-needed");

    auto gap = valid;
    gap.insert(gap.begin() + static_cast<std::ptrdiff_t>(offsets.locator),
               std::byte{0});
    const std::size_t shifted_locator = offsets.locator + 1U;
    const std::size_t shifted_eocd = offsets.eocd + 1U;
    set_u64(gap, shifted_locator + 8U, offsets.zip64);
    require_error(inspect_bytes(temporary, "zip64-gap.zip", gap),
                  CentralDirectoryErrorCode::zip64_eocd_end_mismatch,
                  offsets.locator, "gap between ZIP64 EOCD and locator");
    require(shifted_eocd + 22U == gap.size(), "mutated EOCD remains terminal");

    auto disk = valid;
    set_u32(disk, offsets.zip64 + 16U, 1U);
    require_error(inspect_bytes(temporary, "zip64-disk.zip", disk),
                  CentralDirectoryErrorCode::zip64_eocd_multidisk,
                  offsets.zip64 + 16U, "ZIP64 EOCD disk number");

    auto counts = valid;
    set_u64(counts, offsets.zip64 + 24U, 2U);
    require_error(inspect_bytes(temporary, "zip64-disk-count.zip", counts),
                  CentralDirectoryErrorCode::zip64_eocd_multidisk,
                  offsets.zip64 + 24U, "ZIP64 per-disk count");
}

void test_resolved_values_and_limits(const TemporaryDirectory& temporary)
{
    ArchiveOffsets offsets{};
    const auto valid = make_zip64_one(offsets);

    auto contradiction = valid;
    set_u16(contradiction, offsets.eocd + 10U, 2U);
    require_error(inspect_bytes(temporary, "contradict-count.zip", contradiction),
                  CentralDirectoryErrorCode::classic_zip64_value_mismatch,
                  offsets.eocd + 10U, "classic and ZIP64 contradiction");

    auto outside = valid;
    set_u32(outside, offsets.eocd + 16U, UINT32_MAX);
    set_u64(outside, offsets.zip64 + 48U, UINT64_MAX);
    require_error(inspect_bytes(temporary, "directory-outside.zip", outside),
                  CentralDirectoryErrorCode::central_directory_out_of_bounds,
                  offsets.zip64 + 48U, "directory beyond EOF");

    auto oversized = valid;
    set_u32(oversized, offsets.eocd + 12U, UINT32_MAX);
    set_u64(oversized, offsets.zip64 + 40U, UINT64_MAX);
    require_error(inspect_bytes(temporary, "directory-oversized.zip", oversized),
                  CentralDirectoryErrorCode::central_directory_size_limit_exceeded,
                  offsets.zip64 + 40U, "maximum directory declaration");

    auto excessive_entries = valid;
    set_u64(excessive_entries, offsets.zip64 + 24U, UINT64_MAX);
    set_u64(excessive_entries, offsets.zip64 + 32U, UINT64_MAX);
    require_error(inspect_bytes(temporary, "entries-excessive.zip",
                                excessive_entries),
                  CentralDirectoryErrorCode::entry_count_limit_exceeded,
                  offsets.zip64 + 32U, "maximum entry-count declaration");

    auto entry_pending = valid;
    set_u32(entry_pending, offsets.central + 20U, UINT32_MAX);
    const auto path = temporary.path() / "entry-pending.zip";
    write_bytes(path, entry_pending);
    const auto pending = inspect_classic_central_directory(path);
    require(pending.status == CentralDirectoryStatus::invalid_structure
                && pending.error
                && pending.error->code
                    == CentralDirectoryErrorCode::missing_zip64_extra_field
                && pending.error->offset == offsets.central + 20U,
            "entry-level ZIP64 sentinel requires its own extra field");

    ArchiveOffsets empty_offsets{};
    auto empty_zip64 = make_zip64_one(empty_offsets);
    set_u64(empty_zip64, empty_offsets.zip64 + 24U, 0U);
    set_u64(empty_zip64, empty_offsets.zip64 + 32U, 0U);
    set_u64(empty_zip64, empty_offsets.zip64 + 40U, 0U);
    set_u64(empty_zip64, empty_offsets.zip64 + 48U, empty_offsets.zip64);
    set_u32(empty_zip64, empty_offsets.eocd + 12U, UINT32_MAX);
    set_u32(empty_zip64, empty_offsets.eocd + 16U, UINT32_MAX);
    empty_zip64.erase(empty_zip64.begin(),
                      empty_zip64.begin()
                          + static_cast<std::ptrdiff_t>(empty_offsets.zip64));
    // Erasing the local/directory prefix shifts the ZIP64 record to zero.
    set_u64(empty_zip64, 48U, 0U);
    set_u64(empty_zip64, 56U + 8U, 0U);
    const auto zero = inspect_bytes(temporary, "zero-values.zip", empty_zip64);
    require(zero.structurally_valid()
                && zero.records->central_directory_size == 0U
                && zero.records->entry_count == 0U,
            "zero-valued ZIP64 directory boundaries are valid structurally");
}

void test_false_signatures(const TemporaryDirectory& temporary)
{
    auto classic = make_classic_empty();
    const std::vector<std::byte> comment{
        std::byte{0x50}, std::byte{0x4b}, std::byte{0x06}, std::byte{0x06},
        std::byte{0x50}, std::byte{0x4b}, std::byte{0x06}, std::byte{0x07}};
    set_u16(classic, 20U, static_cast<std::uint16_t>(comment.size()));
    classic.insert(classic.end(), comment.begin(), comment.end());
    const auto result = inspect_bytes(temporary, "false-signatures.zip", classic);
    require(result.structurally_valid() && !result.records->uses_zip64,
            "unrelated ZIP64 signatures do not trigger ZIP64 parsing");
}

} // namespace

int main()
{
    try {
        const TemporaryDirectory temporary;
        test_valid_and_integration(temporary);
        test_locator_failures(temporary);
        test_zip64_record_failures(temporary);
        test_resolved_values_and_limits(temporary);
        test_false_signatures(temporary);
        std::cout << "CORE_ZIP64_END_RECORDS_TESTS_OK\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: unexpected exception: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
