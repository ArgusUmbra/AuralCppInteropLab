# AURAL C++ Interoperability Laboratory

This repository is an experimental, independently developed C++20 laboratory
for the AURAL Core Reader profile in AURAL Audio Evidence Container Draft 0.2.
The published specification is the authority; this work is not a translation
of an existing Python, Node.js, or Java implementation.

The current foundation contains a frozen public specification/conformance
snapshot, a minimal native status executable, dependency known-answer tests,
frozen-input integrity tests, checked byte ranges, and a bounded read-only
native file reader. It does **not** contain an AURAL verifier and does not yet
accept, reject, parse, or extract AURAL packages.

## Clean-room method

Only published specifications, schemas, conformance documentation, public case
metadata, expected results, and public fixtures from the pinned source commit
are frozen here. Existing verifier source, implementation tests, adapters,
internal helpers, runners, and fixture generators are excluded. See
`docs/CLEAN-ROOM-METHODOLOGY.md`.

## Build requirements

- C++20 compiler (validated with MSYS2 UCRT64 GCC 15.2.0)
- CMake (validated with 4.2.3)
- Ninja (validated with 1.13.2)
- OpenSSL Crypto (validated with 3.6.1)
- zlib (validated with 1.3.2)

For MSYS2 UCRT64, prepend `C:\msys64\ucrt64\bin` and
`C:\msys64\usr\bin` to the current process `PATH`, then run:

```powershell
cmake --preset ucrt64-debug
cmake --build --preset ucrt64-debug
ctest --preset ucrt64-debug
```

## Reusable library

Within a CMake build tree, another target can link the library directly:

```cmake
add_subdirectory(path/to/AuralCppInteropLab)
target_link_libraries(my_target PRIVATE aural::core)
```

Include `<aural/core/range.hpp>` to use checked arithmetic and byte ranges:

- `checked_add` performs checked unsigned 64-bit addition.
- `ByteRange` creates validated immutable half-open byte ranges and provides
  size, empty, containment, and overlap queries.
- `range_within_file` validates an offset and length against a declared file
  size, including integer-overflow rejection.

Invalid inputs return an empty `std::optional`; normal validation failures do
not throw exceptions.

Include `<aural/core/file_reader.hpp>` for bounded native reads. This complete
example opens one file, reports a portable error, and reads exactly 16 bytes
into caller-owned storage:

```cpp
#include <aural/core/file_reader.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <utility>

int read_header(const std::filesystem::path& path)
{
    auto opened = aural::core::FileReader::open(path);
    if (!opened) {
        std::cerr << opened.error().description() << '\n';
        return 1;
    }

    auto reader = std::move(opened).value();
    std::array<std::byte, 16> header{};
    const auto read = reader.read_at(0, header);
    if (!read) {
        std::cerr << read.error().description() << '\n';
        return 1;
    }
    return 0;
}
```

`FileReader` owns one regular-file handle, closes it with RAII, is movable but
not copyable, captures the opened size and identity metadata, rejects invalid
ranges before native I/O, and never allocates from a requested file length.
Successful reads fill the entire supplied `std::span<std::byte>`. See
`docs/FILE-READER.md` for platform behavior and concurrency limitations.

All APIs are experimental. No installed-package, stable-source, or stable-ABI
guarantee is provided.

The intended long-term objective is independent execution of the public Core
Reader conformance inventory through Adapter Protocol 1. Passing that public
inventory would be interoperability evidence only—not certification, complete
verification of the input space, or a security guarantee.

## Limitations

- No AURAL JSON, manifest, ZIP, ZIP64, WAV, or adapter implementation exists.
- No package-verification API exists.
- File consistency checks reduce, but cannot eliminate, concurrent-mutation
  races or detect every same-size content change on every filesystem.
- No conformance case is currently executed through a verifier.
- Draft 0.1 legacy fixtures are frozen but explicitly optional.
- The project has no affiliation with or certification by a third party.
