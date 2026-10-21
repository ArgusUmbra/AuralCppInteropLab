# ZIP64 end-record inspection

## Authority and scope

The internal `zip64_end_records` component implements only the archive-level
ZIP64 envelope described by AURAL Draft 0.2 sections 6.1, 6.3, 6.4, 6.6, and
7 and PKWARE APPNOTE 6.3.10 sections 4.3.14-4.3.16 and 4.4.1.4. It does not
resolve the ZIP64 extended-information extra field in a central or local file
header. Successful end-record inspection is therefore not complete ZIP64
layout validation and is never an AURAL package-verification result.

## Records and sentinel resolution

The terminal classic EOCD remains the discovery anchor. Its fixed fields use
`0xffff` sentinels for the two disk numbers and two entry counts and
`0xffffffff` sentinels for central-directory size and offset. If any such
sentinel appears, the inspector requires this physical sequence:

```text
central directory
ZIP64 EOCD record (56 physical bytes; declared remaining size 44)
ZIP64 EOCD locator (20 bytes)
classic EOCD (22 bytes plus its declared comment)
physical EOF
```

The locator signature is `0x07064b50`; it declares disk zero, the unsigned
64-bit physical offset of the ZIP64 EOCD, and exactly one disk. The ZIP64 EOCD
signature is `0x06064b50`; its version-needed field is 4.5, both disk fields
are zero, its on-disk and total entry counts agree, and its 64-bit count,
central-directory size, and central-directory offset become the resolved
values. AURAL permits no extensible data sector, so the declared record size
must be exactly 44 rather than merely at least 44.

Every classic field that is not a sentinel must equal the corresponding
resolved ZIP64 value. Sentinel fields take their value from the ZIP64 record.
All decoding and range calculations use unsigned 64-bit values and checked
addition; no resolved value is narrowed to a classic field.

The classic EOCD alone remains sufficient when it has no sentinel. ZIP64
signatures in comments or other unrelated bytes do not trigger ZIP64 parsing.
When a central file header subsequently contains a sentinel, the central
parser returns `zip64_entry_resolution_pending`; it does not expose a partial
directory as structurally valid.

## Bounds, placement, and errors

Before allocating from declarations, inspection enforces the configured
physical archive, central-directory byte, and entry-count ceilings. The
locator must immediately precede the classic EOCD. The ZIP64 EOCD must fit the
opened file and end exactly at the locator. The resolved central directory
must fit the file and end exactly at the ZIP64 EOCD. These equalities reject
gaps, overlaps, trailers, impossible offsets, and undeclared bytes.

Typed structural errors distinguish terminal classic EOCD failures, missing,
truncated, or incorrectly signed locators, impossible locator offsets,
truncated or incorrectly signed ZIP64 EOCD records, wrong record size or
version, noncontiguous structures, multidisk declarations, classic/ZIP64
contradictions, physical-range failures, and AURAL resource-limit failures.
Each error identifies the responsible physical field or boundary. File and
allocation failures remain separate result categories. Both the standalone
inspector and the central parser can use the same already-open `FileReader`,
so no archive is independently reopened between these stages.

## Unnecessary archive-level ZIP64 ambiguity

The frozen AURAL text is explicit for entry extra fields: a ZIP64 `0x0001`
extra may occur only when a corresponding classic field is a sentinel. The
archive-level wording is less explicit. Section 6.1 says ZIP64 records may
appear only as allowed later; section 6.4 requires the ZIP64 EOCD and locator
when the classic EOCD uses a sentinel, but does not state the converse as an
independent `MUST NOT`. Section 9 also observes that permitted ZIP64 choices
can change whole-archive bytes.

This implementation follows the fail-closed layout reading for Draft 0.2:
without an EOCD sentinel, the central directory must end directly at the
classic EOCD, so an unnecessary ZIP64 EOCD/locator chain is rejected as a gap
or undeclared bytes. That treatment is consistent with “MAY appear only as
allowed below,” but the absence of an explicit archive-level converse leaves
a normative ambiguity. The authoritative standard should clarify whether an
otherwise consistent ZIP64 envelope is permitted when no classic EOCD field
needs expansion.

## Remaining ZIP64 work

ZIP64 per-entry extra-field order and exact-length validation, resolved entry
sizes and local-header offsets, local/central comparison after expansion, and
complete ZIP64 physical-layout validation remain for the next milestone.
CRC32, SHA-256, inventory, manifest correlation, WAVE/RF64 identification,
extraction, and package verification also remain outside this component.
