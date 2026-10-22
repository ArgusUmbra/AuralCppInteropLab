#include "../src/core/zip/classic_local_layout.hpp"

#include <chrono>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using aural::core::zip::CentralDirectoryErrorCode;
using aural::core::zip::ClassicZipLimits;
using aural::core::zip::LocalLayoutErrorCode;
using aural::core::zip::LocalLayoutInspection;
using aural::core::zip::LocalLayoutStatus;
using aural::core::zip::UnsupportedArchiveFeature;
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
                / ("aural-zip64-local-" + std::to_string(seed) + "-"
                   + std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) return;
        }
        throw std::runtime_error{"could not create ZIP64 local test directory"};
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

std::vector<std::byte> zip64_sizes(const std::uint64_t uncompressed,
                                   const std::uint64_t compressed,
                                   const std::uint16_t id = 1U)
{
    std::vector<std::byte> result;
    append_u16(result, id);
    append_u16(result, 16U);
    append_u64(result, uncompressed);
    append_u64(result, compressed);
    return result;
}

std::vector<std::byte> zip64_offset(const std::uint64_t offset)
{
    std::vector<std::byte> result;
    append_u16(result, 1U);
    append_u16(result, 8U);
    append_u64(result, offset);
    return result;
}

struct EntrySpec final {
    std::uint16_t version = 45U;
    std::uint16_t flags = 0U;
    std::uint32_t local_compressed = 3U;
    std::uint32_t local_uncompressed = 3U;
    std::uint32_t central_compressed = 3U;
    std::uint32_t central_uncompressed = 3U;
    std::uint16_t central_disk = 0U;
    std::uint32_t central_local_offset = 0U;
    std::vector<std::byte> local_extra;
    std::vector<std::byte> central_extra;
    std::vector<std::byte> payload = {
        std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
};

struct Archive final {
    std::vector<std::byte> bytes;
    std::size_t central_offset;
    std::size_t central_extra_offset;
    std::optional<std::size_t> zip64_eocd_offset;
};

Archive make_archive(const EntrySpec& spec = {},
                     const bool archive_zip64 = false)
{
    Archive archive{};
    append_u32(archive.bytes, UINT32_C(0x04034b50));
    append_u16(archive.bytes, spec.version);
    append_u16(archive.bytes, spec.flags);
    append_u16(archive.bytes, 0U);
    append_u16(archive.bytes, 0U);
    append_u16(archive.bytes, 0U);
    append_u32(archive.bytes, 0U);
    append_u32(archive.bytes, spec.local_compressed);
    append_u32(archive.bytes, spec.local_uncompressed);
    append_u16(archive.bytes, 1U);
    append_u16(archive.bytes,
               static_cast<std::uint16_t>(spec.local_extra.size()));
    archive.bytes.push_back(std::byte{'a'});
    archive.bytes.insert(archive.bytes.end(), spec.local_extra.begin(),
                         spec.local_extra.end());
    archive.bytes.insert(archive.bytes.end(), spec.payload.begin(),
                         spec.payload.end());

    archive.central_offset = archive.bytes.size();
    append_u32(archive.bytes, UINT32_C(0x02014b50));
    append_u16(archive.bytes, 45U);
    append_u16(archive.bytes, spec.version);
    append_u16(archive.bytes, spec.flags);
    append_u16(archive.bytes, 0U);
    append_u16(archive.bytes, 0U);
    append_u16(archive.bytes, 0U);
    append_u32(archive.bytes, 0U);
    append_u32(archive.bytes, spec.central_compressed);
    append_u32(archive.bytes, spec.central_uncompressed);
    append_u16(archive.bytes, 1U);
    append_u16(archive.bytes,
               static_cast<std::uint16_t>(spec.central_extra.size()));
    append_u16(archive.bytes, 0U);
    append_u16(archive.bytes, spec.central_disk);
    append_u16(archive.bytes, 0U);
    append_u32(archive.bytes, 0U);
    append_u32(archive.bytes, spec.central_local_offset);
    archive.bytes.push_back(std::byte{'a'});
    archive.central_extra_offset = archive.bytes.size();
    archive.bytes.insert(archive.bytes.end(), spec.central_extra.begin(),
                         spec.central_extra.end());
    const std::uint64_t central_size = archive.bytes.size()
        - archive.central_offset;

    if (archive_zip64) {
        archive.zip64_eocd_offset = archive.bytes.size();
        append_u32(archive.bytes, UINT32_C(0x06064b50));
        append_u64(archive.bytes, 44U);
        append_u16(archive.bytes, 45U);
        append_u16(archive.bytes, 45U);
        append_u32(archive.bytes, 0U);
        append_u32(archive.bytes, 0U);
        append_u64(archive.bytes, 1U);
        append_u64(archive.bytes, 1U);
        append_u64(archive.bytes, central_size);
        append_u64(archive.bytes, archive.central_offset);
        append_u32(archive.bytes, UINT32_C(0x07064b50));
        append_u32(archive.bytes, 0U);
        append_u64(archive.bytes, *archive.zip64_eocd_offset);
        append_u32(archive.bytes, 1U);
    }

    append_u32(archive.bytes, UINT32_C(0x06054b50));
    append_u16(archive.bytes, 0U);
    append_u16(archive.bytes, 0U);
    append_u16(archive.bytes, archive_zip64 ? UINT16_MAX : 1U);
    append_u16(archive.bytes, archive_zip64 ? UINT16_MAX : 1U);
    append_u32(archive.bytes, static_cast<std::uint32_t>(central_size));
    append_u32(archive.bytes,
               static_cast<std::uint32_t>(archive.central_offset));
    append_u16(archive.bytes, 0U);
    return archive;
}

struct TwoEntryArchive final {
    std::vector<std::byte> bytes;
    std::array<std::size_t, 2U> locals;
    std::array<std::size_t, 2U> centrals;
    std::size_t central_offset;
};

TwoEntryArchive make_two_entry_archive()
{
    TwoEntryArchive archive{};
    for (std::size_t index = 0U; index < 2U; ++index) {
        archive.locals[index] = archive.bytes.size();
        append_u32(archive.bytes, UINT32_C(0x04034b50));
        append_u16(archive.bytes, 20U);
        append_u16(archive.bytes, 0U);
        append_u16(archive.bytes, 0U);
        append_u16(archive.bytes, 0U);
        append_u16(archive.bytes, 0U);
        append_u32(archive.bytes, 0U);
        append_u32(archive.bytes, 3U);
        append_u32(archive.bytes, 3U);
        append_u16(archive.bytes, 1U);
        append_u16(archive.bytes, 0U);
        archive.bytes.push_back(std::byte{'a'});
        archive.bytes.push_back(std::byte{'1'});
        archive.bytes.push_back(std::byte{'2'});
        archive.bytes.push_back(std::byte{'3'});
    }
    archive.central_offset = archive.bytes.size();
    for (std::size_t index = 0U; index < 2U; ++index) {
        archive.centrals[index] = archive.bytes.size();
        append_u32(archive.bytes, UINT32_C(0x02014b50));
        append_u16(archive.bytes, 20U);
        append_u16(archive.bytes, 20U);
        append_u16(archive.bytes, 0U);
        append_u16(archive.bytes, 0U);
        append_u16(archive.bytes, 0U);
        append_u16(archive.bytes, 0U);
        append_u32(archive.bytes, 0U);
        append_u32(archive.bytes, 3U);
        append_u32(archive.bytes, 3U);
        append_u16(archive.bytes, 1U);
        append_u16(archive.bytes, 0U);
        append_u16(archive.bytes, 0U);
        append_u16(archive.bytes, 0U);
        append_u16(archive.bytes, 0U);
        append_u32(archive.bytes, 0U);
        append_u32(archive.bytes,
                   static_cast<std::uint32_t>(archive.locals[index]));
        archive.bytes.push_back(std::byte{'a'});
    }
    const std::uint32_t central_size = static_cast<std::uint32_t>(
        archive.bytes.size() - archive.central_offset);
    append_u32(archive.bytes, UINT32_C(0x06054b50));
    append_u16(archive.bytes, 0U);
    append_u16(archive.bytes, 0U);
    append_u16(archive.bytes, 2U);
    append_u16(archive.bytes, 2U);
    append_u32(archive.bytes, central_size);
    append_u32(archive.bytes,
               static_cast<std::uint32_t>(archive.central_offset));
    append_u16(archive.bytes, 0U);
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

LocalLayoutInspection inspect(const TemporaryDirectory& temporary,
                              const std::string_view name,
                              const std::vector<std::byte>& bytes,
                              const ClassicZipLimits limits = {})
{
    const auto path = temporary.path() / name;
    write_bytes(path, bytes);
    const auto before = read_bytes(path);
    const auto first = inspect_classic_local_layout(path, limits);
    const auto second = inspect_classic_local_layout(path, limits);
    require(read_bytes(path) == before, "inspection must not modify input");
    require(first.status == second.status, "status must be deterministic");
    require(first.error.has_value() == second.error.has_value(),
            "error shape must be deterministic");
    if (first.error) {
        require(first.error->code == second.error->code
                    && first.error->offset == second.error->offset,
                "error detail must be deterministic");
    }
    return first;
}

void require_error(const LocalLayoutInspection& result,
                   const LocalLayoutErrorCode code,
                   const std::uint64_t offset,
                   const std::string_view context)
{
    require(result.status == LocalLayoutStatus::invalid_structure,
            std::string{context} + ": wrong status");
    require(result.error && result.error->code == code,
            std::string{context} + ": wrong error code");
    require(result.error->offset == offset,
            std::string{context} + ": wrong error offset");
    require(!result.layout, std::string{context} + ": partial layout leaked");
}

void test_valid_representations(const TemporaryDirectory& temporary)
{
    const auto classic = inspect(temporary, "classic.zip", make_archive().bytes);
    require(classic.profile_valid() && classic.layout->entries.size() == 1U,
            "classic ZIP regression");

    EntrySpec local_only;
    local_only.local_compressed = UINT32_MAX;
    local_only.local_uncompressed = UINT32_MAX;
    local_only.local_extra = zip64_sizes(3U, 3U);
    const auto local = inspect(temporary, "local-only.zip",
                               make_archive(local_only).bytes);
    require(local.profile_valid()
                && local.layout->entries.front().payload_size == 3U
                && local.layout->entries.front().payload_offset == 51U,
            "local-only ZIP64 sizes resolve to a physical payload");

    EntrySpec central_only;
    central_only.central_compressed = UINT32_MAX;
    central_only.central_uncompressed = UINT32_MAX;
    central_only.central_extra = zip64_sizes(3U, 3U);
    require(inspect(temporary, "central-only.zip",
                    make_archive(central_only).bytes).profile_valid(),
            "central-only ZIP64 sizes agree with classic local sizes");

    EntrySpec central_offset;
    central_offset.central_local_offset = UINT32_MAX;
    central_offset.central_extra = zip64_offset(0U);
    require(inspect(temporary, "central-offset.zip",
                    make_archive(central_offset).bytes).profile_valid(),
            "central-only ZIP64 local offset drives local lookup");

    EntrySpec both = local_only;
    both.central_compressed = UINT32_MAX;
    both.central_uncompressed = UINT32_MAX;
    both.central_extra = zip64_sizes(3U, 3U);
    require(inspect(temporary, "both.zip", make_archive(both, true).bytes)
                .profile_valid(),
            "matching local and central ZIP64 values validate with ZIP64 EOCD");

    EntrySpec empty;
    empty.local_compressed = 0U;
    empty.local_uncompressed = 0U;
    empty.central_compressed = 0U;
    empty.central_uncompressed = 0U;
    empty.payload.clear();
    const auto zero = inspect(temporary, "empty.zip", make_archive(empty).bytes);
    require(zero.profile_valid()
                && zero.layout->entries.front().payload_size == 0U,
            "zero-length stored entry is structurally valid");
}

void test_local_extra_errors(const TemporaryDirectory& temporary)
{
    EntrySpec missing;
    missing.local_compressed = UINT32_MAX;
    missing.local_uncompressed = UINT32_MAX;
    require_error(inspect(temporary, "missing.zip", make_archive(missing).bytes),
                  LocalLayoutErrorCode::missing_local_zip64_extra_field, 18U,
                  "missing local ZIP64 extra");

    EntrySpec one_sentinel = missing;
    one_sentinel.local_uncompressed = 3U;
    require_error(inspect(temporary, "one-sentinel.zip",
                          make_archive(one_sentinel).bytes),
                  LocalLayoutErrorCode::local_zip64_size_sentinels_incomplete,
                  22U, "APPNOTE local size pair");

    EntrySpec unnecessary;
    unnecessary.local_extra = zip64_sizes(3U, 3U);
    require_error(inspect(temporary, "unnecessary.zip",
                          make_archive(unnecessary).bytes),
                  LocalLayoutErrorCode::unnecessary_local_zip64_extra_field,
                  31U, "unnecessary local ZIP64 extra");

    EntrySpec wrong_id = missing;
    wrong_id.local_extra = zip64_sizes(3U, 3U, 2U);
    require_error(inspect(temporary, "wrong-id.zip",
                          make_archive(wrong_id).bytes),
                  LocalLayoutErrorCode::prohibited_local_extra_field, 31U,
                  "wrong local extra ID");

    EntrySpec duplicate = missing;
    duplicate.local_extra = zip64_sizes(3U, 3U);
    const auto second = zip64_sizes(3U, 3U);
    duplicate.local_extra.insert(duplicate.local_extra.end(), second.begin(),
                                 second.end());
    require_error(inspect(temporary, "duplicate.zip",
                          make_archive(duplicate).bytes),
                  LocalLayoutErrorCode::duplicate_local_zip64_extra_field,
                  51U, "duplicate local ZIP64 extra");

    EntrySpec short_header = missing;
    short_header.local_extra = {
        std::byte{1}, std::byte{0}, std::byte{16}};
    require_error(inspect(temporary, "short-header.zip",
                          make_archive(short_header).bytes),
                  LocalLayoutErrorCode::local_extra_field_header_truncated,
                  31U, "truncated local extra header");

    EntrySpec out_of_bounds = missing;
    out_of_bounds.local_extra = {
        std::byte{1}, std::byte{0}, std::byte{16}, std::byte{0}, std::byte{0}};
    require_error(inspect(temporary, "extra-bounds.zip",
                          make_archive(out_of_bounds).bytes),
                  LocalLayoutErrorCode::local_extra_field_data_out_of_bounds,
                  33U, "local extra data bounds");

    EntrySpec truncated = missing;
    truncated.local_extra = zip64_sizes(3U, 3U);
    truncated.local_extra.resize(12U);
    set_u16(truncated.local_extra, 2U, 8U);
    require_error(inspect(temporary, "truncated-value.zip",
                          make_archive(truncated).bytes),
                  LocalLayoutErrorCode::
                      local_zip64_extra_required_value_truncated,
                  43U, "truncated required local value");

    EntrySpec surplus = missing;
    surplus.local_extra = zip64_sizes(3U, 3U);
    set_u16(surplus.local_extra, 2U, 24U);
    append_u64(surplus.local_extra, 0U);
    require_error(inspect(temporary, "surplus.zip",
                          make_archive(surplus).bytes),
                  LocalLayoutErrorCode::local_zip64_extra_trailing_data, 51U,
                  "surplus local ZIP64 value");
}

void test_consistency_and_profile(const TemporaryDirectory& temporary)
{
    EntrySpec mismatch;
    mismatch.local_compressed = UINT32_MAX;
    mismatch.local_uncompressed = UINT32_MAX;
    mismatch.local_extra = zip64_sizes(4U, 4U);
    require_error(inspect(temporary, "mismatch.zip",
                          make_archive(mismatch).bytes),
                  LocalLayoutErrorCode::compressed_size_mismatch, 18U,
                  "resolved local and central size mismatch");

    EntrySpec order;
    order.local_compressed = UINT32_MAX;
    order.local_uncompressed = UINT32_MAX;
    order.central_compressed = 4U;
    order.central_uncompressed = 3U;
    order.payload.push_back(std::byte{'d'});
    order.local_extra = zip64_sizes(4U, 3U);
    require_error(inspect(temporary, "wrong-order.zip",
                          make_archive(order).bytes),
                  LocalLayoutErrorCode::compressed_size_mismatch, 18U,
                  "local ZIP64 values use uncompressed then compressed order");

    EntrySpec old_version;
    old_version.version = 20U;
    old_version.local_compressed = UINT32_MAX;
    old_version.local_uncompressed = UINT32_MAX;
    old_version.local_extra = zip64_sizes(3U, 3U);
    const auto version = inspect(temporary, "version.zip",
                                 make_archive(old_version).bytes);
    require(version.status == LocalLayoutStatus::unsupported_archive_feature
                && version.unsupported
                && version.unsupported->feature
                    == UnsupportedArchiveFeature::version_needed,
            "ZIP64 requires version-needed 4.5");

    EntrySpec descriptor;
    descriptor.flags = 8U;
    const auto descriptor_result = inspect(
        temporary, "descriptor.zip", make_archive(descriptor).bytes);
    require(descriptor_result.status
                == LocalLayoutStatus::unsupported_archive_feature
                && descriptor_result.unsupported
                && descriptor_result.unsupported->feature
                    == UnsupportedArchiveFeature::data_descriptor,
            "data descriptors remain prohibited");

    EntrySpec disk;
    disk.central_disk = UINT16_MAX;
    append_u16(disk.central_extra, 1U);
    append_u16(disk.central_extra, 4U);
    append_u32(disk.central_extra, 1U);
    const auto disk_result = inspect(temporary, "disk.zip",
                                     make_archive(disk).bytes);
    require(disk_result.status == LocalLayoutStatus::invalid_structure
                && disk_result.central_error
                && disk_result.central_error->code
                    == CentralDirectoryErrorCode::zip64_disk_start_multidisk,
            "resolved nonzero disk start is rejected");
}

void test_ranges_and_limits(const TemporaryDirectory& temporary)
{
    EntrySpec invalid_offset;
    invalid_offset.central_local_offset = UINT32_MAX;
    invalid_offset.central_extra = zip64_offset(UINT64_MAX);
    const auto offset = inspect(temporary, "offset.zip",
                                make_archive(invalid_offset).bytes);
    require(offset.status == LocalLayoutStatus::invalid_structure
                && offset.central_error
                && offset.central_error->code
                    == CentralDirectoryErrorCode::
                        local_header_offset_out_of_bounds,
            "effective 64-bit local offset is bounded");

    EntrySpec huge;
    huge.local_compressed = UINT32_MAX;
    huge.local_uncompressed = UINT32_MAX;
    huge.central_compressed = UINT32_MAX;
    huge.central_uncompressed = UINT32_MAX;
    huge.local_extra = zip64_sizes(UINT64_C(8589934593),
                                   UINT64_C(8589934593));
    huge.central_extra = zip64_sizes(UINT64_C(8589934593),
                                     UINT64_C(8589934593));
    require_error(inspect(temporary, "limit.zip", make_archive(huge).bytes),
                  LocalLayoutErrorCode::entry_size_limit_exceeded,
                  make_archive(huge).central_offset + 24U,
                  "AURAL per-entry size limit");

    EntrySpec truncated = huge;
    truncated.local_extra = zip64_sizes(UINT64_C(8589934592),
                                        UINT64_C(8589934592));
    truncated.central_extra = truncated.local_extra;
    require_error(inspect(temporary, "payload-oob.zip",
                          make_archive(truncated).bytes),
                  LocalLayoutErrorCode::payload_truncated, 51U,
                  "large declaration rejected without large allocation");

    EntrySpec crossing;
    crossing.local_compressed = 4U;
    crossing.local_uncompressed = 4U;
    crossing.central_compressed = 4U;
    crossing.central_uncompressed = 4U;
    const auto crossing_archive = make_archive(crossing, true);
    require_error(inspect(temporary, "zip64-end-crossing.zip",
                          crossing_archive.bytes),
                  LocalLayoutErrorCode::entry_intersects_central_directory,
                  31U, "central-directory barrier protects ZIP64 end records");

    auto prefix = make_archive();
    prefix.bytes.insert(prefix.bytes.begin(), std::byte{0});
    prefix.central_offset += 1U;
    prefix.central_extra_offset += 1U;
    set_u32(prefix.bytes, prefix.central_offset + 42U, 1U);
    const std::size_t eocd = prefix.bytes.size() - 22U;
    set_u32(prefix.bytes, eocd + 16U,
            static_cast<std::uint32_t>(prefix.central_offset));
    require_error(inspect(temporary, "prefix.zip", prefix.bytes),
                  LocalLayoutErrorCode::archive_prefix_prohibited, 0U,
                  "physical prefix");
}

void test_overlaps(const TemporaryDirectory& temporary)
{
    auto duplicate = make_two_entry_archive();
    set_u32(duplicate.bytes, duplicate.centrals[1] + 42U, 0U);
    require_error(inspect(temporary, "duplicate-reference.zip",
                          duplicate.bytes),
                  LocalLayoutErrorCode::duplicate_local_header_reference,
                  duplicate.centrals[1] + 42U,
                  "duplicate effective local-header reference");

    auto header_payload = make_two_entry_archive();
    set_u32(header_payload.bytes, 18U, 34U);
    set_u32(header_payload.bytes, 22U, 34U);
    set_u32(header_payload.bytes,
            header_payload.centrals[0] + 20U, 34U);
    set_u32(header_payload.bytes,
            header_payload.centrals[0] + 24U, 34U);
    require_error(inspect(temporary, "header-payload.zip",
                          header_payload.bytes),
                  LocalLayoutErrorCode::header_payload_overlap,
                  header_payload.locals[1], "payload and header overlap");

    auto payloads = make_two_entry_archive();
    set_u32(payloads.bytes, 18U, 35U);
    set_u32(payloads.bytes, 22U, 35U);
    set_u32(payloads.bytes, payloads.centrals[0] + 20U, 35U);
    set_u32(payloads.bytes, payloads.centrals[0] + 24U, 35U);
    require_error(inspect(temporary, "payloads.zip", payloads.bytes),
                  LocalLayoutErrorCode::payloads_overlap,
                  payloads.locals[1] + 31U, "payload ranges overlap");
}

void test_frozen_fixtures(const std::filesystem::path& fixtures)
{
    const auto classic = inspect_classic_local_layout(
        fixtures / "valid-independent.aural");
    require(classic.profile_valid(), "frozen classic fixture remains valid");
    const auto zip64 = inspect_classic_local_layout(
        fixtures / "valid-required-zip64-end.aural");
    require(zip64.profile_valid(),
            "frozen archive-level ZIP64 fixture has complete physical layout");
}

} // namespace

int main(const int argc, char** argv)
{
    require(argc == 2, "fixture directory argument required");
    const TemporaryDirectory temporary;
    test_valid_representations(temporary);
    test_local_extra_errors(temporary);
    test_consistency_and_profile(temporary);
    test_ranges_and_limits(temporary);
    test_overlaps(temporary);
    test_frozen_fixtures(argv[1]);
    std::cout << "CORE_ZIP64_LOCAL_LAYOUT_TESTS_OK\n";
    return EXIT_SUCCESS;
}
