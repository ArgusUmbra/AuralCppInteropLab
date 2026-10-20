#!/usr/bin/env python3
"""Freeze the allowlisted public AURAL Core Reader inputs as raw Git blobs."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
from pathlib import Path, PurePosixPath


SOURCE_COMMIT = "b2b60b38989ddca9f692a6f1b7b919c5850ec11f"
SOURCE_URL = "https://github.com/ArgusUmbra/AuralOpenStandard.git"

DOCUMENT_PATHS = (
    "LICENSE",
    "docs/SPECIFICATION-0.2-DRAFT.md",
    "docs/manifest-0.2.schema.json",
    "docs/CONFORMANCE.md",
    "docs/CONFORMANCE-KIT.md",
    "docs/CONFORMANCE-ADAPTER-PROTOCOL-1-EXPERIMENTAL.md",
    "docs/INTEROPERABILITY.md",
    "tests/fixtures/conformance/core-reader-0.2-cases.v1.json",
    "tests/fixtures/conformance/expected.json",
    "tests/fixtures/conformance/golden-manifest.json",
)

REQUIRED_FIXTURES = (
    "invalid-altered-original.aural",
    "invalid-asset-hash.aural",
    "invalid-compression.aural",
    "invalid-critical-extension.aural",
    "invalid-duplicate-entry.aural",
    "invalid-duplicate-key.aural",
    "invalid-entry-comment.aural",
    "invalid-excessive-declared-size.aural",
    "invalid-excessive-depth.aural",
    "invalid-extra-field.aural",
    "invalid-fraction.aural",
    "invalid-local-central-crc.aural",
    "invalid-malformed-json.aural",
    "invalid-malformed-local-header.aural",
    "invalid-negative-zero.aural",
    "invalid-noncanonical.aural",
    "invalid-noncharacter.aural",
    "invalid-special-file.aural",
    "invalid-trailer.aural",
    "invalid-unknown-member.aural",
    "invalid-unnecessary-zip64-extra.aural",
    "invalid-unpaired-surrogate.aural",
    "invalid-unsafe-filename.aural",
    "invalid-unsafe-integer.aural",
    "invalid-utf8.aural",
    "invalid-version.aural",
    "valid-independent.aural",
    "valid-minimal.aural",
    "valid-unicode.aural",
    "valid-unknown-optional.aural",
)

OPTIONAL_FIXTURES = (
    "legacy-0.1-zip64-extra.aural",
    "legacy-0.1.aural",
)

FIXTURE_ROOT = "tests/fixtures/conformance"
PUBLIC_PATHS = DOCUMENT_PATHS + tuple(
    f"{FIXTURE_ROOT}/{name}" for name in REQUIRED_FIXTURES + OPTIONAL_FIXTURES
)


def git(repo: Path, *arguments: str) -> bytes:
    result = subprocess.run(
        ["git", "-C", str(repo), *arguments],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return result.stdout


def classification(source_path: str) -> str:
    name = PurePosixPath(source_path).name
    if name in OPTIONAL_FIXTURES:
        return "optional-draft-0.1-fixture"
    if name in REQUIRED_FIXTURES:
        return "required-core-reader-fixture"
    if source_path == "LICENSE":
        return "source-license"
    return "published-document-or-metadata"


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-repo", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--freeze-date", required=True)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    repository = arguments.source_repo.resolve()
    output = arguments.output.resolve()

    resolved = git(repository, "rev-parse", f"{SOURCE_COMMIT}^{{commit}}").decode().strip()
    if resolved != SOURCE_COMMIT:
        raise RuntimeError("required source commit did not resolve exactly")

    records: list[dict[str, object]] = []
    checksum_records: list[tuple[str, str]] = []
    for source_path in PUBLIC_PATHS:
        tree_entry = git(repository, "ls-tree", SOURCE_COMMIT, "--", source_path).decode()
        if not tree_entry.startswith("100644 blob "):
            raise RuntimeError(f"source path is not a regular Git blob: {source_path}")

        content = git(repository, "cat-file", "blob", f"{SOURCE_COMMIT}:{source_path}")
        frozen_path = PurePosixPath("source") / source_path
        destination = output.joinpath(*frozen_path.parts)
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(content)
        if destination.read_bytes() != content:
            raise RuntimeError(f"byte comparison failed after writing {source_path}")

        digest = hashlib.sha256(content).hexdigest()
        frozen_text = frozen_path.as_posix()
        checksum_records.append((frozen_text, digest))
        records.append(
            {
                "source_path": source_path,
                "frozen_path": frozen_text,
                "length": len(content),
                "sha256": digest,
                "classification": classification(source_path),
            }
        )

    checksum_bytes = "".join(
        f"{digest}  {path}\n" for path, digest in sorted(checksum_records)
    ).encode("utf-8")
    aggregate_digest = hashlib.sha256(checksum_bytes).hexdigest()
    (output / "SHA256SUMS.txt").write_bytes(checksum_bytes)

    metadata = {
        "schema": "org.aural.cpp-interop.frozen-source-snapshot",
        "schema_version": 1,
        "freeze_date": arguments.freeze_date,
        "source_repository": {
            "url": SOURCE_URL,
            "commit": SOURCE_COMMIT,
        },
        "path_mapping": "prefix each original repository path with source/",
        "aggregate": {
            "algorithm": (
                "SHA-256 of SHA256SUMS.txt; entries are sorted by frozen path and "
                "encoded as lowercase-digest + two spaces + forward-slash path + LF"
            ),
            "sha256": aggregate_digest,
        },
        "file_count": len(records),
        "files": sorted(records, key=lambda record: str(record["frozen_path"])),
    }
    metadata_bytes = (
        json.dumps(metadata, ensure_ascii=False, indent=2) + "\n"
    ).encode("utf-8")
    (output / "SOURCE-SNAPSHOT.json").write_bytes(metadata_bytes)

    print(f"FROZEN_FILES={len(records)}")
    print(f"AGGREGATE_SHA256={aggregate_digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
