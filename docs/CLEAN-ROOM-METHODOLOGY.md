# Clean-room methodology

## Authority and objective

The published AURAL specification is the sole behavioral authority for this
independent C++20 experiment. The implementation objective is eventual Core
Reader Draft 0.2 verification and Adapter Protocol 1 interoperability, not
source compatibility with any existing implementation.

The baseline is the public repository commit
`08abcc36c6159fbe0b98287999857d5f971bcf9d`. Its selected public materials are
frozen byte-for-byte under `frozen-input/source/` and may not be changed or
rebaselined without an explicit later assignment.

## Permitted inputs

- Draft 0.2 specification and published manifest schema.
- Published conformance, interoperability, and Adapter Protocol 1 documents.
- Public Core Reader case manifest, expected outcomes, golden manifest, and
  listed fixtures.
- PKWARE APPNOTE, RFC 8785, Unicode standards, and official dependency/toolchain
  documentation when later work requires them.

## Excluded inputs

- Python, Node.js, and Java verifier implementations.
- Reference or language-specific adapter implementations.
- Implementation tests, internal validation algorithms, test helpers, and
  fixture generators.
- Code from the Java interoperability laboratory.

During bootstrap, excluded source files were neither copied nor used to derive
the project design. Repository tree names were used only to identify and avoid
excluded material. The public conformance runner is also not vendored here.

## Engineering controls

1. Requirements are cited to published documents before implementation.
2. Frozen inputs retain raw Git-blob bytes; no checkout line-ending conversion
   participates in the freeze.
3. Requirement traceability distinguishes planned, implemented, and tested
   behavior. Unimplemented requirements are never marked covered.
4. New code is written from specifications and standards, with dependency use
   documented explicitly.
5. Conformance results will be reported as evidence, not certification.
