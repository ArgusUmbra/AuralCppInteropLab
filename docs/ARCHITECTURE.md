# Architecture

## Current foundation

The repository currently builds only:

- `aural_lab`, a status executable that states the verifier is not implemented;
- `dependency_smoke`, OpenSSL SHA-256 and zlib CRC32 known-answer checks; and
- `frozen_input_verify`, a streaming check of the frozen SHA-256 inventory;
- `aural_core`, checked 64-bit byte ranges, a bounded native file reader, and
  strict allocation-free AURAL-profile UTF-8 and JSON primitive validation,
  plus an owned bounded JSON value/array model;
  and
- `core_json_string_decoder`, adversarial coverage for the internal bounded
  JSON string-token decoder; and
- `core_json_primitives`, adversarial coverage for bounded integers and the
  exact `true`, `false`, and `null` tokens; and
- `core_json_values`, adversarial coverage for shared value dispatch, bounded
  arrays, exact consumption, source offsets, and aggregate resource limits.

There is no package verifier component yet.

## Planned component boundaries

| Future component | Responsibility | Defensive boundary |
|---|---|---|
| Bounded file access | Read-only move-owned file handle, exact range reads, size/identity checks | Implemented; never allocates or reads from an unvalidated hostile length |
| Checked arithmetic | Typed checked add/subtract/range conversion for offsets and lengths | Reject overflow, truncation, signed/unsigned ambiguity, and ZIP64 high-bit abuse |
| UTF-8 and JSON | Strict UTF-8, bounded string-token decoding, allocation-free AURAL-profile primitives, and iterative owned value/array parsing are implemented; objects and duplicate-name detection remain planned | 1 MiB source ceiling, depth 32, bounded aggregate values/decoded strings, no replacement decoding, normalization, floating-point coercion, or partial success |
| RFC 8785 JCS | Exact canonical-byte validation for the supported JSON domain | Compare original bytes with independently generated canonical bytes |
| Manifest model | Immutable validated Draft 0.2 values, paths, timestamps, extensions | Construct only after syntax, type, range, and semantic validation |
| ZIP/ZIP64 records | Raw little-endian record parsing and cross-record consistency | Validate complete layout, sentinels, extras, offsets, sizes, names, flags, and metadata |
| CRC32/SHA-256 streams | zlib CRC32 and OpenSSL EVP SHA-256 over bounded byte ranges | Stream fixed-size buffers; never buffer an entire hostile asset |
| Package verifier | Ordered orchestration of container, manifest, WAV identity, and integrity checks | No extraction; explicit typed result and controlled rejection |
| Adapter Protocol 1 | Strict bounded stdin/stdout protocol around package verification | Separate protocol errors from controlled package rejection |

## Ownership and error model

Resources will use RAII and single ownership by default. Parsed byte ranges will
not outlive their backing file abstraction. Validated domain objects will be
immutable. Expected validation failures will use explicit structured results;
unexpected resource or platform failures will remain distinguishable from an
invalid package.

All file offsets and archive sizes will use checked unsigned 64-bit arithmetic
with explicit conversion at API boundaries. No pointer arithmetic will be used
to substitute for validated file offsets.

## Intended verification flow

1. Open and identify a stable regular input file.
2. Parse and validate the complete ZIP/ZIP64 layout with bounded reads.
3. Read and strictly decode the bounded manifest entry.
4. Validate exact JCS bytes and Draft 0.2 manifest semantics.
5. Identify the declared original WAV member without extraction.
6. Stream CRC32 and SHA-256 over the declared archive member bytes.
7. Return an explicit accept/reject result with bounded diagnostics.
8. Expose that result through Adapter Protocol 1 in a separate executable layer.

These are design targets only and are not implemented by this foundation.
