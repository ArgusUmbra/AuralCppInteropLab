# Streaming ZIP payload CRC32 verification

## Authority and scope

The internal `payload_crc32` component implements AURAL Draft 0.2 sections
2, 6.3, 6.5, and verification step 8. PKWARE APPNOTE 6.3.10 sections 4.1.5,
4.3.7-4.3.8, 4.3.12, and 4.4.7 define the ZIP CRC32 fields and algorithm.
The CRC register starts at all ones and the final residual is complemented;
zlib's `crc32` interface supplies those ZIP-compatible pre/post conditions.
The CRC32 of an empty entry is zero.

Every structurally admitted stored entry is checked. The computed CRC32 of
the complete payload must equal the declaration already proven identical in
the local and central headers. CRC32 is error detection, not cryptographic
authentication. It is independent of, and does not replace, the later
manifest SHA-256 check.

## Streaming and ownership

The path entry point opens one native `FileReader`, performs complete classic
or ZIP64 physical-layout inspection on that handle, and then verifies payloads
on the same handle. Each payload is read positionally in chunks of at most
65,536 bytes. Memory use is therefore a fixed stack buffer plus result data
bounded by the structural entry limit; it is never proportional to recording
size. Zero-length entries make no native payload read.

Payload positions, remaining byte counts, and ends are unsigned 64-bit values.
The stage defensively rechecks layout file size, checked offset-plus-length,
the stored payload end, and the open-file boundary before reading. It never
decompresses, extracts, writes, or modifies archive data.

`FileReader` owns the read-only handle. It checks handle identity, size, and
modification metadata around reads and reports changes, short reads, native
failures, and unexpected EOF. This is same-handle verification with change
detection; it is not an immutable filesystem snapshot guarantee.

## Results and limits

Success is exactly `crc32_verified` and includes each raw entry name, central
header offset, payload half-open range, declared CRC32, and calculated CRC32.
It establishes only stored-payload CRC32 equality. It is not structural-only
success and is not AURAL package acceptance.

Failures distinguish CRC32 mismatch, invalid prerequisite structure, payload
read failure, allocation failure, and unsupported operation. Integrity errors
retain the entry name and useful central, payload, failure, and remaining-byte
offsets where an entry is available. No failed result contains a successful
entry list.

SHA-256, manifest-to-entry correlation, WAVE/RF64 identification, exact AURAL
inventory, regular-file metadata policy, public package APIs, extraction, and
complete package acceptance are outside this milestone.
