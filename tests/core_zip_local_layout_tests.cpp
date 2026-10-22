#include "../src/core/zip/classic_local_layout.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using aural::core::zip::CentralDirectoryErrorCode;
using aural::core::zip::LocalLayoutErrorCode;
using aural::core::zip::LocalLayoutInspection;
using aural::core::zip::LocalLayoutStatus;
using aural::core::zip::UnsupportedArchiveFeature;

void require(const bool condition, const std::string_view message)
{
    if (!condition) throw std::runtime_error{std::string{message}};
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
                / ("aural-local-layout-" + std::to_string(seed) + "-"
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

void append_u16(std::vector<std::byte>& bytes, const std::uint16_t value)
{
    bytes.push_back(static_cast<std::byte>(value & 0xffU));
    bytes.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
}

void append_u32(std::vector<std::byte>& bytes, const std::uint32_t value)
{
    append_u16(bytes, static_cast<std::uint16_t>(value & 0xffffU));
    append_u16(bytes, static_cast<std::uint16_t>(value >> 16U));
}

void set_u16(std::vector<std::byte>& bytes,
             const std::size_t offset,
             const std::uint16_t value)
{
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::byte>((value >> 8U) & 0xffU);
}

void set_u32(std::vector<std::byte>& bytes,
             const std::size_t offset,
             const std::uint32_t value)
{
    set_u16(bytes, offset, static_cast<std::uint16_t>(value & 0xffffU));
    set_u16(bytes, offset + 2U, static_cast<std::uint16_t>(value >> 16U));
}

std::vector<std::byte> octets(const std::string_view text)
{
    std::vector<std::byte> result;
    for (const unsigned char value : text) {
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

struct EntrySpec final {
    std::vector<std::byte> name;
    std::vector<std::byte> payload;
    std::vector<std::byte> local_extra;
    std::vector<std::byte> central_extra;
    std::vector<std::byte> comment;
    std::uint16_t version = 20U;
    std::uint16_t flags = 0U;
    std::uint16_t method = 0U;
    std::uint16_t time = UINT16_C(0x1234);
    std::uint16_t date = UINT16_C(0x5678);
    std::uint32_t crc = UINT32_C(0x10203040);
};

struct Archive final {
    std::vector<std::byte> bytes;
    std::vector<std::size_t> locals;
    std::vector<std::size_t> centrals;
    std::size_t central_offset{};
    std::size_t eocd_offset{};
};

Archive make_archive(const std::vector<EntrySpec>& specs,
                     std::vector<std::size_t> central_order = {},
                     const std::vector<std::byte>& archive_comment = {})
{
    Archive result;
    if (central_order.empty()) {
        for (std::size_t index = 0; index < specs.size(); ++index) {
            central_order.push_back(index);
        }
    }
    for (const auto& spec : specs) {
        result.locals.push_back(result.bytes.size());
        append_u32(result.bytes, UINT32_C(0x04034b50));
        append_u16(result.bytes, spec.version);
        append_u16(result.bytes, spec.flags);
        append_u16(result.bytes, spec.method);
        append_u16(result.bytes, spec.time);
        append_u16(result.bytes, spec.date);
        append_u32(result.bytes, spec.crc);
        append_u32(result.bytes,
                   static_cast<std::uint32_t>(spec.payload.size()));
        append_u32(result.bytes,
                   static_cast<std::uint32_t>(spec.payload.size()));
        append_u16(result.bytes, static_cast<std::uint16_t>(spec.name.size()));
        append_u16(result.bytes,
                   static_cast<std::uint16_t>(spec.local_extra.size()));
        result.bytes.insert(result.bytes.end(), spec.name.begin(), spec.name.end());
        result.bytes.insert(result.bytes.end(), spec.local_extra.begin(),
                            spec.local_extra.end());
        result.bytes.insert(result.bytes.end(), spec.payload.begin(),
                            spec.payload.end());
    }
    result.central_offset = result.bytes.size();
    for (const auto index : central_order) {
        const auto& spec = specs[index];
        result.centrals.push_back(result.bytes.size());
        append_u32(result.bytes, UINT32_C(0x02014b50));
        append_u16(result.bytes, 63U);
        append_u16(result.bytes, spec.version);
        append_u16(result.bytes, spec.flags);
        append_u16(result.bytes, spec.method);
        append_u16(result.bytes, spec.time);
        append_u16(result.bytes, spec.date);
        append_u32(result.bytes, spec.crc);
        append_u32(result.bytes,
                   static_cast<std::uint32_t>(spec.payload.size()));
        append_u32(result.bytes,
                   static_cast<std::uint32_t>(spec.payload.size()));
        append_u16(result.bytes, static_cast<std::uint16_t>(spec.name.size()));
        append_u16(result.bytes,
                   static_cast<std::uint16_t>(spec.central_extra.size()));
        append_u16(result.bytes, static_cast<std::uint16_t>(spec.comment.size()));
        append_u16(result.bytes, 0U);
        append_u16(result.bytes, 0U);
        append_u32(result.bytes, 0U);
        append_u32(result.bytes,
                   static_cast<std::uint32_t>(result.locals[index]));
        result.bytes.insert(result.bytes.end(), spec.name.begin(), spec.name.end());
        result.bytes.insert(result.bytes.end(), spec.central_extra.begin(),
                            spec.central_extra.end());
        result.bytes.insert(result.bytes.end(), spec.comment.begin(),
                            spec.comment.end());
    }
    result.eocd_offset = result.bytes.size();
    append_u32(result.bytes, UINT32_C(0x06054b50));
    append_u16(result.bytes, 0U);
    append_u16(result.bytes, 0U);
    append_u16(result.bytes, static_cast<std::uint16_t>(specs.size()));
    append_u16(result.bytes, static_cast<std::uint16_t>(specs.size()));
    append_u32(result.bytes,
               static_cast<std::uint32_t>(result.eocd_offset
                                          - result.central_offset));
    append_u32(result.bytes, static_cast<std::uint32_t>(result.central_offset));
    append_u16(result.bytes,
               static_cast<std::uint16_t>(archive_comment.size()));
    result.bytes.insert(result.bytes.end(), archive_comment.begin(),
                        archive_comment.end());
    return result;
}

void write_bytes(const std::filesystem::path& path,
                 const std::span<const std::byte> bytes)
{
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    require(static_cast<bool>(output), "create archive");
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(output), "write archive");
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
                              const std::span<const std::byte> bytes)
{
    const auto path = temporary.path() / name;
    write_bytes(path, bytes);
    const auto before = read_bytes(path);
    const auto first = aural::core::zip::inspect_classic_local_layout(path);
    const auto second = aural::core::zip::inspect_classic_local_layout(path);
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
            std::string{context} + ": status");
    require(result.error && result.error->code == code,
            std::string{context} + ": code");
    require(result.error->offset == offset,
            std::string{context} + ": offset");
    require(!aural::core::zip::local_layout_error_message(code).empty(),
            "error messages are nonempty");
}

void require_unsupported(const LocalLayoutInspection& result,
                         const UnsupportedArchiveFeature feature,
                         const std::string_view context)
{
    require(result.status == LocalLayoutStatus::unsupported_archive_feature,
            std::string{context} + ": status");
    require(result.unsupported && result.unsupported->feature == feature,
            std::string{context} + ": feature");
}

EntrySpec entry(const std::string_view name,
                const std::string_view payload = {})
{
    EntrySpec result;
    result.name = octets(name);
    result.payload = octets(payload);
    return result;
}

Archive make_overlapping_headers_archive()
{
    std::vector<std::byte> nested;
    append_u32(nested, UINT32_C(0x04034b50));
    append_u16(nested, 20U);
    append_u16(nested, 0U);
    append_u16(nested, 0U);
    append_u16(nested, UINT16_C(0x1234));
    append_u16(nested, UINT16_C(0x5678));
    append_u32(nested, UINT32_C(0x10203040));
    append_u32(nested, 0U);
    append_u32(nested, 0U);
    append_u16(nested, 0U);
    append_u16(nested, 0U);

    auto first = entry("unused");
    first.name = nested;
    auto result = make_archive({first, entry("")});
    const std::size_t redundant_second = 30U + nested.size();
    result.bytes.erase(
        result.bytes.begin() + static_cast<std::ptrdiff_t>(redundant_second),
        result.bytes.begin()
            + static_cast<std::ptrdiff_t>(redundant_second + 30U));
    result.central_offset -= 30U;
    result.eocd_offset -= 30U;
    result.centrals[0] -= 30U;
    result.centrals[1] -= 30U;
    set_u32(result.bytes, result.centrals[1] + 42U, 30U);
    set_u32(result.bytes, result.eocd_offset + 16U,
            static_cast<std::uint32_t>(result.central_offset));
    return result;
}

void test_valid_layouts(const TemporaryDirectory& temporary)
{
    const auto one = make_archive({entry("a", "data")});
    const auto one_result = inspect(temporary, "valid-one.zip", one.bytes);
    require(one_result.profile_valid() && one_result.layout->entries.size() == 1U,
            "minimal classic layout is valid");
    require(one_result.layout->entries[0].payload_offset == 31U
                && one_result.layout->entries[0].payload_end == 35U,
            "payload bounds are derived");

    const auto multiple = make_archive({entry("a", "12"), entry("b", "345")});
    require(inspect(temporary, "valid-multiple.zip", multiple.bytes)
                .profile_valid(),
            "multiple contiguous entries are valid");

    const auto reordered = make_archive(
        {entry("first", "1"), entry("second", "22")}, {1U, 0U});
    const auto reordered_result =
        inspect(temporary, "reordered-central.zip", reordered.bytes);
    require(reordered_result.profile_valid()
                && reordered_result.layout->entries.front().local_header_offset
                    == 0U,
            "central order does not control physical order");

    const auto zero = make_archive({entry("empty")});
    require(inspect(temporary, "zero-payload.zip", zero.bytes).profile_valid(),
            "zero-length payload is valid");
}

void test_header_and_field_failures(const TemporaryDirectory& temporary)
{
    const auto base = make_archive({entry("name", "x")});

    auto wrong_signature = base;
    wrong_signature.bytes[0] = std::byte{0};
    require_error(inspect(temporary, "bad-local-signature.zip",
                          wrong_signature.bytes),
                  LocalLayoutErrorCode::invalid_local_header_signature, 0U,
                  "signature");

    auto bogus_offset = base;
    set_u32(bogus_offset.bytes, bogus_offset.centrals[0] + 42U,
            UINT32_C(0xfffffffe));
    require_error(inspect(temporary, "bogus-offset.zip", bogus_offset.bytes),
                  LocalLayoutErrorCode::local_header_offset_out_of_bounds,
                  bogus_offset.centrals[0] + 42U, "bogus offset");

    auto truncated = base;
    set_u32(truncated.bytes, truncated.centrals[0] + 42U,
            static_cast<std::uint32_t>(truncated.central_offset - 20U));
    require_error(inspect(temporary, "truncated-local.zip", truncated.bytes),
                  LocalLayoutErrorCode::local_header_truncated,
                  truncated.central_offset - 20U, "truncated local fields");

    auto short_name = base;
    set_u16(short_name.bytes, 26U, 3U);
    require_error(inspect(temporary, "short-name.zip", short_name.bytes),
                  LocalLayoutErrorCode::filename_mismatch, 30U,
                  "short local name declaration");

    auto long_name = base;
    set_u16(long_name.bytes, 26U, 5U);
    require_error(inspect(temporary, "long-name.zip", long_name.bytes),
                  LocalLayoutErrorCode::filename_mismatch, 30U,
                  "long local name declaration");

    auto name_mismatch = base;
    name_mismatch.bytes[30U] = std::byte{'N'};
    require_error(inspect(temporary, "name-mismatch.zip", name_mismatch.bytes),
                  LocalLayoutErrorCode::filename_mismatch, 30U,
                  "filename bytes");

    struct Mutation final {
        std::size_t local_field;
        LocalLayoutErrorCode error;
    };
    for (const auto mutation : {
             Mutation{4U, LocalLayoutErrorCode::version_needed_mismatch},
             Mutation{6U, LocalLayoutErrorCode::flags_mismatch},
             Mutation{8U, LocalLayoutErrorCode::compression_method_mismatch},
             Mutation{10U, LocalLayoutErrorCode::modification_time_mismatch},
             Mutation{12U, LocalLayoutErrorCode::modification_date_mismatch},
             Mutation{14U, LocalLayoutErrorCode::crc32_mismatch},
             Mutation{18U, LocalLayoutErrorCode::compressed_size_mismatch},
             Mutation{22U, LocalLayoutErrorCode::uncompressed_size_mismatch}}) {
        auto changed = base;
        changed.bytes[mutation.local_field] ^= std::byte{1};
        require_error(inspect(temporary,
                              "field-" + std::to_string(mutation.local_field)
                                  + ".zip",
                              changed.bytes),
                      mutation.error, mutation.local_field,
                      "central/local field mismatch");
    }

    auto extra_length = base;
    set_u16(extra_length.bytes, 28U, UINT16_MAX);
    require_error(inspect(temporary, "extra-overrun.zip", extra_length.bytes),
                  LocalLayoutErrorCode::local_variable_fields_out_of_bounds,
                  26U, "extra length manipulation");
}

void test_profile_features(const TemporaryDirectory& temporary)
{
    auto descriptor_spec = entry("a", "x");
    descriptor_spec.flags = 8U;
    require_unsupported(
        inspect(temporary, "descriptor.zip",
                make_archive({descriptor_spec}).bytes),
        UnsupportedArchiveFeature::data_descriptor, "descriptor mode");

    auto encrypted = entry("a", "x");
    encrypted.flags = 1U;
    require_unsupported(inspect(temporary, "flag.zip",
                                make_archive({encrypted}).bytes),
                        UnsupportedArchiveFeature::general_purpose_flag,
                        "prohibited flag");

    auto deflated = entry("a", "x");
    deflated.method = 8U;
    require_unsupported(inspect(temporary, "method.zip",
                                make_archive({deflated}).bytes),
                        UnsupportedArchiveFeature::compression_method,
                        "compression method");

    auto local_extra = entry("a", "x");
    local_extra.local_extra = octets("x");
    require_error(inspect(temporary, "local-extra.zip",
                          make_archive({local_extra}).bytes),
                  LocalLayoutErrorCode::local_extra_field_header_truncated,
                  31U, "local extra");

    auto central_extra = entry("a", "x");
    central_extra.central_extra = octets("x");
    const auto central_extra_archive = make_archive({central_extra});
    const auto central_extra_result = inspect(
        temporary, "central-extra.zip", central_extra_archive.bytes);
    require(central_extra_result.status == LocalLayoutStatus::invalid_structure
                && central_extra_result.error
                && central_extra_result.error->code
                    == LocalLayoutErrorCode::central_directory_invalid
                && central_extra_result.central_error
                && central_extra_result.central_error->code
                    == CentralDirectoryErrorCode::extra_field_header_truncated,
            "prohibited central extra fails in central parsing");

    auto comment = entry("a", "x");
    comment.comment = octets("x");
    require_unsupported(inspect(temporary, "entry-comment.zip",
                                make_archive({comment}).bytes),
                        UnsupportedArchiveFeature::entry_comment,
                        "entry comment");

    require_unsupported(inspect(temporary, "archive-comment.zip",
                                make_archive({entry("a")}, {}, octets("x")).bytes),
                        UnsupportedArchiveFeature::archive_comment,
                        "archive comment");

    auto zip64_local = make_archive({entry("a", "x")});
    set_u32(zip64_local.bytes, 18U, UINT32_MAX);
    const auto zip64_result =
        inspect(temporary, "zip64-local.zip", zip64_local.bytes);
    require(zip64_result.status == LocalLayoutStatus::invalid_structure
                && zip64_result.error
                && zip64_result.error->code
                    == LocalLayoutErrorCode::
                        local_zip64_size_sentinels_incomplete
                && zip64_result.error->offset == 22U,
            "local ZIP64 size sentinels must be paired");

    auto zip64_central = make_archive({entry("a", "x")});
    set_u32(zip64_central.bytes, zip64_central.centrals[0] + 20U,
            UINT32_MAX);
    const auto central_zip64_result =
        inspect(temporary, "zip64-central.zip", zip64_central.bytes);
    require(central_zip64_result.status == LocalLayoutStatus::invalid_structure
                && central_zip64_result.central_error
                && central_zip64_result.central_error->code
                    == CentralDirectoryErrorCode::missing_zip64_extra_field
                && central_zip64_result.central_error->offset
                    == zip64_central.centrals[0] + 20U,
            "central ZIP64 sentinel without an extra is rejected");
}

void test_ranges_and_layout(const TemporaryDirectory& temporary)
{
    const auto base = make_archive({entry("a", "12"), entry("b", "34")});

    auto duplicate = base;
    const std::size_t second_central = duplicate.centrals[1];
    set_u32(duplicate.bytes, second_central + 42U, 0U);
    duplicate.bytes[second_central + 46U] = std::byte{'a'};
    require_error(inspect(temporary, "duplicate-local.zip", duplicate.bytes),
                  LocalLayoutErrorCode::duplicate_local_header_reference,
                  second_central + 42U, "duplicate local reference");

    const auto overlapping_headers = make_overlapping_headers_archive();
    require_error(inspect(temporary, "overlapping-headers.zip",
                          overlapping_headers.bytes),
                  LocalLayoutErrorCode::local_structures_overlap, 30U,
                  "overlapping local structures");

    auto header_payload = base;
    set_u32(header_payload.bytes, 18U, 4U);
    set_u32(header_payload.bytes, 22U, 4U);
    set_u32(header_payload.bytes, header_payload.centrals[0] + 20U, 4U);
    set_u32(header_payload.bytes, header_payload.centrals[0] + 24U, 4U);
    require_error(inspect(temporary, "header-payload.zip", header_payload.bytes),
                  LocalLayoutErrorCode::header_payload_overlap,
                  base.locals[1], "header payload overlap");

    auto payload_overlap = base;
    set_u32(payload_overlap.bytes, 18U, 34U);
    set_u32(payload_overlap.bytes, 22U, 34U);
    set_u32(payload_overlap.bytes, payload_overlap.centrals[0] + 20U, 34U);
    set_u32(payload_overlap.bytes, payload_overlap.centrals[0] + 24U, 34U);
    require_error(inspect(temporary, "payload-overlap.zip",
                          payload_overlap.bytes),
                  LocalLayoutErrorCode::payloads_overlap,
                  base.locals[1] + 31U, "payload overlap");

    auto directory_overlap = make_archive({entry("a", "x")});
    set_u32(directory_overlap.bytes, 18U, 2U);
    set_u32(directory_overlap.bytes, 22U, 2U);
    set_u32(directory_overlap.bytes,
            directory_overlap.centrals[0] + 20U, 2U);
    set_u32(directory_overlap.bytes,
            directory_overlap.centrals[0] + 24U, 2U);
    require_error(inspect(temporary, "directory-overlap.zip",
                          directory_overlap.bytes),
                  LocalLayoutErrorCode::entry_intersects_central_directory,
                  31U, "payload directory overlap");

    auto eocd_overlap = make_archive({entry("a", "x")});
    const auto through_eocd = static_cast<std::uint32_t>(
        eocd_overlap.eocd_offset + 1U - 31U);
    set_u32(eocd_overlap.bytes, 18U, through_eocd);
    set_u32(eocd_overlap.bytes, 22U, through_eocd);
    set_u32(eocd_overlap.bytes, eocd_overlap.centrals[0] + 20U,
            through_eocd);
    set_u32(eocd_overlap.bytes, eocd_overlap.centrals[0] + 24U,
            through_eocd);
    require_error(inspect(temporary, "eocd-overlap.zip", eocd_overlap.bytes),
                  LocalLayoutErrorCode::entry_intersects_eocd, 31U,
                  "payload EOCD overlap");

    auto beyond_eof = make_archive({entry("a", "x")});
    set_u32(beyond_eof.bytes, 18U, UINT32_C(1000000));
    set_u32(beyond_eof.bytes, 22U, UINT32_C(1000000));
    set_u32(beyond_eof.bytes, beyond_eof.centrals[0] + 20U,
            UINT32_C(1000000));
    set_u32(beyond_eof.bytes, beyond_eof.centrals[0] + 24U,
            UINT32_C(1000000));
    require_error(inspect(temporary, "payload-eof.zip", beyond_eof.bytes),
                  LocalLayoutErrorCode::payload_truncated, 31U,
                  "payload beyond EOF");

    auto boundary = make_archive({entry("a", "x")});
    set_u32(boundary.bytes, 18U, UINT32_C(0xfffffffe));
    set_u32(boundary.bytes, 22U, UINT32_C(0xfffffffe));
    set_u32(boundary.bytes, boundary.centrals[0] + 20U,
            UINT32_C(0xfffffffe));
    set_u32(boundary.bytes, boundary.centrals[0] + 24U,
            UINT32_C(0xfffffffe));
    require_error(inspect(temporary, "integer-boundary.zip", boundary.bytes),
                  LocalLayoutErrorCode::payload_truncated, 31U,
                  "integer boundary without allocation");

    auto prefix = make_archive({entry("a")});
    prefix.bytes.insert(prefix.bytes.begin(), std::byte{0});
    prefix.locals[0] += 1U;
    prefix.central_offset += 1U;
    prefix.eocd_offset += 1U;
    set_u32(prefix.bytes, prefix.centrals[0] + 1U + 42U, 1U);
    set_u32(prefix.bytes, prefix.eocd_offset + 16U,
            static_cast<std::uint32_t>(prefix.central_offset));
    require_error(inspect(temporary, "prefix.zip", prefix.bytes),
                  LocalLayoutErrorCode::archive_prefix_prohibited, 0U,
                  "prefix");

    auto gap = make_archive({entry("a"), entry("b")});
    const std::size_t insertion = gap.locals[1];
    gap.bytes.insert(gap.bytes.begin() + static_cast<std::ptrdiff_t>(insertion),
                     std::byte{0});
    set_u32(gap.bytes, gap.centrals[1] + 1U + 42U,
            static_cast<std::uint32_t>(gap.locals[1] + 1U));
    set_u32(gap.bytes, gap.eocd_offset + 1U + 16U,
            static_cast<std::uint32_t>(gap.central_offset + 1U));
    require_error(inspect(temporary, "gap.zip", gap.bytes),
                  LocalLayoutErrorCode::archive_gap_prohibited,
                  gap.locals[1], "gap");
}

void test_frozen_fixtures(const std::filesystem::path& fixtures)
{
    const auto valid = aural::core::zip::inspect_classic_local_layout(
        fixtures / "valid-independent.aural");
    require(valid.profile_valid() && valid.layout->entries.size() == 2U,
            "frozen independent package has valid local layout");

    require_error(aural::core::zip::inspect_classic_local_layout(
                      fixtures / "invalid-malformed-local-header.aural"),
                  LocalLayoutErrorCode::invalid_local_header_signature, 0U,
                  "frozen malformed local header");

    const auto crc = aural::core::zip::inspect_classic_local_layout(
        fixtures / "invalid-local-central-crc.aural");
    require(crc.status == LocalLayoutStatus::invalid_structure
                && crc.error->code == LocalLayoutErrorCode::crc32_mismatch,
            "frozen local central CRC mismatch is rejected");

    require_unsupported(aural::core::zip::inspect_classic_local_layout(
                            fixtures / "invalid-compression.aural"),
                        UnsupportedArchiveFeature::compression_method,
                        "frozen compression fixture");
    const auto extra = aural::core::zip::inspect_classic_local_layout(
        fixtures / "invalid-extra-field.aural");
    require(extra.status == LocalLayoutStatus::invalid_structure
                && extra.central_error
                && extra.central_error->code
                    == CentralDirectoryErrorCode::prohibited_extra_field,
            "frozen prohibited central extra is rejected");

    const auto trailer = aural::core::zip::inspect_classic_local_layout(
        fixtures / "invalid-trailer.aural");
    require(trailer.status == LocalLayoutStatus::invalid_structure
                && trailer.central_error
                && trailer.central_error->code
                    == aural::core::zip::CentralDirectoryErrorCode::
                        eocd_not_terminal,
            "frozen trailer is rejected by the central prerequisite");
}

} // namespace

int main(const int argc, char** argv)
{
    require(argc == 2, "fixture directory argument required");
    const TemporaryDirectory temporary;
    const auto missing = aural::core::zip::inspect_classic_local_layout(
        temporary.path() / "missing.zip");
    require(missing.status == LocalLayoutStatus::file_failure
                && missing.file_error && !missing.layout,
            "native file errors remain structured");
    test_valid_layouts(temporary);
    test_header_and_field_failures(temporary);
    test_profile_features(temporary);
    test_ranges_and_layout(temporary);
    test_frozen_fixtures(argv[1]);
    std::cout << "CORE_ZIP_LOCAL_LAYOUT_TESTS_OK\n";
    return EXIT_SUCCESS;
}
