# Frozen public inputs

## Snapshot boundary

`frozen-input/source/` contains raw blobs selected from public commit
`08abcc36c6159fbe0b98287999857d5f971bcf9d`. Each original repository path is
mapped to `frozen-input/source/<original-path>`. The inventory contains public
specification/conformance material, 33 required Core Reader fixtures, and two
optional Draft 0.1 fixtures. It has 45 files and aggregate SHA-256
`a0cecc4594a5098ea236b7cc900d27e4a7c5bd61a70cff9bcbb3dbd21cf499e5`.
The optional fixtures remain under the original path and are classified as
optional in `SOURCE-SNAPSHOT.json`.

The 33-case profile adds `valid-required-zip64-end.aural`,
`invalid-redundant-zip64-end.aural`, and
`invalid-missing-required-zip64-end.aural`. The refreshed specification and
conformance material state the strict necessity policy recorded by the
authoritative repository's design decision D-012: classic EOCD sentinels
require the ZIP64 EOCD/locator chain, that chain is forbidden without a
sentinel, and local and central ZIP64 extra fields are independently limited
to sentinel-required values in APPNOTE order.

`.gitattributes` marks every file below `frozen-input/source/` as `-text`, so
Git does not translate line endings. The freeze tool obtains each file through
`git cat-file blob`, bypassing checkout conversion entirely.

## Aggregate digest algorithm

For each frozen source file:

1. Compute SHA-256 over its exact bytes.
2. Form an ASCII/UTF-8 line containing the lowercase hexadecimal digest, two
   spaces, its forward-slash frozen path relative to `frozen-input/`, and LF.
3. Sort lines by frozen relative path using Python's ordinal string ordering.
4. Concatenate the lines. Those exact bytes are `SHA256SUMS.txt`.
5. Compute SHA-256 over the complete `SHA256SUMS.txt` byte sequence. This is the
   aggregate snapshot digest recorded in `SOURCE-SNAPSHOT.json`.

The checksum and metadata files are descriptions of the snapshot and are not
members of their own recursive digest inventory.

## Independent reproduction

From a clean checkout of this laboratory and a local clone containing the
required source commit, run:

```powershell
python tools/freeze_public_snapshot.py `
  --source-repo C:\path\to\aural-open-standard `
  --output frozen-input `
  --freeze-date 2026-10-21
```

The command verifies the commit object, reads the allowlisted paths as raw Git
blobs, writes them unchanged, rereads them byte-for-byte, and regenerates both
metadata files deterministically. A different local clone path does not enter
the metadata. Then configure/build the project and run CTest; the native
`frozen_input_checksums` test independently hashes every listed frozen file.

Changing this snapshot or its source commit requires an explicitly authorized
rebaseline. Ordinary implementation work must treat it as immutable.
