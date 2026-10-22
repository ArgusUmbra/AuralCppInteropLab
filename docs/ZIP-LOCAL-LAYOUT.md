# ZIP and ZIP64 local headers and physical layout

## Authority and boundary

The internal `classic_local_layout` component applies AURAL Draft 0.2
sections 6.1-6.6 and verification step 4 on top of the resolved central-
directory inspector. Record interpretation follows PKWARE APPNOTE 6.3.10
sections 4.3.2-4.3.9, 4.3.12, 4.4.1, 4.4.3-4.4.12, 4.4.16-4.4.18, and 4.5.3.

Success establishes a complete structural layout for classic or ZIP64 stored
entries. It is not complete AURAL package acceptance. Exact two-name inventory,
regular-file attribute policy, manifest correlation, SHA-256, and WAVE/RF64
validation remain separate stages. CRC32 computation is a following internal
stage over the returned payload ranges.

## Local ZIP64 resolution and consistency

For every resolved central entry, the component uses the effective unsigned
64-bit local-header offset and reads the 30-byte fixed local header. Filename
and extra-field regions are bounded before allocation and read from the same
open `FileReader`. Exact raw filename bytes are retained and compared without
decoding or including untrusted bytes in diagnostics.

Local and central declarations are independent. A local header with neither
size sentinel must have no extra field. If local ZIP64 is used, APPNOTE 4.5.3
requires both local size values; AURAL's no-surplus rule therefore requires
both classic size fields to be sentinels and exactly one `0x0001` field with
exactly 16 data bytes. The values are decoded in fixed APPNOTE order:
uncompressed size followed by compressed size. Missing, unnecessary,
duplicate, truncated, surplus, and non-`0x0001` fields are rejected.

After independent expansion, effective local and central compressed and
uncompressed sizes must agree. Their classic sentinel representations need
not be identical: local-only and central-only necessary ZIP64 declarations are
valid when their effective values agree. Version-needed must be 4.5 when
either header uses ZIP64. Version, flags, method, timestamps, CRC declaration,
and filename bytes otherwise match exactly. Stored compressed and
uncompressed sizes must be equal.

## Physical range algorithm

The payload is the checked half-open range beginning immediately after the
fixed header, filename, and permitted extra field. Its length is the resolved
64-bit compressed size. Payload bytes are never buffered or read by this
stage. Returned metadata owns the bounded filename and local extra bytes and
is sorted by physical local-header offset, independently of central order.

The validator checks fixed and variable local-header ranges, payload ends,
physical EOF, duplicate local references, header/header, header/payload, and
payload/payload overlap. Every local entry must remain before the central
directory. Because the validated central directory is contiguous with the
applicable ZIP64 EOCD/locator and classic EOCD chain, the central-directory
barrier also prevents any payload from reaching those end records.

The first physical local header must begin at offset zero, each later header
must begin at the previous payload end, and the final payload must end exactly
at the central-directory start. These equalities reject prefixes, gaps,
trailers, and undeclared regions. All derived ends use checked unsigned 64-bit
arithmetic.

## Profile restrictions, limits, and errors

Method 0 is required; flags must be zero except optional UTF-8-name bit 11;
data descriptors and all encryption-related flags are unsupported. Entry and
archive comments are prohibited. ZIP64 disk starts remain single-disk only.

Default limits are 8,606,711,808 physical bytes, 4,194,304 central-directory
bytes, two entries, and 8,589,934,592 uncompressed bytes per entry. Variable
local fields are additionally bounded by their 16-bit lengths. Allocation
failure, native I/O failure, unsupported profile features, malformed local
ZIP64 fields, ZIP64 necessity violations, local/central disagreement, and
invalid physical ranges remain typed outcomes. Structural errors carry a
physical byte offset and no invalid result contains a partial layout.

## Remaining work

This component itself does not compute payload CRC32 or SHA-256, validate
payload content, enforce the exact AURAL entry inventory or regular-file
attributes, correlate a manifest, validate WAVE/RF64, decompress, extract, or
provide a Core Reader conformance adapter. The separate CRC32 stage consumes
its validated payload locations. None of those operations can be inferred
from a successful structural layout.
