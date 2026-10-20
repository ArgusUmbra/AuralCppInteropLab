#ifndef AURAL_CORE_FILE_READER_HPP
#define AURAL_CORE_FILE_READER_HPP

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace aural::core {

// This API is experimental. Source and ABI compatibility are not guaranteed.
enum class FileErrorCode {
    file_not_found,
    open_failed,
    not_regular_file,
    metadata_failed,
    invalid_offset_or_length,
    arithmetic_overflow,
    read_out_of_bounds,
    unexpected_truncation,
    read_failed,
    file_changed,
    reader_not_open
};

enum class FileOperation {
    open,
    inspect,
    read
};

enum class NativeErrorDomain {
    none,
    windows,
    posix
};

// A native code is diagnostic only. Callers should branch on FileErrorCode,
// never on platform-specific values.
struct NativeFileError final {
    NativeErrorDomain domain{NativeErrorDomain::none};
    std::uint64_t code{0};
};

struct FileError final {
    FileErrorCode code{FileErrorCode::open_failed};
    FileOperation operation{FileOperation::open};
    NativeFileError native{};

    [[nodiscard]] std::string_view description() const noexcept;
};

// Minimal C++20 result type used for expected file and validation failures.
// Check operator bool() before calling value() or error().
template <typename T>
class FileResult final {
public:
    [[nodiscard]] static FileResult success(T value)
    {
        return FileResult{SuccessTag{}, std::move(value)};
    }

    [[nodiscard]] static FileResult failure(const FileError error)
    {
        return FileResult{FailureTag{}, error};
    }

    [[nodiscard]] bool has_value() const noexcept
    {
        return value_.has_value();
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return has_value();
    }

    [[nodiscard]] T& value() & noexcept
    {
        assert(has_value());
        return *value_;
    }

    [[nodiscard]] const T& value() const& noexcept
    {
        assert(has_value());
        return *value_;
    }

    [[nodiscard]] T&& value() && noexcept
    {
        assert(has_value());
        return std::move(*value_);
    }

    [[nodiscard]] const FileError& error() const noexcept
    {
        assert(!has_value());
        return error_;
    }

private:
    struct SuccessTag final {};
    struct FailureTag final {};

    explicit FileResult(SuccessTag, T value)
        : value_{std::move(value)}
    {
    }

    explicit FileResult(FailureTag, const FileError error) noexcept
        : error_{error}
    {
    }

    std::optional<T> value_;
    FileError error_{};
};

// Owns one read-only regular-file handle. FileReader is movable, not copyable.
// Reads are exact: success means the entire destination span was filled.
class FileReader final {
public:
    [[nodiscard]] static FileResult<FileReader> open(
        const std::filesystem::path& path);

    ~FileReader();

    FileReader(FileReader&& other) noexcept;
    FileReader& operator=(FileReader&& other) noexcept;

    FileReader(const FileReader&) = delete;
    FileReader& operator=(const FileReader&) = delete;

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] std::uint64_t size() const noexcept;

    // Reads destination.size() bytes starting at offset without allocating a
    // buffer. Returns destination.size() on success, including zero at EOF.
    [[nodiscard]] FileResult<std::size_t> read_at(
        std::uint64_t offset,
        std::span<std::byte> destination) const;

private:
    class Impl;

    explicit FileReader(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace aural::core

#endif
