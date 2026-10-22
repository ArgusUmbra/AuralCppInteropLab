#include "../src/core/zip/classic_local_layout_internal.hpp"
#include "../src/core/zip/payload_crc32.hpp"

#include <aural/core/file_reader.hpp>

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

using aural::core::zip::ClassicArchivePhysicalLayout;
using aural::core::zip::ClassicEntryPhysicalLayout;
using aural::core::zip::Crc32IntegrityErrorCode;
using aural::core::zip::Crc32VerificationResult;
using aural::core::zip::Crc32VerificationStatus;

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
                / ("aural-payload-crc32-" + std::to_string(seed) + "-"
                   + std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) return;
        }
        throw std::runtime_error{"could not create CRC32 test directory"};
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

void set_u32(std::vector<std::byte>& output,
             const std::size_t offset,
             const std::uint32_t value)
{
    output[offset] = static_cast<std::byte>(value & 0xffU);
    output[offset + 1U] = static_cast<std::byte>((value >> 8U) & 0xffU);
    output[offset + 2U] = static_cast<std::byte>((value >> 16U) & 0xffU);
    output[offset + 3U] = static_cast<std::byte>((value >> 24U) & 0xffU);
}

void set_u16(std::vector<std::byte>& output,
             const std::size_t offset,
             const std::uint16_t value)
{
    output[offset] = static_cast<std::byte>(value & 0xffU);
    output[offset + 1U] = static_cast<std::byte>((value >> 8U) & 0xffU);
}

std::vector<std::byte> octets(const std::string_view text)
{
    std::vector<std::byte> output;
    output.reserve(text.size());
    for (const unsigned char value : text) {
        output.push_back(static_cast<std::byte>(value));
    }
    return output;
}

struct EntrySpec final {
    std::vector<std::byte> name;
    std::vector<std::byte> payload;
    std::uint32_t crc32;
    bool local_zip64{};
};

struct Archive final {
    std::vector<std::byte> bytes;
    std::vector<std::size_t> local_crc_offsets;
    std::vector<std::size_t> payload_offsets;
    std::vector<std::size_t> central_crc_offsets;
};

Archive make_archive(const std::vector<EntrySpec>& entries)
{
    Archive archive;
    std::vector<std::size_t> local_offsets;
    for (const auto& entry : entries) {
        local_offsets.push_back(archive.bytes.size());
        append_u32(archive.bytes, UINT32_C(0x04034b50));
        append_u16(archive.bytes, entry.local_zip64 ? 45U : 20U);
        append_u16(archive.bytes, 0U);
        append_u16(archive.bytes, 0U);
        append_u16(archive.bytes, 0U);
        append_u16(archive.bytes, 0U);
        archive.local_crc_offsets.push_back(archive.bytes.size());
        append_u32(archive.bytes, entry.crc32);
        append_u32(archive.bytes, entry.local_zip64
                                      ? UINT32_MAX
                                      : static_cast<std::uint32_t>(
                                            entry.payload.size()));
        append_u32(archive.bytes, entry.local_zip64
                                      ? UINT32_MAX
                                      : static_cast<std::uint32_t>(
                                            entry.payload.size()));
        append_u16(archive.bytes,
                   static_cast<std::uint16_t>(entry.name.size()));
        append_u16(archive.bytes, entry.local_zip64 ? 20U : 0U);
        archive.bytes.insert(
            archive.bytes.end(), entry.name.begin(), entry.name.end());
        if (entry.local_zip64) {
            append_u16(archive.bytes, 1U);
            append_u16(archive.bytes, 16U);
            append_u64(archive.bytes, entry.payload.size());
            append_u64(archive.bytes, entry.payload.size());
        }
        archive.payload_offsets.push_back(archive.bytes.size());
        archive.bytes.insert(
            archive.bytes.end(), entry.payload.begin(), entry.payload.end());
    }

    const std::size_t central_offset = archive.bytes.size();
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        append_u32(archive.bytes, UINT32_C(0x02014b50));
        append_u16(archive.bytes, 63U);
        append_u16(archive.bytes, entry.local_zip64 ? 45U : 20U);
        append_u16(archive.bytes, 0U);
        append_u16(archive.bytes, 0U);
        append_u16(archive.bytes, 0U);
        append_u16(archive.bytes, 0U);
        archive.central_crc_offsets.push_back(archive.bytes.size());
        append_u32(archive.bytes, entry.crc32);
        append_u32(
            archive.bytes, static_cast<std::uint32_t>(entry.payload.size()));
        append_u32(
            archive.bytes, static_cast<std::uint32_t>(entry.payload.size()));
        append_u16(archive.bytes,
                   static_cast<std::uint16_t>(entry.name.size()));
        append_u16(archive.bytes, 0U);
        append_u16(archive.bytes, 0U);
        append_u16(archive.bytes, 0U);
        append_u16(archive.bytes, 0U);
        append_u32(archive.bytes, 0U);
        append_u32(
            archive.bytes, static_cast<std::uint32_t>(local_offsets[index]));
        archive.bytes.insert(
            archive.bytes.end(), entry.name.begin(), entry.name.end());
    }

    const std::size_t eocd_offset = archive.bytes.size();
    append_u32(archive.bytes, UINT32_C(0x06054b50));
    append_u16(archive.bytes, 0U);
    append_u16(archive.bytes, 0U);
    append_u16(archive.bytes, static_cast<std::uint16_t>(entries.size()));
    append_u16(archive.bytes, static_cast<std::uint16_t>(entries.size()));
    append_u32(archive.bytes,
               static_cast<std::uint32_t>(eocd_offset - central_offset));
    append_u32(archive.bytes, static_cast<std::uint32_t>(central_offset));
    append_u16(archive.bytes, 0U);
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
    std::vector<std::byte> bytes;
    char value = 0;
    while (input.get(value)) {
        bytes.push_back(
            static_cast<std::byte>(static_cast<unsigned char>(value)));
    }
    return bytes;
}

