#include "../src/core/zip/classic_central_directory.hpp"

#include <algorithm>
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
using aural::core::zip::CentralDirectoryInspection;
using aural::core::zip::CentralDirectoryStatus;
using aural::core::zip::ClassicZipLimits;
using aural::core::zip::Zip64RequiredField;
using aural::core::zip::inspect_classic_central_directory;

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
                / ("aural-zip-tests-" + std::to_string(seed) + "-"
                   + std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) return;
        }
        throw std::runtime_error{"could not create ZIP test directory"};
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

std::vector<std::byte> byte_string(const std::initializer_list<unsigned int> values)
{
    std::vector<std::byte> result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

struct EntrySpec final {
    std::vector<std::byte> name;
    std::uint16_t flags = 0;
    std::uint16_t method = 0;
    std::uint32_t crc32 = 0;
    std::uint32_t compressed_size = 0;
    std::uint32_t uncompressed_size = 0;
    std::uint32_t local_offset = 0;
    std::vector<std::byte> extra;
    std::vector<std::byte> comment;
};

std::vector<std::byte> make_archive(
    const std::vector<EntrySpec>& specs,
    const std::vector<std::byte>& archive_comment = {})
{
    std::vector<std::byte> archive;
    const std::size_t local_area_size = specs.size() * 30U;
    archive.resize(local_area_size);
    for (std::size_t index = 0; index < specs.size(); ++index) {
        set_u32(archive, index * 30U, UINT32_C(0x04034b50));
    }
    const auto central_offset = static_cast<std::uint32_t>(archive.size());
    for (const auto& spec : specs) {
        append_u32(archive, UINT32_C(0x02014b50));
        append_u16(archive, 63U);
        append_u16(archive, 20U);
        append_u16(archive, spec.flags);
        append_u16(archive, spec.method);
        append_u16(archive, UINT16_C(0x1234));
        append_u16(archive, UINT16_C(0x5678));
        append_u32(archive, spec.crc32);
        append_u32(archive, spec.compressed_size);
        append_u32(archive, spec.uncompressed_size);
        append_u16(archive, static_cast<std::uint16_t>(spec.name.size()));
        append_u16(archive, static_cast<std::uint16_t>(spec.extra.size()));
        append_u16(archive, static_cast<std::uint16_t>(spec.comment.size()));
        append_u16(archive, 0U);
        append_u16(archive, UINT16_C(0x4321));
        append_u32(archive, UINT32_C(0x81a40000));
        append_u32(archive, spec.local_offset);
        archive.insert(archive.end(), spec.name.begin(), spec.name.end());
        archive.insert(archive.end(), spec.extra.begin(), spec.extra.end());
        archive.insert(archive.end(), spec.comment.begin(), spec.comment.end());
    }
    const auto central_size =
        static_cast<std::uint32_t>(archive.size() - central_offset);
    append_u32(archive, UINT32_C(0x06054b50));
    append_u16(archive, 0U);
    append_u16(archive, 0U);
    append_u16(archive, static_cast<std::uint16_t>(specs.size()));
    append_u16(archive, static_cast<std::uint16_t>(specs.size()));
    append_u32(archive, central_size);
    append_u32(archive, central_offset);
    append_u16(archive, static_cast<std::uint16_t>(archive_comment.size()));
    archive.insert(archive.end(), archive_comment.begin(), archive_comment.end());
    return archive;
}

void write_bytes(const std::filesystem::path& path,
                 const std::span<const std::byte> bytes)
{
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    require(static_cast<bool>(output), "create test archive");
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(output), "write test archive");
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

CentralDirectoryInspection inspect_bytes(
    const TemporaryDirectory& temporary,
    const std::string_view name,
    const std::span<const std::byte> bytes,
    const ClassicZipLimits limits = {})
{
    const auto path = temporary.path() / name;
    write_bytes(path, bytes);
    const auto before = read_bytes(path);
    const auto result = inspect_classic_central_directory(path, limits);
    const auto repeated = inspect_classic_central_directory(path, limits);
    const auto after = read_bytes(path);
    require(before == after, "inspection must not modify input bytes");
    require(result.status == repeated.status,
            "repeated inspection must have deterministic status");
    require(result.error.has_value() == repeated.error.has_value(),
            "repeated inspection must have deterministic error shape");
    if (result.error) {
        require(result.error->code == repeated.error->code
                    && result.error->offset == repeated.error->offset,
                "repeated inspection must have deterministic error detail");
    }
    require(result.zip64.has_value() == repeated.zip64.has_value(),
            "repeated inspection must have deterministic ZIP64 shape");
    if (result.zip64) {
        require(result.zip64->field == repeated.zip64->field
                    && result.zip64->offset == repeated.zip64->offset,
                "repeated inspection must have deterministic ZIP64 detail");
    }
    return result;
}

void require_error(const CentralDirectoryInspection& result,
                   const CentralDirectoryErrorCode code,
                   const std::uint64_t offset,
                   const std::string_view context)
{
    require(result.status == CentralDirectoryStatus::invalid_structure,
            std::string{context} + ": invalid status");
    require(result.error.has_value() && result.error->code == code,
            std::string{context} + ": wrong error code");
    require(result.error->offset == offset,
            std::string{context} + ": wrong error offset");
    require(!aural::core::zip::central_directory_error_message(code).empty(),
            "all structure errors have messages");
}

void test_valid_structures(const TemporaryDirectory& temporary)
{
    const auto empty = make_archive({});
    const auto empty_result = inspect_bytes(temporary, "empty.zip", empty);
    require(empty_result.structurally_valid(), "empty classic directory is valid");
    require(empty_result.directory->entries.empty()
                && empty_result.directory->central_directory_offset == 0U
                && empty_result.directory->eocd_offset == 0U,
            "empty directory offsets are retained");

    EntrySpec entry;
    entry.name = byte_string({0x61U, 0x00U, 0xffU, 0x62U});
    entry.flags = UINT16_C(0x0800);
    entry.method = 8U;
    entry.crc32 = UINT32_C(0xdeadbeef);
    entry.compressed_size = 7U;
    entry.uncompressed_size = 11U;
    entry.extra = byte_string({1U, 2U, 3U});
    entry.comment = byte_string({4U, 5U});
    const auto ordinary = make_archive({entry});
    const auto result = inspect_bytes(temporary, "ordinary.zip", ordinary);
    require(result.structurally_valid(),
            "ordinary non-AURAL metadata remains structurally inspectable");
    const auto& parsed = result.directory->entries.front();
    require(parsed.file_name == entry.name, "filename bytes are preserved exactly");
    require(parsed.flags == entry.flags && parsed.compression_method == 8U
                && parsed.declared_crc32 == entry.crc32
                && parsed.declared_compressed_size == 7U
                && parsed.declared_uncompressed_size == 11U
                && parsed.local_header_offset == 0U
                && parsed.extra_field_length == 3U
                && parsed.comment_length == 2U,
            "central metadata is retained without package-validity claims");

    const auto false_signature_comment = byte_string(
        {0x50U, 0x4bU, 0x05U, 0x06U, 0xffU, 0xffU, 0xffU});
    const auto with_false_signature = make_archive({}, false_signature_comment);
    const auto false_result = inspect_bytes(
        temporary, "false-signature.zip", with_false_signature);
    require(false_result.structurally_valid()
                && false_result.directory->archive_comment_length
                    == false_signature_comment.size(),
            "false EOCD signature inside the terminal comment is ignored");
}

void test_eocd_failures(const TemporaryDirectory& temporary)
{
    const std::vector<std::byte> none;
    require_error(inspect_bytes(temporary, "zero.bin", none),
                  CentralDirectoryErrorCode::archive_too_small, 0U,
                  "zero length");

    auto truncated = std::vector<std::byte>(22U);
    set_u32(truncated, 18U, UINT32_C(0x06054b50));
    require_error(inspect_bytes(temporary, "truncated-eocd.zip", truncated),
                  CentralDirectoryErrorCode::eocd_record_truncated, 18U,
                  "truncated EOCD fixed record");

    auto missing = std::vector<std::byte>(22U);
    require_error(inspect_bytes(temporary, "missing-eocd.zip", missing),
                  CentralDirectoryErrorCode::eocd_not_found, 0U,
                  "missing EOCD signature");

    auto trailer = make_archive({});
    trailer.push_back(std::byte{0x7f});
    require_error(inspect_bytes(temporary, "trailer.zip", trailer),
                  CentralDirectoryErrorCode::eocd_not_terminal, 0U,
                  "trailing byte");

    auto bad_comment = make_archive({});
    set_u16(bad_comment, 20U, 1U);
    require_error(inspect_bytes(temporary, "bad-comment.zip", bad_comment),
                  CentralDirectoryErrorCode::eocd_record_truncated, 0U,
                  "truncated EOCD comment");
}

void test_eocd_fields_and_limits(const TemporaryDirectory& temporary)
{
    auto multidisk = make_archive({});
    set_u16(multidisk, 4U, 1U);
    require_error(inspect_bytes(temporary, "multidisk.zip", multidisk),
                  CentralDirectoryErrorCode::multidisk_unsupported, 4U,
                  "multidisk declaration");

    auto zip64 = make_archive({});
    set_u16(zip64, 10U, UINT16_MAX);
    const auto zip64_result = inspect_bytes(temporary, "zip64-eocd.zip", zip64);
    require(zip64_result.status == CentralDirectoryStatus::zip64_support_required
                && zip64_result.zip64->field
                    == Zip64RequiredField::eocd_total_entries
                && zip64_result.zip64->offset == 10U,
            "EOCD sentinel produces distinct ZIP64-required outcome");

    const auto empty = make_archive({});
    auto small_archive_limit = ClassicZipLimits{};
    small_archive_limit.maximum_archive_size = empty.size() - 1U;
    require_error(inspect_bytes(temporary, "archive-limit.zip", empty,
                                small_archive_limit),
                  CentralDirectoryErrorCode::archive_size_limit_exceeded,
                  empty.size() - 1U, "physical archive limit");

    EntrySpec entry;
    entry.name = byte_string({0x61U});
    const auto one = make_archive({entry});
    const std::uint64_t eocd = one.size() - 22U;
    auto small_directory_limit = ClassicZipLimits{};
    small_directory_limit.maximum_central_directory_size = 1U;
    require_error(inspect_bytes(temporary, "directory-limit.zip", one,
                                small_directory_limit),
                  CentralDirectoryErrorCode::central_directory_size_limit_exceeded,
                  eocd + 12U, "central directory byte limit");

    auto zero_entries = ClassicZipLimits{};
    zero_entries.maximum_entry_count = 0U;
    require_error(inspect_bytes(temporary, "entry-limit.zip", one, zero_entries),
                  CentralDirectoryErrorCode::entry_count_limit_exceeded,
                  eocd + 10U, "entry count limit before allocation");
}

void test_directory_ranges(const TemporaryDirectory& temporary)
{
    EntrySpec entry;
    entry.name = byte_string({0x61U});
    auto archive = make_archive({entry});
    const std::size_t eocd = archive.size() - 22U;

    auto outside = archive;
    set_u32(outside, eocd + 16U, UINT32_C(0xfffffffe));
    require_error(inspect_bytes(temporary, "directory-outside.zip", outside),
                  CentralDirectoryErrorCode::central_directory_out_of_bounds,
                  eocd + 16U, "impossible central directory offset");

    auto overlap = archive;
    set_u32(overlap, eocd + 16U, static_cast<std::uint32_t>(eocd - 1U));
    set_u32(overlap, eocd + 12U, 2U);
    require_error(inspect_bytes(temporary, "directory-overlap.zip", overlap),
                  CentralDirectoryErrorCode::central_directory_overlaps_eocd,
                  eocd - 1U, "directory EOCD overlap");

    auto gap = archive;
    set_u32(gap, eocd + 12U,
            static_cast<std::uint32_t>(eocd - 31U));
    require_error(inspect_bytes(temporary, "directory-gap.zip", gap),
                  CentralDirectoryErrorCode::central_directory_end_mismatch,
                  eocd - 1U, "directory end mismatch");

    auto short_header = archive;
    set_u32(short_header, eocd + 12U, 4U);
    set_u32(short_header, eocd + 16U,
            static_cast<std::uint32_t>(eocd - 4U));
    require_error(inspect_bytes(temporary, "short-header.zip", short_header),
                  CentralDirectoryErrorCode::central_header_truncated,
                  eocd - 4U, "truncated central fixed header");

    auto wrong_signature = archive;
    wrong_signature[30U] = std::byte{0};
    require_error(inspect_bytes(temporary, "wrong-signature.zip", wrong_signature),
                  CentralDirectoryErrorCode::invalid_central_header_signature,
                  30U, "central signature");

    auto huge_name = archive;
    set_u16(huge_name, 30U + 28U, UINT16_MAX);
    require_error(inspect_bytes(temporary, "huge-name.zip", huge_name),
                  CentralDirectoryErrorCode::central_entry_out_of_bounds,
                  30U + 28U, "unchecked oversized name declaration");

    auto local_overlap = archive;
    set_u32(local_overlap, 30U + 42U, 29U);
    require_error(inspect_bytes(temporary, "local-overlap.zip", local_overlap),
                  CentralDirectoryErrorCode::local_header_range_overlaps_directory,
                  30U + 42U, "local fixed range overlaps directory");
}

void test_counts_and_entry_zip64(const TemporaryDirectory& temporary)
{
    EntrySpec entry;
    entry.name = byte_string({0x61U});
    auto archive = make_archive({entry});
    const std::size_t eocd = archive.size() - 22U;

    auto too_many_declared = archive;
    set_u16(too_many_declared, eocd + 8U, 2U);
    set_u16(too_many_declared, eocd + 10U, 2U);
    require_error(inspect_bytes(temporary, "count-high.zip", too_many_declared),
                  CentralDirectoryErrorCode::entry_count_mismatch,
                  eocd + 10U, "declared count exceeds actual records");

    auto too_few_declared = archive;
    set_u16(too_few_declared, eocd + 8U, 0U);
    set_u16(too_few_declared, eocd + 10U, 0U);
    require_error(inspect_bytes(temporary, "count-low.zip", too_few_declared),
                  CentralDirectoryErrorCode::entry_count_mismatch,
                  eocd + 10U, "declared count below actual records");

    auto zip64_entry = archive;
    set_u32(zip64_entry, 30U + 20U, UINT32_MAX);
    const auto result = inspect_bytes(
        temporary, "zip64-entry.zip", zip64_entry);
    require(result.status == CentralDirectoryStatus::zip64_support_required
                && result.zip64->field
                    == Zip64RequiredField::entry_compressed_size
                && result.zip64->offset == 50U,
            "entry sentinel produces distinct ZIP64-required outcome");

    EntrySpec first;
    first.name = byte_string({0x61U});
    EntrySpec second;
    second.name = byte_string({0x62U});
    second.local_offset = 15U;
    const auto overlapping_locals = make_archive({first, second});
    require_error(inspect_bytes(temporary, "local-ranges-overlap.zip",
                                overlapping_locals),
                  CentralDirectoryErrorCode::local_header_ranges_overlap,
                  149U, "declared local fixed ranges overlap");
}

void test_frozen_examples(const std::filesystem::path& fixture_directory)
{
    const auto valid = inspect_classic_central_directory(
        fixture_directory / "valid-independent.aural");
    require(valid.structurally_valid() && valid.directory->entries.size() == 2U,
            "frozen independent classic package has valid central structure");

    const auto compression = inspect_classic_central_directory(
        fixture_directory / "invalid-compression.aural");
    require(compression.structurally_valid()
                && std::any_of(compression.directory->entries.begin(),
                               compression.directory->entries.end(),
                               [](const auto& entry) {
                                   return entry.compression_method != 0U;
                               }),
            "compression policy remains outside structural inspection");

    const auto trailer = inspect_classic_central_directory(
        fixture_directory / "invalid-trailer.aural");
    require(trailer.status == CentralDirectoryStatus::invalid_structure
                && trailer.error->code
                    == CentralDirectoryErrorCode::eocd_not_terminal,
            "frozen trailer fixture is structurally rejected");
}

} // namespace

int main(const int argc, char** argv)
{
    require(argc == 2, "fixture directory argument is required");
    const TemporaryDirectory temporary;
    const auto missing = inspect_classic_central_directory(
        temporary.path() / "does-not-exist.zip");
    require(missing.status == CentralDirectoryStatus::file_failure
                && missing.file_error.has_value() && !missing.directory,
            "open failures remain structured file failures");
    test_valid_structures(temporary);
    test_eocd_failures(temporary);
    test_eocd_fields_and_limits(temporary);
    test_directory_ranges(temporary);
    test_counts_and_entry_zip64(temporary);
    test_frozen_examples(argv[1]);
    std::cout << "CORE_ZIP_CENTRAL_DIRECTORY_TESTS_OK\n";
    return EXIT_SUCCESS;
}
