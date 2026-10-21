# Architecture

## Current foundation

The repository currently builds only:

- `aural_lab`, a status executable that states the verifier is not implemented;
- `dependency_smoke`, OpenSSL SHA-256 and zlib CRC32 known-answer checks; and
- `frozen_input_verify`, a streaming check of the frozen SHA-256 inventory;
- `aural_core`, checked 64-bit byte ranges, a bounded native file reader, and
  strict allocation-free AURAL-profile UTF-8 and JSON primitive validation,
  plus an owned bounded complete JSON document model;
  and
- `core_json_string_decoder`, adversarial coverage for the internal bounded
  JSON string-token decoder; and
- `core_json_primitives`, adversarial coverage for bounded integers and the
  exact `true`, `false`, and `null` tokens; and
- `core_json_values`, adversarial coverage for shared value dispatch, bounded
  arrays, exact consumption, source offsets, and aggregate resource limits;
  and
- `core_json_objects_and_documents`, adversarial coverage for source-ordered
  objects, decoded duplicate names, complete documents, and mixed containers.
  and
- `core_json_canonicalization_foundations`, adversarial coverage for RFC 8785
  decoded-string serialization and independent UTF-16 property ordering; and
- `core_json_document_canonicalization`, complete bounded RFC 8785 document
  serialization coverage over the AURAL integer-only JSON model; and
- `core_json_canonicality`, byte-for-byte canonicality, failure-taxonomy,
  resource-limit, immutability, and frozen-golden-manifest coverage.
- `aural_core` also contains an internal Draft 0.2 manifest structure
  validator over the owned JSON tree and a parse-once canonical-bytes plus
  structure entry point; and
- `core_manifest_structure`, adversarial coverage for the closed core shape,
  field constraints, extension policy, safe paths, determinism, immutability,
  combined-pipeline ordering, and the frozen golden manifest.
- `aural_core` contains an internal bounded classic ZIP EOCD and central-
  directory inspector with raw filename ownership, typed offset diagnostics,
  AURAL resource ceilings, multidisk rejection, and a distinct ZIP64-required
  outcome; and
- `core_zip_central_directory`, independently assembled byte-record tests and
  selected frozen-fixture structural coverage.

There is no package verifier component yet.

## Planned component boundaries

| Future component | Responsibility | Defensive boundary |
|---|---|---|
| Bounded file access | Read-only move-owned file handle, exact range reads, size/identity checks | Implemented; never allocates or reads from an unvalidated hostile length |
| Checked arithmetic | Typed checked add/subtract/range conversion for offsets and lengths | Reject overflow, truncation, signed/unsigned ambiguity, and ZIP64 high-bit abuse |
| UTF-8 and JSON | Strict UTF-8, bounded string-token decoding, allocation-free AURAL-profile primitives, and iterative owned arrays/objects with decoded duplicate-name rejection are implemented | 1 MiB source ceiling, depth 32, bounded aggregate values/members/decoded strings, no replacement decoding, normalization, floating-point coercion, or partial success |
| RFC 8785 JCS | Canonical decoded-string serialization, non-mutating UTF-16 property ordering, complete iterative document serialization, and original-byte validation are implemented | Validate/measure before exact output allocation, depth/value/string/output bounds, no partial success, first mismatch offset, and separate sorted index views |
| Manifest structure | Required/optional Draft 0.2 members, closed core objects, local field constraints, timestamps, and extensions | Implemented over a const owned JSON tree; success is structural only and cannot imply package validity |
| Manifest semantic model | Immutable package-correlated Draft 0.2 values | Planned; construct only after archive agreement, hash, and cross-field validation |
| ZIP/ZIP64 records | Classic EOCD discovery and bounded central-directory parsing are implemented; local records and ZIP64 chains are planned | Current stage validates classic record bounds, counts, raw names, offsets, single-disk form, and sentinels without claiming package validity |
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
