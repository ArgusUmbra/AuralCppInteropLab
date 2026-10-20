# Experimental bounded file reader

`<aural/core/file_reader.hpp>` is an experimental C++20 API. Its source and ABI
may change before the reusable library is versioned for external release.

## Contract

`FileReader::open` opens an existing regular file read-only and captures its
native identity, length, and available modification metadata. A successful
reader owns exactly one handle. It is movable, cannot be copied, and closes the
handle during destruction or move assignment.

`read_at(offset, destination)` fills a caller-owned `std::span<std::byte>`.
Success means every requested byte was returned; the result value equals the
span size. Before I/O, the reader checks 64-bit addition, native offset
representability, the captured file boundary, and current metadata. It checks
metadata again after non-empty reads. Zero bytes at the captured EOF is valid.
The reader never allocates a buffer based on the file size or requested range.

Expected failures use `FileResult<T>` and `FileError`. Applications should make
decisions using `FileErrorCode`. `NativeFileError` retains an opaque diagnostic
domain and numeric code for logging; portable application logic must not depend
on Win32 or POSIX constants.

## Windows boundary

Windows paths go directly to `CreateFileW`, preserving native Unicode paths.
The handle requests only `GENERIC_READ` and shares existing/future reads, but
does not share writes or deletes. This normally prevents a new writer,
truncation, replacement, or deletion while verification owns the handle.
Directories and non-disk handles are rejected. `GetFileInformationByHandle`
captures the volume serial, file index, size, and last-write time.

The implementation uses synchronous `SetFilePointerEx` plus exact `ReadFile`
loops. A per-reader mutex makes multiple `read_at` calls safe, but serializes
them because the Windows file pointer is handle-wide. Moving or destroying a
reader concurrently with a read is not supported. Separate readers may operate
in parallel.

## Linux/POSIX boundary

The POSIX implementation opens with `O_RDONLY`, applies close-on-exec, requires
a regular `fstat` result, and uses `pread` so reads do not share a mutable file
position. Interrupted opens, metadata calls, and reads are retried. Reads are
chunked to the native `ssize_t` limit and must return the exact requested byte
count. Device/inode, mode, size, modification time, and change time are checked.
CMake defines `_FILE_OFFSET_BITS=64` for UNIX builds.

Linux tests inspect the owned descriptor through `/proc/self/fd` to confirm
read-only access and close-on-exec, exercise a non-ASCII UTF-8 path, exact and
EOF reads, changing offsets, move ownership, cleanup, and detectable
truncation. The retry loops for `EINTR` are code-reviewed but not fault-injected:
regular-file `open`, `fstat`, and `pread` cannot be interrupted reliably in a
portable deterministic test.

Calls to `read_at` on one POSIX reader do not mutate a shared file position.
Moving or destroying a reader concurrently with a read is not supported.

## GCC analyzer disposition

GCC 15 `-fanalyzer` reports `-Wanalyzer-use-of-uninitialized-value` at both
`FileResult<FileReader>::success` and `FileResult<FileReader>::failure` return
expressions. The failure path never constructs or accesses a `FileReader`, and
explicitly initializing the optional's active and inactive states does not
change the diagnostic. Normal strict-warning builds and ownership tests pass.
This is therefore treated as a GCC analysis false positive around returning a
result that contains `std::optional` storage for a move-only type, not as an
observed initialization or ownership defect.

The analyzer preset keeps `-fanalyzer` enabled across the library target and
relaxes `-Werror` only for that named diagnostic in `file_reader.cpp`, so it
remains visible in CI. All other compiler and analyzer warnings remain errors.
This exception should be revisited when the CI GCC major version changes.

## Consistency limits

These checks are defense in depth, not an atomic snapshot. A process that
already has a writable handle, memory mapping, or sufficient platform-specific
privileges may race the reader. Filesystem timestamp granularity and caching can
hide a same-size rewrite. The path may also be rebound after open; verification
continues against the owned handle and captured native identity, not by reopening
the path. Callers requiring a cryptographic content identity must compute and
validate one while streaming through bounded reads.

On Windows, the adversarial test confirms that the sharing policy denies a
truncation attempt. It cannot force the native unexpected-short-read branch
without bypassing that safety policy. On POSIX, the test suite truncates through
a second descriptor and expects `file_changed`. The `unexpected_truncation`
error remains available for a short native read whose metadata appears stable.
