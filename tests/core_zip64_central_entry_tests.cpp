#include "../src/core/zip/classic_central_directory.hpp"
#include "../src/core/zip/classic_local_layout.hpp"

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
using aural::core::zip::LocalLayoutStatus;
using aural::core::zip::inspect_classic_central_directory;
using aural::core::zip::inspect_classic_local_layout;

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
                / ("aural-zip64-central-" + std::to_string(seed) + "-"
                   + std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) return;
        }
        throw std::runtime_error{"could not create test directory"};
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

std::vector<std::byte> zip64_extra(const std::vector<std::uint64_t>& values,
                                   const bool disk_value = false,
                                   const std::uint16_t id = 1U)
{
    std::vector<std::byte> result;
    append_u16(result, id);
    const std::size_t width = disk_value ? 4U : 8U;
    append_u16(result,
               static_cast<std::uint16_t>(values.size() * width));
    for (const auto value : values) {
        if (disk_value) {
            append_u32(result, static_cast<std::uint32_t>(value));
        } else {
            append_u64(result, value);
        }
    }
    return result;
}

struct EntrySpec final {
    std::uint32_t compressed = 7U;
    std::uint32_t uncompressed = 11U;
    std::uint16_t disk_start = 0U;
    std::uint32_t local_offset = 0U;
    std::vector<std::byte> extra;
};

