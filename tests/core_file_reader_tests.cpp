#include <aural/core/file_reader.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <charconv>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

using aural::core::FileErrorCode;
using aural::core::FileReader;

int failures = 0;

void expect(const bool condition, const char* const message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
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
                / ("aural-reader-tests-" + std::to_string(seed) + "-"
                   + std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) {
                return;
            }
        }
        throw std::runtime_error{"could not create a temporary test directory"};
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void write_bytes(
    const std::filesystem::path& path,
    const std::span<const std::byte> bytes)
{
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output) {
        throw std::runtime_error{"could not create a test file"};
    }
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        throw std::runtime_error{"could not write a test file"};
    }
}

[[nodiscard]] std::vector<std::byte> load_bytes(
    const std::filesystem::path& path)
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

[[nodiscard]] bool equals(
    const std::span<const std::byte> left,
    const std::span<const std::byte> right)
{
    return left.size() == right.size()
        && std::equal(left.begin(), left.end(), right.begin());
}

#if defined(__linux__)
[[nodiscard]] int find_descriptor_for(const std::filesystem::path& path)
{
    struct stat expected {};
    if (::stat(path.c_str(), &expected) != 0) {
        return -1;
    }

    for (const auto& entry : std::filesystem::directory_iterator{"/proc/self/fd"}) {
        const std::string name = entry.path().filename().string();
        int descriptor = -1;
        const auto parsed = std::from_chars(
            name.data(), name.data() + name.size(), descriptor);
        if (parsed.ec != std::errc{} || parsed.ptr != name.data() + name.size()) {
            continue;
        }

        struct stat candidate {};
        if (::fstat(descriptor, &candidate) == 0
            && candidate.st_dev == expected.st_dev
            && candidate.st_ino == expected.st_ino) {
            return descriptor;
        }
    }
    return -1;
}
#endif

} // namespace

