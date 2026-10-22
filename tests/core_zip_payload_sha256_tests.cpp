#include "../src/core/zip/classic_local_layout_internal.hpp"
#include "../src/core/zip/payload_crc32.hpp"
#include "../src/core/zip/payload_sha256.hpp"

#include <aural/core/file_reader.hpp>

#include <zlib.h>

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
using aural::core::zip::Sha256CalculationResult;
using aural::core::zip::Sha256CalculationStatus;
using aural::core::zip::Sha256IntegrityErrorCode;

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
                / ("aural-payload-sha256-" + std::to_string(seed) + "-"
                   + std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) return;
        }
        throw std::runtime_error{"could not create SHA-256 test directory"};
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

std::vector<std::byte> octets(const std::string_view text)
{
    std::vector<std::byte> output;
    output.reserve(text.size());
    for (const unsigned char value : text) {
        output.push_back(static_cast<std::byte>(value));
    }
    return output;
}

std::uint32_t payload_crc32(const std::span<const std::byte> payload)
{
    uLong crc = crc32(0L, Z_NULL, 0U);
    std::size_t position = 0U;
    while (position < payload.size()) {
        const auto length = static_cast<uInt>(std::min<std::size_t>(
            payload.size() - position,
            std::numeric_limits<uInt>::max()));
        crc = crc32(
            crc,
            reinterpret_cast<const Bytef*>(payload.data() + position),
            length);
        position += length;
    }
    return static_cast<std::uint32_t>(crc);
}

struct EntrySpec final {
    std::vector<std::byte> name;
    std::vector<std::byte> payload;
    bool local_zip64{};
};

struct Archive final {
    std::vector<std::byte> bytes;
    std::vector<std::size_t> local_crc_offsets;
    std::vector<std::size_t> central_crc_offsets;
    std::vector<std::size_t> payload_offsets;
};

