# Frozen public inputs

## Snapshot boundary

`frozen-input/source/` contains raw blobs selected from public commit
`b2b60b38989ddca9f692a6f1b7b919c5850ec11f`. Each original repository path is
mapped to `frozen-input/source/<original-path>`. The inventory contains public
specification/conformance material, 30 required Core Reader fixtures, and two
optional Draft 0.1 fixtures. The optional fixtures remain under the original
path and are classified as optional in `SOURCE-SNAPSHOT.json`.

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
  --freeze-date 2026-10-20
```

The command verifies the commit object, reads the allowlisted paths as raw Git
blobs, writes them unchanged, rereads them byte-for-byte, and regenerates both
metadata files deterministically. A different local clone path does not enter
the metadata. Then configure/build the project and run CTest; the native
`frozen_input_checksums` test independently hashes every listed frozen file.

Changing this snapshot or its source commit requires an explicitly authorized
rebaseline. Ordinary implementation work must treat it as immutable.
