# Streaming ZIP payload SHA-256 calculation

## Provider and scope

The internal `payload_sha256` component uses the OpenSSL 3 EVP digest API.
OpenSSL was already a required Windows and Linux dependency, supports
incremental SHA-256, and already has an independent `abc` known-answer smoke
test in this project. No cryptographic primitive or dependency was added.

The component calculates SHA-256 for every structurally admitted stored entry.
It returns a complete 32-byte binary digest and provides canonical lowercase
64-character hexadecimal formatting. It deliberately performs no expected-
digest comparison. A calculated digest establishes only the bytes observed on
the validated archive payload range; it does not establish manifest agreement,
authenticity, provenance, or AURAL package validity.

## Streaming, prerequisites, and ownership

The path entry point opens one read-only `FileReader`, performs complete
classic or ZIP64 physical-layout inspection, and calculates payload digests on
that same handle without repeating structural parsing. SHA-256 and the ZIP
CRC32 prerequisite are updated together from each chunk. A digest result is
released only after the entry's calculated CRC32 matches the identical local
and central declaration.

Each read is at most 65,536 bytes. Memory is one fixed stack buffer, one EVP
context, and result data bounded by the structural entry limit; it is not
proportional to recording length. Empty payloads are finalized without a
native payload read. Offsets, lengths, positions, and ends are unsigned 64-bit
values with checked addition and whole-range validation before I/O. Binary
payloads, including embedded null bytes, are passed with explicit lengths.

`FileReader` owns and closes the native read-only handle. It checks identity,
size, and modification metadata around reads and reports native failures,
unexpected truncation, and changes. Same-handle access with those checks is
not an immutable filesystem snapshot guarantee.

## Typed outcomes and remaining work

Outcomes distinguish successful calculation, invalid prerequisite structure,
CRC32 prerequisite failure, read failure, resource exhaustion, cryptographic
provider failure, and unsupported archive operations. Entry results preserve
the raw name, central-header offset, payload half-open range, declared and
calculated CRC32, and binary SHA-256 digest. Failed calculations expose no
partial successful entry list.

Manifest-to-entry SHA-256 correlation, exact package inventory, WAV/RF64
validation, public package-verification APIs, signatures, provenance,
extraction, and complete package acceptance remain outside this milestone.