Crc32VerificationResult verify_bytes(const TemporaryDirectory& temporary,
                                     const std::string_view name,
                                     const std::span<const std::byte> bytes)
{
    const auto path = temporary.path() / name;
    write_bytes(path, bytes);
    const auto before = read_bytes(path);
    auto result = aural::core::zip::verify_stored_payload_crc32(path);
    require(read_bytes(path) == before, "CRC32 verification modified input");
    return result;
}

void require_mismatch(const Crc32VerificationResult& result,
                      const std::uint32_t declared,
                      const std::uint32_t calculated,
                      const std::string_view context)
{
    require(result.status == Crc32VerificationStatus::crc32_mismatch,
            std::string{context} + ": status");
    require(result.integrity_error
                && result.integrity_error->code
                    == Crc32IntegrityErrorCode::mismatch,
            std::string{context} + ": typed error");
    require(result.integrity_error->declared_crc32 == declared
                && result.integrity_error->calculated_crc32 == calculated,
            std::string{context} + ": CRC values");
    require(result.entries.empty(),
            std::string{context} + ": no structural-success result");
}

void test_known_answers_and_empty(const TemporaryDirectory& temporary)
{
    const auto known = make_archive({EntrySpec{
        octets("known.bin"), octets("123456789"), UINT32_C(0xcbf43926)}});
    const auto known_result =
        verify_bytes(temporary, "known.zip", known.bytes);
    require(known_result.verified() && known_result.entries.size() == 1U,
            "standard CRC32 known answer");
    require(known_result.entries[0].calculated_crc32 == UINT32_C(0xcbf43926),
            "known answer value");

    const auto empty = make_archive(
        {EntrySpec{octets("empty.bin"), {}, UINT32_C(0x00000000)}});
    const auto empty_result =
        verify_bytes(temporary, "empty.zip", empty.bytes);
    require(empty_result.verified()
                && empty_result.entries[0].payload_size == 0U
                && empty_result.entries[0].calculated_crc32 == 0U,
            "empty payload CRC32");
}

