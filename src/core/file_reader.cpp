#include <aural/core/file_reader.hpp>

#include <aural/core/range.hpp>

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <mutex>
#else
#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace aural::core {
namespace {

[[nodiscard]] FileError make_error(
    const FileErrorCode code,
    const FileOperation operation,
    const NativeErrorDomain domain = NativeErrorDomain::none,
    const std::uint64_t native_code = 0) noexcept
{
    return FileError{code, operation, NativeFileError{domain, native_code}};
}

#ifdef _WIN32

struct FileMetadata final {
    std::uint64_t size{};
    DWORD volume_serial{};
    DWORD file_index_high{};
    DWORD file_index_low{};
    FILETIME last_write_time{};
};

[[nodiscard]] bool same_metadata(
    const FileMetadata& left,
    const FileMetadata& right) noexcept
{
    return left.size == right.size
        && left.volume_serial == right.volume_serial
        && left.file_index_high == right.file_index_high
        && left.file_index_low == right.file_index_low
        && left.last_write_time.dwHighDateTime == right.last_write_time.dwHighDateTime
        && left.last_write_time.dwLowDateTime == right.last_write_time.dwLowDateTime;
}

[[nodiscard]] FileErrorCode open_error_code(const DWORD error) noexcept
{
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
        return FileErrorCode::file_not_found;
    }
    return FileErrorCode::open_failed;
}

#else

struct FileMetadata final {
    std::uint64_t size{};
    dev_t device{};
    ino_t inode{};
    mode_t mode{};
    timespec modified{};
    timespec changed{};
};

[[nodiscard]] bool same_time(const timespec left, const timespec right) noexcept
{
    return left.tv_sec == right.tv_sec && left.tv_nsec == right.tv_nsec;
}

[[nodiscard]] bool same_metadata(
    const FileMetadata& left,
    const FileMetadata& right) noexcept
{
    return left.size == right.size
        && left.device == right.device
        && left.inode == right.inode
        && left.mode == right.mode
        && same_time(left.modified, right.modified)
        && same_time(left.changed, right.changed);
}

[[nodiscard]] FileErrorCode open_error_code(const int error) noexcept
{
    if (error == ENOENT || error == ENOTDIR) {
        return FileErrorCode::file_not_found;
    }
    return FileErrorCode::open_failed;
}

#endif

} // namespace

class FileReader::Impl final {
public:
    Impl() = default;
    ~Impl();

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    [[nodiscard]] FileResult<std::size_t> read(
        std::uint64_t offset,
        std::span<std::byte> destination) const;

    [[nodiscard]] FileResult<std::size_t> validate_read(
        std::uint64_t offset,
        std::span<std::byte> destination) const;

    [[nodiscard]] bool read_metadata(
        FileMetadata& metadata,
        FileError& error,
        FileOperation operation) const noexcept;

    FileMetadata opened_metadata{};

#ifdef _WIN32
    HANDLE handle{INVALID_HANDLE_VALUE};
    // SetFilePointerEx + ReadFile is positional only while this lock is held.
    // Concurrent reads on one FileReader are therefore safe but serialized.
    mutable std::mutex position_mutex;
#else
    int descriptor{-1};
#endif
};

std::string_view FileError::description() const noexcept
{
    switch (code) {
    case FileErrorCode::file_not_found:
        return "file was not found";
    case FileErrorCode::open_failed:
        return "file could not be opened for reading";
    case FileErrorCode::not_regular_file:
        return "path does not identify a regular file";
    case FileErrorCode::metadata_failed:
        return "file metadata could not be inspected";
    case FileErrorCode::invalid_offset_or_length:
        return "offset or length is not supported by the native file API";
    case FileErrorCode::arithmetic_overflow:
        return "offset plus length overflows the 64-bit range";
    case FileErrorCode::read_out_of_bounds:
        return "requested range is outside the opened file boundary";
    case FileErrorCode::unexpected_truncation:
        return "the native read ended before the requested bytes were returned";
    case FileErrorCode::read_failed:
        return "the operating system could not complete the read";
    case FileErrorCode::file_changed:
        return "file identity, size, or modification metadata changed after open";
    case FileErrorCode::reader_not_open:
        return "file reader does not own an open handle";
    }
    return "unknown file error";
}