std::vector<std::byte> make_archive(const EntrySpec& spec)
{
    std::vector<std::byte> archive(30U);
    archive[0] = std::byte{0x50};
    archive[1] = std::byte{0x4b};
    archive[2] = std::byte{0x03};
    archive[3] = std::byte{0x04};
    const std::uint32_t central_offset = 30U;
    append_u32(archive, UINT32_C(0x02014b50));
    append_u16(archive, 45U);
    append_u16(archive, 45U);
    append_u16(archive, 0U);
    append_u16(archive, 0U);
    append_u16(archive, 0U);
    append_u16(archive, 0U);
    append_u32(archive, 0U);
    append_u32(archive, spec.compressed);
    append_u32(archive, spec.uncompressed);
    append_u16(archive, 1U);
    append_u16(archive, static_cast<std::uint16_t>(spec.extra.size()));
    append_u16(archive, 0U);
    append_u16(archive, spec.disk_start);
    append_u16(archive, 0U);
    append_u32(archive, 0U);
    append_u32(archive, spec.local_offset);
    archive.push_back(std::byte{'a'});
    archive.insert(archive.end(), spec.extra.begin(), spec.extra.end());
    const auto central_size =
        static_cast<std::uint32_t>(archive.size() - central_offset);
    append_u32(archive, UINT32_C(0x06054b50));
    append_u16(archive, 0U);
    append_u16(archive, 0U);
    append_u16(archive, 1U);
    append_u16(archive, 1U);
    append_u32(archive, central_size);
    append_u32(archive, central_offset);
    append_u16(archive, 0U);
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

CentralDirectoryInspection inspect(const TemporaryDirectory& temporary,
                                   const std::string_view name,
                                   const std::vector<std::byte>& bytes)
{
    const auto path = temporary.path() / name;
    write_bytes(path, bytes);
    const auto before = read_bytes(path);
    const auto first = inspect_classic_central_directory(path);
    const auto second = inspect_classic_central_directory(path);
    require(read_bytes(path) == before, "parser must not modify input");
    require(first.status == second.status,
            "repeated parse must have deterministic status");
    require(first.error.has_value() == second.error.has_value(),
            "repeated parse must have deterministic error shape");
    if (first.error) {
        require(first.error->code == second.error->code
                    && first.error->offset == second.error->offset,
                "repeated parse must have deterministic error detail");
    }
    return first;
}

void require_error(const CentralDirectoryInspection& result,
                   const CentralDirectoryErrorCode code,
                   const std::uint64_t offset,
                   const std::string_view context)
{
    require(result.status == CentralDirectoryStatus::invalid_structure,
            std::string{context} + ": wrong status");
    require(result.error && result.error->code == code,
            std::string{context} + ": wrong error code");
    require(result.error->offset == offset,
            std::string{context} + ": wrong error offset");
}

void test_successful_resolution(const TemporaryDirectory& temporary)
{
    const auto classic = inspect(temporary, "classic.zip", make_archive({}));
    require(classic.structurally_valid(), "classic entry remains valid");
    const auto& classic_entry = classic.directory->entries.front();
    require(!classic_entry.uses_zip64 && classic_entry.compressed_size == 7U
                && classic_entry.uncompressed_size == 11U
                && classic_entry.local_header_offset == 0U
                && classic_entry.disk_start == 0U,
            "classic values resolve without ambiguity");

    EntrySpec size_entry;
    size_entry.compressed = UINT32_MAX;
    size_entry.uncompressed = UINT32_MAX;
    size_entry.extra = zip64_extra({UINT64_C(0x100000002),
                                    UINT64_C(0x100000001)});
    const auto sizes = inspect(temporary, "sizes.zip", make_archive(size_entry));
    require(sizes.structurally_valid(), "ZIP64 sizes resolve");
    const auto& resolved_sizes = sizes.directory->entries.front();
    require(resolved_sizes.uses_zip64
                && resolved_sizes.declared_compressed_size == UINT32_MAX
                && resolved_sizes.declared_uncompressed_size == UINT32_MAX
                && resolved_sizes.uncompressed_size == UINT64_C(0x100000002)
                && resolved_sizes.compressed_size == UINT64_C(0x100000001),
            "ZIP64 size values follow APPNOTE order");

    EntrySpec offset_entry;
    offset_entry.local_offset = UINT32_MAX;
    offset_entry.extra = zip64_extra({0U});
    const auto offset = inspect(temporary, "offset.zip",
                                make_archive(offset_entry));
    require(offset.structurally_valid()
                && offset.directory->entries.front().local_header_offset == 0U
                && offset.directory->entries.front().declared_local_header_offset
                    == UINT32_MAX,
            "ZIP64 local-header offset resolves while declaration is retained");

    EntrySpec disk_entry;
    disk_entry.disk_start = UINT16_MAX;
    disk_entry.extra = zip64_extra({0U}, true);
    const auto disk = inspect(temporary, "disk-zero.zip",
                              make_archive(disk_entry));
    require(disk.structurally_valid()
                && disk.directory->entries.front().declared_disk_start
                    == UINT16_MAX
                && disk.directory->entries.front().disk_start == 0U,
            "ZIP64 disk-start zero resolves under the single-disk profile");

    EntrySpec multiple;
    multiple.compressed = UINT32_MAX;
    multiple.uncompressed = UINT32_MAX;
    multiple.disk_start = UINT16_MAX;
    multiple.local_offset = UINT32_MAX;
    append_u16(multiple.extra, 1U);
    append_u16(multiple.extra, 28U);
    append_u64(multiple.extra, 11U);
    append_u64(multiple.extra, 7U);
    append_u64(multiple.extra, 0U);
    append_u32(multiple.extra, 0U);
    const auto combined = inspect(temporary, "combined.zip",
                                  make_archive(multiple));
    require(combined.structurally_valid(), "multiple ZIP64 values resolve");
    const auto& resolved = combined.directory->entries.front();
    require(resolved.uncompressed_size == 11U && resolved.compressed_size == 7U
                && resolved.local_header_offset == 0U
                && resolved.disk_start == 0U,
            "multiple ZIP64 values retain normative ordering");

    const auto pending_path = temporary.path() / "local-pending.zip";
    write_bytes(pending_path, make_archive(multiple));
    require(inspect_classic_local_layout(pending_path).status
                == LocalLayoutStatus::invalid_structure,
            "resolved central ZIP64 metadata proceeds into local validation");
}

void test_necessity_and_format_errors(const TemporaryDirectory& temporary)
{
    constexpr std::uint64_t central = 30U;
    constexpr std::uint64_t extra = central + 47U;

    EntrySpec missing;
    missing.uncompressed = UINT32_MAX;
    require_error(inspect(temporary, "missing.zip", make_archive(missing)),
                  CentralDirectoryErrorCode::missing_zip64_extra_field,
                  central + 24U, "missing required ZIP64 extra");

    EntrySpec unnecessary;
    unnecessary.extra = zip64_extra({1U});
    require_error(inspect(temporary, "unnecessary.zip",
                          make_archive(unnecessary)),
                  CentralDirectoryErrorCode::unnecessary_zip64_extra_field,
                  extra, "unnecessary ZIP64 extra");

    EntrySpec duplicate;
    duplicate.uncompressed = UINT32_MAX;
    duplicate.extra = zip64_extra({11U});
    const auto second = zip64_extra({12U});
    duplicate.extra.insert(duplicate.extra.end(), second.begin(), second.end());
    require_error(inspect(temporary, "duplicate.zip", make_archive(duplicate)),
                  CentralDirectoryErrorCode::duplicate_zip64_extra_field,
                  extra + 12U, "duplicate ZIP64 extra");

    EntrySpec wrong_id;
    wrong_id.uncompressed = UINT32_MAX;
    wrong_id.extra = zip64_extra({11U}, false, 2U);
    require_error(inspect(temporary, "wrong-id.zip", make_archive(wrong_id)),
                  CentralDirectoryErrorCode::prohibited_extra_field, extra,
                  "wrong extra-field ID");

    EntrySpec short_header;
    short_header.extra = {std::byte{1}, std::byte{0}, std::byte{8}};
    require_error(inspect(temporary, "short-header.zip",
                          make_archive(short_header)),
                  CentralDirectoryErrorCode::extra_field_header_truncated,
                  extra, "truncated extra-field header");

    EntrySpec out_of_bounds;
    out_of_bounds.uncompressed = UINT32_MAX;
    out_of_bounds.extra = {std::byte{1}, std::byte{0}, std::byte{8},
                           std::byte{0}, std::byte{1}};
    require_error(inspect(temporary, "data-out-of-bounds.zip",
                          make_archive(out_of_bounds)),
                  CentralDirectoryErrorCode::extra_field_data_out_of_bounds,
                  extra + 2U, "extra-field data bounds");

    EntrySpec truncated;
    truncated.uncompressed = UINT32_MAX;
    truncated.extra = zip64_extra({11U});
    truncated.extra.resize(8U);
    truncated.extra[2] = std::byte{4};
    truncated.extra[3] = std::byte{0};
    require_error(inspect(temporary, "truncated-value.zip",
                          make_archive(truncated)),
                  CentralDirectoryErrorCode::zip64_extra_required_value_truncated,
                  extra + 8U, "truncated required value");

    EntrySpec trailing;
    trailing.uncompressed = UINT32_MAX;
    trailing.extra = zip64_extra({11U, 12U});
    require_error(inspect(temporary, "trailing.zip", make_archive(trailing)),
                  CentralDirectoryErrorCode::zip64_extra_trailing_data,
                  extra + 12U, "trailing ZIP64 value");
}

void test_disk_and_offset_attacks(const TemporaryDirectory& temporary)
{
    constexpr std::uint64_t central = 30U;
    constexpr std::uint64_t extra = central + 47U;

    EntrySpec disk;
    disk.disk_start = UINT16_MAX;
    disk.extra = zip64_extra({1U}, true);
    require_error(inspect(temporary, "disk.zip", make_archive(disk)),
                  CentralDirectoryErrorCode::zip64_disk_start_multidisk,
                  extra + 4U, "forbidden resolved disk start");

    EntrySpec overflow;
    overflow.local_offset = UINT32_MAX;
    overflow.extra = zip64_extra({UINT64_MAX});
    require_error(inspect(temporary, "offset-overflow.zip",
                          make_archive(overflow)),
                  CentralDirectoryErrorCode::local_header_offset_out_of_bounds,
                  central + 42U, "checked local-header offset addition");

    EntrySpec overlap;
    overlap.local_offset = UINT32_MAX;
    overlap.extra = zip64_extra({30U});
    require_error(inspect(temporary, "offset-overlap.zip",
                          make_archive(overlap)),
                  CentralDirectoryErrorCode::local_header_range_overlaps_directory,
                  central + 42U, "resolved local-header physical bound");
}

} // namespace

int main()
{
    const TemporaryDirectory temporary;
    test_successful_resolution(temporary);
    test_necessity_and_format_errors(temporary);
    test_disk_and_offset_attacks(temporary);
    std::cout << "CORE_ZIP64_CENTRAL_ENTRY_TESTS_OK\n";
    return EXIT_SUCCESS;
}