void test_multiple_classic_and_zip64(const TemporaryDirectory& temporary)
{
    const auto multiple = make_archive({
        EntrySpec{octets("first.bin"), octets("123456789"),
                  UINT32_C(0xcbf43926)},
        EntrySpec{octets("second.bin"),
                  octets("The quick brown fox jumps over the lazy dog"),
                  UINT32_C(0x414fa339)}});
    const auto result = verify_bytes(temporary, "multiple.zip", multiple.bytes);
    require(result.verified() && result.entries.size() == 2U,
            "multiple classic stored payloads");
    require(result.entries[0].payload_offset == multiple.payload_offsets[0]
                && result.entries[1].payload_offset
                    == multiple.payload_offsets[1],
            "physical payload offsets retained");

    const auto zip64 = make_archive({EntrySpec{
        octets("zip64.bin"), octets("123456789"), UINT32_C(0xcbf43926), true}});
    const auto zip64_result =
        verify_bytes(temporary, "entry-zip64.zip", zip64.bytes);
    require(zip64_result.verified(), "resolved entry-level ZIP64 payload");
}

std::vector<std::byte> large_payload()
{
    std::vector<std::byte> payload(131123U);
    for (std::size_t index = 0; index < payload.size(); ++index) {
        payload[index] = static_cast<std::byte>((index * 37U + 11U) & 0xffU);
    }
    return payload;
}

void test_large_and_corruption(const TemporaryDirectory& temporary)
{
    const auto base = make_archive({EntrySpec{
        octets("large.bin"), large_payload(), UINT32_C(0x6818a21a)}});
    require(verify_bytes(temporary, "large.zip", base.bytes).verified(),
            "payload larger than one 64 KiB chunk");

    auto early = base;
    early.bytes[early.payload_offsets[0] + 7U] ^= std::byte{1U};
    const auto early_result = verify_bytes(temporary, "early.zip", early.bytes);
    require(early_result.status == Crc32VerificationStatus::crc32_mismatch,
            "early-chunk corruption mismatch");
    require(early_result.integrity_error
                && early_result.integrity_error->failure_offset
                    == early.payload_offsets[0] + large_payload().size(),
            "early mismatch reports payload end");

    auto late = base;
    late.bytes[late.payload_offsets[0] + 70000U] ^= std::byte{1U};
    require(verify_bytes(temporary, "late.zip", late.bytes).status
                == Crc32VerificationStatus::crc32_mismatch,
            "later-chunk corruption mismatch");

    auto metadata = base;
    set_u32(metadata.bytes, metadata.local_crc_offsets[0], UINT32_C(0x12345678));
    set_u32(
        metadata.bytes, metadata.central_crc_offsets[0], UINT32_C(0x12345678));
    require_mismatch(
        verify_bytes(temporary, "metadata.zip", metadata.bytes),
        UINT32_C(0x12345678),
        UINT32_C(0x6818a21a),
        "incorrect matching declarations");
}