FileReader::FileReader(std::unique_ptr<Impl> impl) noexcept
    : impl_{std::move(impl)}
{
}

FileReader::~FileReader() = default;
FileReader::FileReader(FileReader&& other) noexcept = default;
FileReader& FileReader::operator=(FileReader&& other) noexcept = default;

bool FileReader::is_open() const noexcept
{
    return impl_ != nullptr;
}

std::uint64_t FileReader::size() const noexcept
{
    return impl_ == nullptr ? 0 : impl_->opened_metadata.size;
}

FileResult<std::size_t> FileReader::read_at(
    const std::uint64_t offset,
    const std::span<std::byte> destination) const
{
    if (impl_ == nullptr) {
        return FileResult<std::size_t>::failure(
            make_error(FileErrorCode::reader_not_open, FileOperation::read));
    }
    return impl_->read(offset, destination);
}

FileResult<std::size_t> FileReader::Impl::validate_read(
    const std::uint64_t offset,
    const std::span<std::byte> destination) const
{
    static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));
    const auto length = static_cast<std::uint64_t>(destination.size());
    const auto end = checked_add(offset, length);
    if (!end) {
        return FileResult<std::size_t>::failure(
            make_error(FileErrorCode::arithmetic_overflow, FileOperation::read));
    }

    constexpr auto maximum_native_offset =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (offset > maximum_native_offset || *end > maximum_native_offset) {
        return FileResult<std::size_t>::failure(make_error(
            FileErrorCode::invalid_offset_or_length,
            FileOperation::read));
    }

    if (!range_within_file(offset, length, opened_metadata.size)) {
        return FileResult<std::size_t>::failure(
            make_error(FileErrorCode::read_out_of_bounds, FileOperation::read));
    }

    FileMetadata current{};
    FileError metadata_error{};
    if (!read_metadata(current, metadata_error, FileOperation::read)) {
        return FileResult<std::size_t>::failure(metadata_error);
    }
    if (!same_metadata(opened_metadata, current)) {
        return FileResult<std::size_t>::failure(
            make_error(FileErrorCode::file_changed, FileOperation::read));
    }
    return FileResult<std::size_t>::success(destination.size());
}

#ifdef _WIN32

FileReader::Impl::~Impl()
{
    if (handle != INVALID_HANDLE_VALUE) {
        static_cast<void>(CloseHandle(handle));
    }
}

bool FileReader::Impl::read_metadata(
    FileMetadata& metadata,
    FileError& error,
    const FileOperation operation) const noexcept
{
    SetLastError(ERROR_SUCCESS);
    const DWORD type = GetFileType(handle);
    const DWORD type_error = GetLastError();
    if (type == FILE_TYPE_UNKNOWN && type_error != ERROR_SUCCESS) {
        error = make_error(
            FileErrorCode::metadata_failed,
            operation,
            NativeErrorDomain::windows,
            type_error);
        return false;
    }
    if (type != FILE_TYPE_DISK) {
        error = make_error(FileErrorCode::not_regular_file, operation);
        return false;
    }

    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(handle, &information) == FALSE) {
        error = make_error(
            FileErrorCode::metadata_failed,
            operation,
            NativeErrorDomain::windows,
            GetLastError());
        return false;
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
        error = make_error(FileErrorCode::not_regular_file, operation);
        return false;
    }

    const std::uint64_t size =
        (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32U)
        | information.nFileSizeLow;
    constexpr auto maximum_native_size =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (size > maximum_native_size) {
        error = make_error(
            FileErrorCode::invalid_offset_or_length,
            operation);
        return false;
    }

    metadata = FileMetadata{
        size,
        information.dwVolumeSerialNumber,
        information.nFileIndexHigh,
        information.nFileIndexLow,
        information.ftLastWriteTime};
    return true;
}

