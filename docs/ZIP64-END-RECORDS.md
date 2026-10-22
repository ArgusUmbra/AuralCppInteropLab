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
Central file-header sentinels are now resolved independently from a necessary
and exact ZIP64 `0x0001` extra field. This does not change whether the
archive-level ZIP64 end-record chain is necessary.

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

## Strict ZIP64 necessity policy

The frozen AURAL text now makes the rule recorded by authoritative design
decision D-012 explicit. If any classic EOCD count, size, offset, or disk field
uses its ZIP64 sentinel, the ZIP64 EOCD and locator are required. If no classic
EOCD field uses a sentinel, both ZIP64 end records are prohibited. Local and
central ZIP64 `0x0001` extra fields follow an independent per-header necessity
rule: every sentinel-required value is present in APPNOTE order, no
non-required value or trailing byte is present, and the field is absent when
that header has no corresponding sentinel.

The existing archive-level inspector is consistent with this clarification.
It requires the chain whenever a classic EOCD sentinel is present, while a
redundant chain leaves undeclared bytes between the central directory and
classic EOCD and is rejected by the exact-end boundary check. No parser change
is needed for the archive-level rule. Central per-entry ZIP64 resolution is
implemented separately; local-header ZIP64 resolution remains deferred.

## Remaining ZIP64 work

Local ZIP64 per-entry extra-field resolution, local/central comparison after
expansion, and complete ZIP64 physical-layout validation remain for the next
milestone.
CRC32, SHA-256, inventory, manifest correlation, WAVE/RF64 identification,
extraction, and package verification also remain outside this component.