void test_failures_boundaries_and_determinism(
    const TemporaryDirectory& temporary)
{
    auto modified = make_archive({EntrySpec{
        octets("payload.bin"), octets("123456789"), UINT32_C(0xcbf43926)}});
    modified.bytes[modified.payload_offsets[0] + 4U] ^= std::byte{1U};
    const auto path = temporary.path() / "repeat.zip";
    write_bytes(path, modified.bytes);
    const auto first = aural::core::zip::verify_stored_payload_crc32(path);
    const auto second = aural::core::zip::verify_stored_payload_crc32(path);
    require(first.status == second.status && first.integrity_error
                && second.integrity_error
                && first.integrity_error->code == second.integrity_error->code
                && first.integrity_error->failure_offset
                    == second.integrity_error->failure_offset
                && first.integrity_error->calculated_crc32
                    == second.integrity_error->calculated_crc32,
            "deterministic repeated mismatch");

    auto truncated = make_archive({EntrySpec{
        octets("short.bin"), octets("123456789"), UINT32_C(0xcbf43926)}});
    truncated.bytes.erase(truncated.bytes.begin()
                              + static_cast<std::ptrdiff_t>(
                                  truncated.payload_offsets[0] + 8U));
    require(verify_bytes(temporary, "truncated.zip", truncated.bytes).status
                == Crc32VerificationStatus::invalid_prerequisite_structure,
            "truncated payload rejected before integrity success");

    const auto missing = aural::core::zip::verify_stored_payload_crc32(
        temporary.path() / "missing.zip");
    require(missing.status == Crc32VerificationStatus::payload_read_failure
                && missing.file_error.has_value(),
            "unreadable archive is a file failure");

    auto unsupported = make_archive({EntrySpec{
        octets("compressed.bin"), octets("123456789"),
        UINT32_C(0xcbf43926)}});
    set_u16(unsupported.bytes, 8U, 8U);
    set_u16(unsupported.bytes, unsupported.central_crc_offsets[0] - 6U, 8U);
    require(verify_bytes(temporary, "unsupported.zip", unsupported.bytes)
                .status == Crc32VerificationStatus::unsupported_operation,
            "unsupported compression is not integrity success");

    const auto valid = make_archive({EntrySpec{
        octets("range.bin"), octets("123456789"), UINT32_C(0xcbf43926)}});
    const auto range_path = temporary.path() / "ranges.zip";
    write_bytes(range_path, valid.bytes);
    auto opened = aural::core::FileReader::open(range_path);
    require(static_cast<bool>(opened), "open range fixture");
    auto inspected = aural::core::zip::inspect_classic_local_layout_open(
        opened.value());
    require(inspected.profile_valid(), "inspect range fixture");

    ClassicArchivePhysicalLayout overflow = *inspected.layout;
    overflow.entries[0].payload_offset =
        std::numeric_limits<std::uint64_t>::max() - 3U;
    overflow.entries[0].payload_size = 9U;
    const auto overflow_result =
        aural::core::zip::verify_stored_payload_crc32_open(
            opened.value(), overflow);
    require(overflow_result.status
                == Crc32VerificationStatus::invalid_prerequisite_structure
                && overflow_result.integrity_error
                && overflow_result.integrity_error->code
                    == Crc32IntegrityErrorCode::payload_range_overflow,
            "checked payload offset overflow");

    ClassicArchivePhysicalLayout remaining = *inspected.layout;
    remaining.entries[0].payload_size =
        opened.value().size() - remaining.entries[0].payload_offset + 1U;
    remaining.entries[0].payload_end = opened.value().size() + 1U;
    const auto remaining_result =
        aural::core::zip::verify_stored_payload_crc32_open(
            opened.value(), remaining);
    require(remaining_result.status
                == Crc32VerificationStatus::invalid_prerequisite_structure
                && remaining_result.integrity_error
                && remaining_result.integrity_error->code
                    == Crc32IntegrityErrorCode::payload_range_out_of_bounds,
            "remaining-length file boundary");
}

void test_frozen_fixtures(const std::filesystem::path& fixtures)
{
    for (const auto name : {"valid-minimal.aural",
                            "valid-independent.aural",
                            "valid-required-zip64-end.aural"}) {
        const auto result = aural::core::zip::verify_stored_payload_crc32(
            fixtures / name);
        require(result.verified(), std::string{"frozen CRC32 fixture: "} + name);
    }
    const auto disagreement =
        aural::core::zip::verify_stored_payload_crc32(
            fixtures / "invalid-local-central-crc.aural");
    require(disagreement.status
                == Crc32VerificationStatus::invalid_prerequisite_structure,
            "frozen local/central CRC disagreement remains structural failure");
}

} // namespace

int main(const int argc, char** argv)
{
    try {
        require(argc == 2, "expected frozen fixture directory");
        const TemporaryDirectory temporary;
        test_known_answers_and_empty(temporary);
        test_multiple_classic_and_zip64(temporary);
        test_large_and_corruption(temporary);
        test_failures_boundaries_and_determinism(temporary);
        test_frozen_fixtures(argv[1]);
        require(!aural::core::zip::crc32_integrity_error_message(
                     Crc32IntegrityErrorCode::mismatch)
                     .empty(),
                "typed CRC32 error message");
        std::cout << "CORE_ZIP_PAYLOAD_CRC32_TESTS_OK\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