FileResult<FileReader> FileReader::open(const std::filesystem::path& path)
{
    auto impl = std::make_unique<Impl>();
    impl->handle = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (impl->handle == INVALID_HANDLE_VALUE) {
        const DWORD native_error = GetLastError();
        return FileResult<FileReader>::failure(make_error(
            open_error_code(native_error),
            FileOperation::open,
            NativeErrorDomain::windows,
            native_error));
    }

    FileError metadata_error{};
    if (!impl->read_metadata(
            impl->opened_metadata,
            metadata_error,
            FileOperation::inspect)) {
        return FileResult<FileReader>::failure(metadata_error);
    }
    return FileResult<FileReader>::success(FileReader{std::move(impl)});
}

FileResult<std::size_t> FileReader::Impl::read(
    const std::uint64_t offset,
    const std::span<std::byte> destination) const
{
    std::lock_guard<std::mutex> lock{position_mutex};
    const auto validation = validate_read(offset, destination);
    if (!validation) {
        return validation;
    }
    if (destination.empty()) {
        return validation;
    }

    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) == FALSE) {
        return FileResult<std::size_t>::failure(make_error(
            FileErrorCode::read_failed,
            FileOperation::read,
            NativeErrorDomain::windows,
            GetLastError()));
    }

    std::size_t completed = 0;
    while (completed < destination.size()) {
        const std::size_t remaining = destination.size() - completed;
        const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
            remaining,
            std::numeric_limits<DWORD>::max()));
        DWORD received = 0;
        if (ReadFile(
                handle,
                destination.data() + completed,
                requested,
                &received,
                nullptr) == FALSE) {
            return FileResult<std::size_t>::failure(make_error(
                FileErrorCode::read_failed,
                FileOperation::read,
                NativeErrorDomain::windows,
                GetLastError()));
        }
        if (received == 0 || received > requested) {
            FileMetadata current{};
            FileError metadata_error{};
            if (!read_metadata(current, metadata_error, FileOperation::read)) {
                return FileResult<std::size_t>::failure(metadata_error);
            }
            const auto code = same_metadata(opened_metadata, current)
                ? FileErrorCode::unexpected_truncation
                : FileErrorCode::file_changed;
            return FileResult<std::size_t>::failure(
                make_error(code, FileOperation::read));
        }
        completed += received;
    }

    FileMetadata current{};
    FileError metadata_error{};
    if (!read_metadata(current, metadata_error, FileOperation::read)) {
        return FileResult<std::size_t>::failure(metadata_error);
    }
    if (!same_metadata(opened_metadata, current)) {
        return FileResult<std::size_t>::failure(
            make_error(FileErrorCode::file_changed, FileOperation::read));
    }
    return FileResult<std::size_t>::success(completed);
}

#else

FileReader::Impl::~Impl()
{
    if (descriptor >= 0) {
        static_cast<void>(::close(descriptor));
    }
}

bool FileReader::Impl::read_metadata(
    FileMetadata& metadata,
    FileError& error,
    const FileOperation operation) const noexcept
{
    struct stat information {};
    int status = 0;
    do {
        status = ::fstat(descriptor, &information);
    } while (status < 0 && errno == EINTR);
    if (status < 0) {
        error = make_error(
            FileErrorCode::metadata_failed,
            operation,
            NativeErrorDomain::posix,
            static_cast<std::uint64_t>(errno));
        return false;
    }
    if (!S_ISREG(information.st_mode)) {
        error = make_error(FileErrorCode::not_regular_file, operation);
        return false;
    }
    if (information.st_size < 0) {
        error = make_error(
            FileErrorCode::invalid_offset_or_length,
            operation);
        return false;
    }
    const auto size = static_cast<std::uint64_t>(information.st_size);
    constexpr auto maximum_native_size =
        static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
    if (size > maximum_native_size) {
        error = make_error(
            FileErrorCode::invalid_offset_or_length,
            operation);
        return false;
    }

#if defined(__APPLE__)
    const timespec modified = information.st_mtimespec;
    const timespec changed = information.st_ctimespec;
#else
    const timespec modified = information.st_mtim;
    const timespec changed = information.st_ctim;
#endif
    metadata = FileMetadata{
        size,
        information.st_dev,
        information.st_ino,
        information.st_mode,
        modified,
        changed};
    return true;
}