Archive make_archive(const std::vector<EntrySpec>& entries)
{
    Archive archive;
    std::vector<std::size_t> local_offsets;
    std::vector<std::uint32_t> crcs;
    for (const auto& entry : entries) {
        const auto crc = payload_crc32(entry.payload);
        crcs.push_back(crc);
        local_offsets.push_back(archive.bytes.size());
        append_u32(archive.bytes, UINT32_C(0x04034b50));
        append_u16(archive.bytes, entry.local_zip64 ? 45U : 20U);
        append_u16(archive.bytes, 0U);
        append_u16(archive.bytes, 0U);
        append_u16(archive.bytes, 0U);
        append_u16(archive.bytes, 0U);
        archive.local_crc_offsets.push_back(archive.bytes.size());
        append_u32(archive.bytes, crc);
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
        append_u32(archive.bytes, crcs[index]);
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

Sha256CalculationResult calculate_bytes(
    const TemporaryDirectory& temporary,
    const std::string_view name,
    const std::span<const std::byte> bytes)
{
    const auto path = temporary.path() / name;
    write_bytes(path, bytes);
    const auto before = read_bytes(path);
    auto result = aural::core::zip::calculate_stored_payload_sha256(path);
    require(read_bytes(path) == before, "SHA-256 calculation modified input");
    return result;
}

std::string only_hex(const Sha256CalculationResult& result)
{
    require(result.calculated() && result.entries.size() == 1U,
            "single digest result");
    return aural::core::zip::sha256_lowercase_hex(result.entries[0].digest);
}

std::vector<std::byte> repeated(
    const std::size_t count,
    const std::byte value)
{
    return std::vector<std::byte>(count, value);
}

std::vector<std::byte> patterned(const std::size_t count)
{
    std::vector<std::byte> bytes(count);
    for (std::size_t index = 0; index < count; ++index) {
        bytes[index] = static_cast<std::byte>((index * 37U + 11U) & 0xffU);
    }
    return bytes;
}

void test_independent_known_answers(const TemporaryDirectory& temporary)
{
    const std::vector<std::byte> binary{
        std::byte{0x00}, std::byte{0x01}, std::byte{0x02}, std::byte{0x7f},
        std::byte{0x80}, std::byte{0xfe}, std::byte{0xff}};
    const auto cases = std::vector<std::pair<EntrySpec, std::string_view>>{
        {{octets("empty.bin"), {}},
         "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {{octets("abc.bin"), octets("abc")},
         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {{octets("million-a.bin"), repeated(1000000U, std::byte{'a'})},
         "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"},
        {{octets("binary.bin"), binary},
         "7bb6463b30f9e301fed333cdf8960ca9497b602ccd8eeb46ae42693fdea15a4d"},
        {{octets("multichunk.bin"), patterned(131123U)},
         "6360e8b7a2b7b59cd68e71568252685db75c860639ad0d07f1efd78795a99a2a"}};

    std::size_t index = 0U;
    for (const auto& [entry, expected] : cases) {
        const auto archive = make_archive({entry});
        const auto result = calculate_bytes(
            temporary, "known-" + std::to_string(index++) + ".zip",
            archive.bytes);
        require(only_hex(result) == expected, "independent SHA-256 known answer");
    }
}

void test_layouts_boundaries_and_repeatability(
    const TemporaryDirectory& temporary)
{
    const auto multiple = make_archive({
        {octets("first.bin"), octets("abc")},
        {octets("second.bin"), patterned(65537U)}});
    const auto first = calculate_bytes(temporary, "multiple.zip", multiple.bytes);
    const auto second = calculate_bytes(temporary, "repeat.zip", multiple.bytes);
    require(first.calculated() && first.entries.size() == 2U,
            "multiple classic entries");
    require(first.entries[0].file_name == octets("first.bin")
                && first.entries[0].payload_offset
                    == multiple.payload_offsets[0]
                && first.entries[1].payload_end
                    == multiple.payload_offsets[1] + 65537U,
            "entry identity and physical ranges retained");
    require(first.entries[0].digest == second.entries[0].digest
                && first.entries[1].digest == second.entries[1].digest,
            "repeatable digest values");

    const auto zip64 = make_archive(
        {{octets("zip64.bin"), octets("abc"), true}});
    require(calculate_bytes(temporary, "entry-zip64.zip", zip64.bytes)
                .calculated(),
            "entry-level ZIP64 payload");

    for (const std::size_t size : {65535U, 65536U, 65537U, 131072U}) {
        const auto boundary = make_archive(
            {{octets("boundary.bin"), patterned(size)}});
        const auto result = calculate_bytes(
            temporary, "boundary-" + std::to_string(size) + ".zip",
            boundary.bytes);
        require(result.calculated() && result.entries[0].payload_size == size,
                "read chunk boundary");
    }
}

void test_typed_failures_and_crc_regression(
    const TemporaryDirectory& temporary)
{
    auto mismatch = make_archive({{octets("mismatch.bin"), octets("abc")}});
    set_u32(mismatch.bytes, mismatch.local_crc_offsets[0], UINT32_C(0x12345678));
    set_u32(
        mismatch.bytes, mismatch.central_crc_offsets[0], UINT32_C(0x12345678));
    const auto mismatch_result =
        calculate_bytes(temporary, "crc-mismatch.zip", mismatch.bytes);
    require(mismatch_result.status
                == Sha256CalculationStatus::crc32_prerequisite_failure
                && mismatch_result.integrity_error
                && mismatch_result.integrity_error->code
                    == Sha256IntegrityErrorCode::crc32_mismatch
                && mismatch_result.entries.empty(),
            "typed CRC32 prerequisite failure");

    const auto valid = make_archive({{octets("range.bin"), octets("abc")}});
    const auto path = temporary.path() / "ranges.zip";
    write_bytes(path, valid.bytes);
    require(aural::core::zip::verify_stored_payload_crc32(path).verified(),
            "existing CRC32 success regression");
    auto opened = aural::core::FileReader::open(path);
    require(static_cast<bool>(opened), "open range fixture");
    auto inspected = aural::core::zip::inspect_classic_local_layout_open(
        opened.value());
    require(inspected.profile_valid(), "inspect range fixture");

    ClassicArchivePhysicalLayout overflow = *inspected.layout;
    overflow.entries[0].payload_offset =
        std::numeric_limits<std::uint64_t>::max() - 3U;
    overflow.entries[0].payload_size = 9U;
    const auto overflow_result =
        aural::core::zip::calculate_stored_payload_sha256_open(
            opened.value(), overflow);
    require(overflow_result.status
                == Sha256CalculationStatus::invalid_prerequisite_structure
                && overflow_result.integrity_error
                && overflow_result.integrity_error->code
                    == Sha256IntegrityErrorCode::payload_range_overflow,
            "checked hostile payload offset overflow");

    ClassicArchivePhysicalLayout outside = *inspected.layout;
    outside.entries[0].payload_size =
        opened.value().size() - outside.entries[0].payload_offset + 1U;
    outside.entries[0].payload_end = opened.value().size() + 1U;
    const auto outside_result =
        aural::core::zip::calculate_stored_payload_sha256_open(
            opened.value(), outside);
    require(outside_result.status
                == Sha256CalculationStatus::invalid_prerequisite_structure
                && outside_result.integrity_error
                && outside_result.integrity_error->code
                    == Sha256IntegrityErrorCode::payload_range_out_of_bounds,
            "hostile remaining length out of bounds");

    ClassicArchivePhysicalLayout wrong_size = *inspected.layout;
    ++wrong_size.file_size;
    require(aural::core::zip::calculate_stored_payload_sha256_open(
                opened.value(), wrong_size).integrity_error->code
                == Sha256IntegrityErrorCode::layout_file_size_mismatch,
            "layout and handle size mismatch");

    auto truncated = valid;
    truncated.bytes.erase(truncated.bytes.begin()
                          + static_cast<std::ptrdiff_t>(
                              truncated.payload_offsets[0] + 2U));
    require(calculate_bytes(temporary, "truncated.zip", truncated.bytes).status
                == Sha256CalculationStatus::invalid_prerequisite_structure,
            "truncated payload rejected structurally");

    const auto missing = aural::core::zip::calculate_stored_payload_sha256(
        temporary.path() / "missing.zip");
    require(missing.status == Sha256CalculationStatus::payload_read_failure
                && missing.file_error.has_value(),
            "unreadable archive typed read failure");
}

void test_frozen_fixtures(const std::filesystem::path& fixtures)
{
    for (const auto name : {"valid-minimal.aural",
                            "valid-independent.aural",
                            "valid-required-zip64-end.aural"}) {
        const auto result =
            aural::core::zip::calculate_stored_payload_sha256(fixtures / name);
        require(result.calculated(), std::string{"frozen SHA fixture: "} + name);
    }
    const auto disagreement =
        aural::core::zip::calculate_stored_payload_sha256(
            fixtures / "invalid-local-central-crc.aural");
    require(disagreement.status
                == Sha256CalculationStatus::invalid_prerequisite_structure,
            "frozen local/central CRC disagreement remains structural failure");
}

} // namespace

int main(const int argc, char** argv)
{
    try {
        require(argc == 2, "expected frozen fixture directory");
        const TemporaryDirectory temporary;
        test_independent_known_answers(temporary);
        test_layouts_boundaries_and_repeatability(temporary);
        test_typed_failures_and_crc_regression(temporary);
        test_frozen_fixtures(argv[1]);
        require(!aural::core::zip::sha256_integrity_error_message(
                     Sha256IntegrityErrorCode::crc32_mismatch).empty(),
                "typed integrity error message");
        require(!aural::core::zip::sha256_provider_error_message(
                     aural::core::zip::Sha256ProviderErrorCode::update_failed)
                     .empty(),
                "typed provider error message");
        std::cout << "CORE_ZIP_PAYLOAD_SHA256_TESTS_OK\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
