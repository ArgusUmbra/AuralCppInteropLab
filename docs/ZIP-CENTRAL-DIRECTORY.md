# Classic ZIP central-directory inspection

## Authority and boundary

The internal component in `src/core/zip/` is derived from AURAL Draft 0.2
sections 6.1-6.6 and 7, the frozen conformance and interoperability documents,
and PKWARE APPNOTE 6.3.10 sections 4.3.12-4.3.16 and 4.4. It opens one path
through `FileReader`, resolves the classic or ZIP64 end-record chain, and
inspects the central-directory records it declares.

A successful result means only that this bounded classic structure is
self-consistent. It is not an AURAL package-verification result.

## Records and fields

EOCD discovery searches backward through at most 65,557 bytes: the 22-byte
fixed EOCD plus the format's maximum 65,535-byte comment. A candidate is real
only when its fixed fields are present and its declared comment ends exactly at
physical EOF. Later false signatures are ignored while searching for that
terminal boundary. Bytes after a complete candidate and truncated candidate
records are reported separately.

The classic EOCD fields inspected are disk numbers, on-disk and total entry
counts, central-directory size and offset, and archive-comment length. Every
central file header retains:

- physical header offset;
- versions made-by and needed;
- general-purpose flags and compression method;
- MS-DOS modification time and date;
- declared CRC32, compressed size, and uncompressed size;
- internal and external attributes;
- declared local-header offset;
- exact filename octets, including embedded NUL or non-UTF-8 bytes; and
- extra-field and entry-comment lengths.

All multibyte values are decoded as unsigned little-endian values. Fixed and
variable record regions must fit wholly inside the declared directory. The
parsed record count must equal both resolved single-disk counts. The directory
must fit the physical file and end exactly where the applicable classic or
ZIP64 end-record chain begins. A declared local-header fixed range must end
before the directory, but its bytes
are deliberately not read or validated in this milestone.

## AURAL limits and ordinary ZIP features

The default resource policy is the frozen AURAL ceiling: 8,606,711,808
physical bytes, 4,194,304 central-directory bytes, and at most two entries.
Limits are checked before archive-controlled allocation. The EOCD search buffer
is at most 65,557 bytes. The central-directory allocation is at most the checked
directory ceiling, and filename allocations are made only from already bounded
record lengths. Entry storage is reserved only after the count ceiling passes.

Single-disk structure is enforced now because split and spanned disks are
outside the frozen profile. Classic EOCD sentinels are resolved through the
required ZIP64 EOCD and locator. A sentinel in an entry size, disk, or local
offset field produces `zip64_entry_resolution_pending`, never a partially
valid directory. See `ZIP64-END-RECORDS.md` for the envelope rules.

Ordinary ZIP metadata that AURAL later prohibits is retained rather than
treated as structural corruption here. That includes non-stored compression,
general-purpose flags, arbitrary filename bytes, extra fields, and comments.
Later package-profile validation must require the two exact AURAL names,
method 0, allowed flags and ZIP64 extras, empty comments, regular-file
attributes, and the exact two-entry inventory.

## Error and ownership model

Expected outcomes are separated into structurally valid, invalid structure,
ZIP64 entry resolution pending, resource failure, and file failure. Structural
and ZIP64 details carry the physical offset of the responsible record or field.
Native file details remain nested `FileError` values. Allocation failure is
reported without a partial directory.

The result owns entry metadata and filename bytes. `FileReader` owns and closes
the read-only handle. Inspection never writes the input and returns no borrowed
views into temporary I/O buffers.

## Explicitly unsupported

This milestone does not validate local-header signatures or equality, entry
payload boundaries, contiguity of complete local records, entry data, CRC32,
hashes, manifest agreement, WAVE identity, decompression, extraction, ZIP64
entry extras, prefixes, or complete AURAL package validity. These remain
separate verification stages.