FileResult<FileReader> FileReader::open(const std::filesystem::path& path)
{
    auto impl = std::make_unique<Impl>();
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int descriptor = -1;
    do {
        descriptor = ::open(path.c_str(), flags);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        const int native_error = errno;
        return FileResult<FileReader>::failure(make_error(
            open_error_code(native_error),
            FileOperation::open,
            NativeErrorDomain::posix,
            static_cast<std::uint64_t>(native_error)));
    }
    impl->descriptor = descriptor;

#ifndef O_CLOEXEC
    if (::fcntl(descriptor, F_SETFD, FD_CLOEXEC) < 0) {
        const int native_error = errno;
        return FileResult<FileReader>::failure(make_error(
            FileErrorCode::open_failed,
            FileOperation::open,
            NativeErrorDomain::posix,
            static_cast<std::uint64_t>(native_error)));
    }
#endif

    FileError metadata_error{};
    if (!impl->read_metadata(
            impl->opened_metadata,
            metadata_error,
            FileOperation::inspect)) {
        return FileResult<FileReader>::failure(metadata_error);
    }
    return FileResult<FileReader>::success(FileReader{std::move(impl)});
}

FileResult<std::size_t> FileReader::Impl::read(
    const std::uint64_t offset,
    const std::span<std::byte> destination) const
{
    const auto validation = validate_read(offset, destination);
    if (!validation || destination.empty()) {
        return validation;
    }

    std::size_t completed = 0;
    while (completed < destination.size()) {
        const std::size_t remaining = destination.size() - completed;
        const std::size_t maximum_read =
            static_cast<std::size_t>(std::numeric_limits<ssize_t>::max());
        const std::size_t requested = std::min(remaining, maximum_read);
        const auto native_offset = static_cast<off_t>(offset + completed);

        ssize_t received = -1;
        do {
            received = ::pread(
                descriptor,
                destination.data() + completed,
                requested,
                native_offset);
        } while (received < 0 && errno == EINTR);

        if (received < 0) {
            return FileResult<std::size_t>::failure(make_error(
                FileErrorCode::read_failed,
                FileOperation::read,
                NativeErrorDomain::posix,
                static_cast<std::uint64_t>(errno)));
        }
        if (received == 0) {
            FileMetadata current{};
            FileError metadata_error{};
            if (!read_metadata(current, metadata_error, FileOperation::read)) {
                return FileResult<std::size_t>::failure(metadata_error);
            }
            const auto code = same_metadata(opened_metadata, current)
                ? FileErrorCode::unexpected_truncation
                : FileErrorCode::file_changed;
            return FileResult<std::size_t>::failure(
                make_error(code, FileOperation::read));
        }
        completed += static_cast<std::size_t>(received);
    }

    FileMetadata current{};
    FileError metadata_error{};
    if (!read_metadata(current, metadata_error, FileOperation::read)) {
        return FileResult<std::size_t>::failure(metadata_error);
    }
    if (!same_metadata(opened_metadata, current)) {
        return FileResult<std::size_t>::failure(
            make_error(FileErrorCode::file_changed, FileOperation::read));
    }
    return FileResult<std::size_t>::success(completed);
}

#endif

} // namespace aural::core
