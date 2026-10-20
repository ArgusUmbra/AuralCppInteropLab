# AURAL C++ Interoperability Laboratory

This repository is an experimental, independently developed C++20 laboratory
for the AURAL Core Reader profile in AURAL Audio Evidence Container Draft 0.2.
The published specification is the authority; this work is not a translation
of an existing Python, Node.js, or Java implementation.

The current foundation contains a frozen public specification/conformance
snapshot, a minimal native status executable, dependency known-answer tests,
and frozen-input integrity tests. It does **not** contain an AURAL verifier and
does not yet accept, reject, parse, or extract AURAL packages.

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

The intended long-term objective is independent execution of the public Core
Reader conformance inventory through Adapter Protocol 1. Passing that public
inventory would be interoperability evidence only—not certification, complete
verification of the input space, or a security guarantee.

## Limitations

- No AURAL JSON, manifest, ZIP, ZIP64, WAV, or adapter implementation exists.
- No package-verification API exists.
- No conformance case is currently executed through a verifier.
- Draft 0.1 legacy fixtures are frozen but explicitly optional.
- The project has no affiliation with or certification by a third party.
