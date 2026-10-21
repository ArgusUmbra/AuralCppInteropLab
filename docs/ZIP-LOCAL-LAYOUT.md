# Classic ZIP local headers and physical layout

## Authority and boundary

The internal `classic_local_layout` component applies AURAL Draft 0.2
sections 6.1-6.6 and verification step 4 on top of the classic central-
directory inspector. Its ZIP record interpretation follows PKWARE APPNOTE
6.3.10 sections 4.3.2-4.3.9, 4.3.12, 4.4.1, 4.4.3-4.4.12, 4.4.16-4.4.18,
and 4.5.3.

Success means that the classic local records agree with their central records
and satisfy the applicable Draft 0.2 physical ZIP profile. It is not complete
AURAL package acceptance. Exact two-name inventory, regular-file attributes,
manifest parsing/correlation, CRC32 computation, SHA-256, WAVE/RF64 identity,
and ZIP64 entry-extra resolution remain later stages.

## Validated records and consistency

For every central entry, the component uses the declared physical local-header
offset, reads the 30-byte fixed header, decodes unsigned little-endian fields,
and bounds the declared filename and extra-field regions before allocating or
reading them. It requires exact equality of version-needed, flags, method,
MS-DOS time/date, CRC32 declaration, compressed and uncompressed sizes, and
filename octets. Filename bytes are compared and retained without decoding or
including them in diagnostics.

The derived payload is the checked half-open range beginning after the fixed
header, filename, and extra field and having the central compressed size. No
payload byte is read. Returned metadata owns its bounded filename and local
extra-field bytes and is sorted by physical local-header offset, independently
of central-directory order.

## Ordinary ZIP versus the AURAL profile

APPNOTE permits features that Draft 0.2 prohibits. This stage enforces the
Draft 0.2 choices relevant to local records and layout:

- method 0 only, with equal compressed and uncompressed sizes;
- flags zero except optional UTF-8-name bit 11;
- bit 3 data descriptors are reported as unsupported and never skipped;
- no encryption, patched data, strong encryption, or masked local fields;
- no local or central extra field in a fully classic result;
- no entry or archive comment; and
- no prefix, gap, trailer, or undeclared byte between local entries, the
  central directory, and terminal EOCD.

The central parser already requires a single disk, a terminal classic/ZIP64
EOCD chain, and a central directory ending at that chain. This stage requires the first
physical local header at offset zero, each following physical header exactly
at the preceding payload end, and the final payload exactly at the central-
directory start. A reordered central directory is valid.

## ZIP64 and descriptors

An entry sentinel discovered by the central parser returns
`zip64_entry_resolution_pending`; either 32-bit local-size sentinel returns
`zip64_support_required`. No ZIP64 entry extra is partially accepted. In a
classic record, a nonempty
extra field is an unsupported AURAL feature because Draft 0.2 permits only a
necessary ZIP64 field.

APPNOTE permits bit-3 data descriptors and both signed and unsigned descriptor
forms. Draft 0.2 prohibits descriptors, so the component reports the feature
at the local flags field and does not infer a descriptor length or scan for a
signature.

## Physical range algorithm

After parsing all local records, the component sorts owned range metadata by
physical offset. It detects duplicate local references, overlapping local
structures, header/payload intersections, payload/payload intersections, and
entry intersections with the central directory or EOCD. Checked 64-bit
addition is used for every derived end. A payload beyond physical EOF is
reported as truncated before any payload I/O.

## Errors, ownership, and limits

Malformed structure, unsupported AURAL ZIP features, ZIP64 entry-pending or
local ZIP64-required input, resource exhaustion, and native file failures are distinct statuses. Structural
and feature details carry a physical byte offset. Central-parser details remain
nested when that prerequisite fails. Human diagnostics never contain an
untrusted filename.

The file is opened once, read-only, and shared with the central and local
stages, preserving the existing native snapshot checks. Default limits remain
8,606,711,808 physical bytes, 4,194,304 central-directory bytes, and two
entries. A local filename plus extra field is inherently bounded by the two
16-bit length fields. Allocation failure returns no partial layout. There is no
recursion, extraction, decompression, archive write, or payload buffering.

## Resolved interpretation and remaining ambiguity

Draft 0.2 explicitly requires exact local/central agreement after ZIP64
expansion and gapless layout; those are profile rules, not claims about
ordinary ZIP validity. For this classic-only milestone, any extra field is
reported as unsupported rather than malformed unless a sentinel first requires
ZIP64 support. The next milestone must resolve the contents and equality of
necessary ZIP64 extras and integrate their values into local layout.

APPNOTE does not make central-directory ordering match physical ordering, and
Draft 0.2 says entry order is insignificant to readers. Therefore central
record reordering is accepted. No unresolved ambiguity affects classic
acceptance in this stage.