int main()
{
    static_assert(!std::is_copy_constructible_v<FileReader>);
    static_assert(!std::is_copy_assignable_v<FileReader>);
    static_assert(std::is_nothrow_move_constructible_v<FileReader>);
    static_assert(std::is_nothrow_move_assignable_v<FileReader>);

    const TemporaryDirectory temporary;
    const auto file_path = temporary.path() / "known.bin";
    constexpr std::array known{
        std::byte{0x10},
        std::byte{0x20},
        std::byte{0x30},
        std::byte{0x40},
        std::byte{0x50}};
    write_bytes(file_path, known);
    const auto original_time = std::filesystem::last_write_time(file_path);

    auto opened = FileReader::open(file_path);
    expect(opened.has_value(), "open a valid temporary file");
    if (!opened) {
        return 1;
    }
    FileReader reader = std::move(opened).value();
    expect(reader.is_open(), "successful reader owns a handle");
    expect(reader.size() == known.size(), "actual file length is retained");

#if defined(__linux__)
    const int descriptor = find_descriptor_for(file_path);
    expect(descriptor >= 0, "Linux reader owns a descriptor for the opened file");
    if (descriptor >= 0) {
        const int status_flags = ::fcntl(descriptor, F_GETFL);
        const int descriptor_flags = ::fcntl(descriptor, F_GETFD);
        expect(status_flags >= 0 && (status_flags & O_ACCMODE) == O_RDONLY,
               "Linux descriptor is opened read-only");
        expect(descriptor_flags >= 0 && (descriptor_flags & FD_CLOEXEC) != 0,
               "Linux descriptor is close-on-exec");
    }
#endif

    std::array<std::byte, 3> middle{};
    const auto middle_read = reader.read_at(1, middle);
    const std::array expected_middle{
        std::byte{0x20}, std::byte{0x30}, std::byte{0x40}};
    expect(middle_read && middle_read.value() == middle.size(),
           "known sequence read returns exact byte count");
    expect(equals(middle, expected_middle), "known sequence bytes match");

    std::array<std::byte, 2> beginning{};
    expect(reader.read_at(0, beginning).has_value(), "read from offset zero");
    const std::array expected_beginning{std::byte{0x10}, std::byte{0x20}};
    expect(equals(beginning, expected_beginning), "offset-zero bytes match");

    std::array<std::byte, 1> final_byte{};
    expect(reader.read_at(known.size() - 1U, final_byte).has_value(),
           "read final byte");
    expect(final_byte[0] == known.back(), "final byte matches");

    std::span<std::byte> empty_destination;
    const auto empty_at_eof = reader.read_at(known.size(), empty_destination);
    expect(empty_at_eof && empty_at_eof.value() == 0,
           "zero-byte read at EOF succeeds");

    std::array<std::byte, 1> one_byte{};
    const auto past_end = reader.read_at(known.size(), one_byte);
    expect(!past_end && past_end.error().code == FileErrorCode::read_out_of_bounds,
           "one-byte read beyond EOF is rejected");

    const auto overflow =
        reader.read_at(std::numeric_limits<std::uint64_t>::max(), one_byte);
    expect(!overflow && overflow.error().code == FileErrorCode::arithmetic_overflow,
           "offset plus length overflow is rejected");

    const auto impossible = reader.read_at(
        std::numeric_limits<std::uint64_t>::max(),
        empty_destination);
    expect(!impossible
               && impossible.error().code
                   == FileErrorCode::invalid_offset_or_length,
           "native-impossible offset is rejected");

    constexpr std::uint64_t two_to_32 = UINT64_C(1) << 32U;
    constexpr std::uint64_t two_to_63 = UINT64_C(1) << 63U;
    const auto near_32 = reader.read_at(two_to_32, empty_destination);
    expect(!near_32 && near_32.error().code == FileErrorCode::read_out_of_bounds,
           "boundary near 2^32 is checked without a large file");
    const auto near_63 = reader.read_at(two_to_63, empty_destination);
    expect(!near_63
               && near_63.error().code
                   == FileErrorCode::invalid_offset_or_length,
           "boundary at 2^63 is rejected before native conversion");

    std::array<std::byte, 2> repeated_a{};
    std::array<std::byte, 2> repeated_b{};
    expect(reader.read_at(3, repeated_a).has_value(), "later repeated read");
    expect(reader.read_at(1, repeated_b).has_value(), "earlier repeated read");
    expect(repeated_a[0] == std::byte{0x40}
               && repeated_a[1] == std::byte{0x50}
               && repeated_b[0] == std::byte{0x20}
               && repeated_b[1] == std::byte{0x30},
           "positional reads do not depend on previous offsets");

    FileReader moved = std::move(reader);
    expect(moved.is_open() && !reader.is_open(), "move transfers handle ownership");
    const auto moved_from_read = reader.read_at(0, one_byte);
    expect(!moved_from_read
               && moved_from_read.error().code == FileErrorCode::reader_not_open,
           "moved-from reader fails safely");
    expect(moved.read_at(0, one_byte).has_value(), "moved-to reader remains usable");

    auto assignment_open = FileReader::open(file_path);
    expect(assignment_open.has_value(), "open move-assignment destination");
    if (!assignment_open) {
        return 1;
    }
    FileReader assigned = std::move(assignment_open).value();
    assigned = std::move(moved);
    expect(assigned.is_open() && !moved.is_open(),
           "move assignment transfers handle ownership");
    expect(assigned.read_at(0, one_byte).has_value(),
           "move-assigned reader remains usable");

    const auto missing_path = temporary.path() / "missing.bin";
    const auto missing = FileReader::open(missing_path);
    expect(!missing && missing.error().code == FileErrorCode::file_not_found,
           "missing file returns a structured error");
    expect(!missing.error().description().empty(), "errors have descriptions");
    write_bytes(missing_path, std::span<const std::byte>{});
    expect(std::filesystem::exists(missing_path),
           "failed open leaves no resource preventing file creation");

    const auto empty_file_path = temporary.path() / "empty.bin";
    write_bytes(empty_file_path, std::span<const std::byte>{});
    auto empty_open = FileReader::open(empty_file_path);
    expect(empty_open && empty_open.value().size() == 0, "empty file opens");
    if (empty_open) {
        expect(empty_open.value().read_at(0, empty_destination).has_value(),
               "empty file permits a zero-byte EOF read");
        const auto empty_overread = empty_open.value().read_at(0, one_byte);
        expect(!empty_overread
                   && empty_overread.error().code
                       == FileErrorCode::read_out_of_bounds,
               "empty file rejects a one-byte read");
    }

    const auto directory_open = FileReader::open(temporary.path());
    expect(!directory_open
               && directory_open.error().code == FileErrorCode::not_regular_file,
           "non-regular input is rejected");

#ifdef _WIN32
    const auto unicode_path = temporary.path()
        / std::filesystem::path{L"\u97F3\u58F0-\u8AAD\u53D6.bin"};
#else
    const auto unicode_path = temporary.path()
        / std::filesystem::path{u8"\u97F3\u58F0-\u8AAD\u53D6.bin"};
#endif
    write_bytes(unicode_path, known);
    auto unicode_open = FileReader::open(unicode_path);
    expect(unicode_open.has_value(), "native path supports the platform encoding");
    if (unicode_open) {
        expect(unicode_open.value().read_at(0, one_byte).has_value(),
               "file opened through native path is readable");
    }

    const auto truncation_path = temporary.path() / "truncation.bin";
    write_bytes(truncation_path, known);
    auto truncation_open = FileReader::open(truncation_path);
    expect(truncation_open.has_value(), "open truncation test file");
    if (truncation_open) {
        std::error_code resize_error;
        std::filesystem::resize_file(truncation_path, 1, resize_error);
#ifdef _WIN32
        expect(static_cast<bool>(resize_error),
               "Windows sharing policy prevents truncation while open");
        expect(truncation_open.value().read_at(0, one_byte).has_value(),
               "reader remains valid after denied truncation");
#else
        expect(!resize_error, "POSIX test truncates through a second handle");
        const auto truncated = truncation_open.value().read_at(0, one_byte);
        expect(!truncated
                   && truncated.error().code == FileErrorCode::file_changed,
               "reader rejects a detectably truncated file");
#endif
    }

    expect(std::filesystem::last_write_time(file_path) == original_time,
           "reads do not change input modification time");
    expect(equals(load_bytes(file_path), known), "input file bytes are unchanged");

    // Huge hostile offsets exercise validation using only this one-byte buffer;
    // FileReader has no API that allocates from an untrusted requested length.
    const auto hostile = assigned.read_at(two_to_63 - 1U, one_byte);
    expect(!hostile, "hostile range is rejected without a large allocation");

    const auto cleanup_path = temporary.path() / "cleanup.bin";
    write_bytes(cleanup_path, known);
    {
        auto cleanup_open = FileReader::open(cleanup_path);
        expect(cleanup_open.has_value(), "open cleanup test handle");
        if (cleanup_open) {
            const auto failure = cleanup_open.value().read_at(known.size(), one_byte);
            expect(!failure, "read failure path is exercised before cleanup");
        }
    }
    std::error_code cleanup_error;
    const bool cleanup_removed =
        std::filesystem::remove(cleanup_path, cleanup_error);
    expect(cleanup_removed && !cleanup_error,
           "handle is released after a reader leaves scope");

    if (failures != 0) {
        return 1;
    }
    std::cout << "CORE_FILE_READER_TESTS_OK\n";
    return 0;
}
